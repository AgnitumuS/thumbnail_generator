/*
 * Copyright (c) 2012 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * Demuxing and decoding example.
 *
 * Show how to use the libavformat and libavcodec API to demux and
 * decode audio and video data.
 * @example demuxing_decoding.c
 */

#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>

#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
//#include "libavfilter/lavfutils.h"
int ff_load_image(uint8_t *data[4], int linesize[4], int *w, int *h, enum AVPixelFormat *pix_fmt, const char *filename, void *log_ctx);
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <libavutil/avassert.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <x264.h>
#include <pthread.h>
#include <libxml/xmlreader.h>
#include <sys/time.h>

#if _POSIX_C_SOURCE >= 199309L
#include <time.h>
#endif

#include "font.h"

#define USE_XML                 1
#define FULL_TASK_RUN           (!inputSource->quit && !stop_all_tasks)
#define GET_OUTPUT_SETTINGS     OutputInfo *outputSettings = outputMosaics[0];

#define STREAM_DURATION         0                 // any other value runs for that amount of time.
#define STREAM_PIX_FMT          AV_PIX_FMT_YUV420P /* default pix_fmt */
#define SCALE_FLAGS             SWS_BICUBIC

#define USE_PACKETS_LIST        1

#define YCrCb_BLACK             0x108080
#define YCrCb_BLACK_S           "108080"
#define YCrCb_WHITE             0xea8080
#define YCrCb_GREY              0x7d8080
#define YCrCb_RED               0x4c54ff
#define YCrCb_BLUE              0x1dff6b
#define YCrCb_GREEN             0x952b15

#define IS_TILE_nF_FnC(X)       ( 1)
#define IS_TILE_FnC(X)          ( 1)
#define IS_TILE_F(X)            ( 1)
#define IS_TILE_VIDEO(X)        ( 1)

typedef int                     TILE_MAP; 

enum {
    VIDEO_INDEX,
    AUDIO_INDEX,
    MAX_INDEX
};

volatile int stop_all_tasks;

typedef struct _videoFrames {
    void *next;
    AVFrame *frame;
    double pts;
} videoFrames;

// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st;

    AVFrame *video_frame;
    AVFrame *tmp_video_frame;

    AVFrame *final_video_frame;
    int final_width;
    int final_height;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;

    float t, tincr, tincr2;

    struct SwsContext *out_sws_ctx;
    struct SwsContext *final_sws_ctx;
} OutputStream;

typedef struct _Tiles {
    int x;
    int y;
    int w;
    int h;
    int frames;
    int index;
    int fixed;
    int want_audio;

    uint8_t *video_dst_data[4];
    int      video_dst_linesize[4];
    int      video_dst_bufsize;
    int      video_dst_dirty;

    int updates_per_second;
} Tiles;

typedef struct _inputMosaic {
    pthread_t pulledThread;

    pthread_mutex_t av_mutex;
    pthread_t videoThread;

    AVFormatContext *fmt_ctx;

    pthread_mutex_t list_mutex;
    videoFrames *packets_list[MAX_INDEX];
    double pts[ MAX_INDEX];

    AVCodecContext *video_dec_ctx;
    AVStream *video_stream;
    int video_stream_idx;
    int video_frame_count;

#define MAX_TILES_PER_INPUT 1
    // Rescalers
    struct SwsContext *scale_sws_ctx[MAX_TILES_PER_INPUT];
    struct SwrContext *scale_swr_ctx[MAX_TILES_PER_INPUT];
    int tile_number[MAX_TILES_PER_INPUT];
    int tx[MAX_TILES_PER_INPUT], ty[MAX_TILES_PER_INPUT], tw[MAX_TILES_PER_INPUT], th[MAX_TILES_PER_INPUT];

    AVPacket pkt;

    int skip;
    int running;
    int quit;
    int finished;

    // Options needed
    char *name;
    char *src_filename;
    int adult;
    int skip_frames;
    float fps;

    char *artist;
    char *album;
    int year;

} inputMosaic;

typedef struct _OutputInfo {
    AVFormatContext *oc;
    OutputStream video_st;
    AVCodec *video_codec;

    pthread_t threadMain;
    pthread_t outputThread;
    pthread_mutex_t tile_mutex;

    int mode;

    int video_encoding;
    int video_bitrate;
    int video_frame_rate;
    int gop_size;
    const char *x264_preset;
    const char *x264_threads;

    int screen_width;
    int screen_height;
    int final_width;
    int final_height;
    int border_width;
    uint8_t fillColourY, fillColourCb, fillColourCr;

    int audio_encoding;
    int audio_bit_rate;
    int audio_channels;
    int audio_sample_rate;
    int audio_format;
    int need_audio;

    int frames_count;
    int thumbnail_count;

    char *filename;
    char *background;
    AVFrame *background_frame;

    int tiles_count;
    Tiles **tiles;
    TILE_MAP *tile_map;
    int tiles_across;
    int tiles_down;

    pthread_mutex_t buffer_mutex;
} OutputInfo;

static OutputInfo  **outputMosaics;
static int          outputMosaicsCnt;

static int         inputs_count;
static inputMosaic **inputs;

static int         verbose;
static int         encoded_frames;
static volatile int frame_ready;

/* The different ways of decoding and managing data memory. You are not
 * supposed to support all the modes in your application but pick the one most
 * appropriate to your needs. Look for the use of api_mode in this example to
 * see what are the differences of API usage between them */
enum {
    API_MODE_OLD                  = 0, /* old method, deprecated */
    API_MODE_NEW_API_REF_COUNT    = 1, /* new method, using the frame reference counting */
    API_MODE_NEW_API_NO_REF_COUNT = 2, /* new method, without reference counting */
};

#define API_MODE    1       // API_MODE_NEW_API_REF_COUNT

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt);

typedef struct _codecList {
    const char *name;
    int id;
} codecList;

static codecList videoCodecs[] = {
    { "MPEG1",  AV_CODEC_ID_MPEG1VIDEO },
    { "MPEG2",  AV_CODEC_ID_MPEG2VIDEO },
    { "MPEG4",  AV_CODEC_ID_MPEG4 },
    { "H264",   AV_CODEC_ID_H264 },
    { "H265",   AV_CODEC_ID_H265 },
    { "HEVC",   AV_CODEC_ID_HEVC },
    { NULL,     AV_CODEC_ID_H264 }
};

static codecList audioCodecs[] = {
    { "AAC",    AV_CODEC_ID_AAC },
    { "AC3",    AV_CODEC_ID_AC3 },
    { "MP2",    AV_CODEC_ID_MP2 },
    { "MP3",    AV_CODEC_ID_MP3 },
    { NULL,     AV_CODEC_ID_AAC }
};

static codecList audioFormats[] = {
    { "AV_SAMPLE_FMT_S16", AV_SAMPLE_FMT_S16 },
//    { "AV_SAMPLE_FMT_FLTP", AV_SAMPLE_FMT_FLTP },
    { NULL, AV_SAMPLE_FMT_S16 }
};

static const char *controlStrings[] = { "verbose", "save_input", "save_output", NULL };
#define NUMBER_OF_CONTROLS  (sizeof(controlStrings)/sizeof(char *))

enum { MODE_THUMBNAIL };
static const char *mosaicsStrings[] = { "size", "url", "frame_count", "video_bitrate", "video_framerate", "gop_size", "x264_preset", "audio_bitrate", "x264_threads", "border", "video_encoding", "audio_encoding", "fill_colour", "mode", "final_size", "tiles_across", "tiles_down", NULL };
#define NUMBER_OF_MOSAICS   (sizeof(mosaicsStrings)/sizeof(char *))
static const char *tileStrings[]    = { "position", "fixed", "map", "audio", "frames", "vu_meter", "index", "clock", "analog", "named", "popup", NULL };
#define NUMBER_OF_TILES     (sizeof(tileStrings)/sizeof(char *))
static const char *streamStrings[]  = { "name", "url", "adult", "skip", "artist", "album", "year", "fps", NULL };
#define NUMBER_OF_STREAMS   (sizeof(streamStrings)/sizeof(char *))

static void signal_handler( int no )
{
    stop_all_tasks = 1;
}

static void stradd( char *string, const char *addition)
{
    if( string) {
        while( *string)
            string++;
        strcpy( string, addition);
    }
}

static int localGetId( const char *codec_name, codecList *whichList)
{
    while( whichList->name) {
        if( !strcmp( whichList->name, codec_name))
            return whichList->id;
        whichList++;
    }

    return whichList->id;
}

static int localFindString( const char *string, const char **strings)
{
int index;

    for( index=0; strings[index]; index++) {
        if( !strcmp( string, strings[index])) {
            return index;
        }
    }

    return -1;
}

static Tiles *localFindTile( int tileNumber)
{
int o;
int t;

    for(o=0; o<outputMosaicsCnt; o++) {
    OutputInfo *output = outputMosaics[o];

        for(t=0;t<output->tiles_count;t++) {
            if( output->tile_map[t]==tileNumber) {
                return output->tiles[t];
            }
        }
    }

    return NULL;
}

enum {
    LEVEL_TV,
    LEVEL_CONTROL_OUTPUT_INPUTS_PLAYLIST,
    LEVEL_CONTROL,
    LEVEL_OUTPUT,
    LEVEL_INPUTS,
};

#if defined( LIBXML_DOTTED_VERSION)
static void print_element_names(xmlNode * a_node, char *xmlUrl, int level)
{
    xmlNode *cur_node = NULL;

    for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
        if (cur_node->type == XML_ELEMENT_NODE) {
            switch( level)
            {
                case LEVEL_TV: // TV
                    if( strcmp( (char *)cur_node->name, "MosaicControl")) {
                        printf( "%s, NOT A VALID XML FILE, MISSING 'MosaicControl' element\n", xmlUrl);
                        return;
                    }
                    if( cur_node->children) {
                        print_element_names(cur_node->children, xmlUrl, LEVEL_CONTROL_OUTPUT_INPUTS_PLAYLIST);
                    }
                    break;

                case LEVEL_CONTROL_OUTPUT_INPUTS_PLAYLIST:
                    if( !strcmp( (char *)cur_node->name, "Control")) {
                    xmlAttr *attr;

                        attr = cur_node->properties;
                        while( attr) {
                        xmlNode *values = attr->children;
                        int index = localFindString( (char *)attr->name, controlStrings);

                            switch( index) {
                                case 0:
                                    verbose = atoi( (char *)values->content);
                                    break;

                                case 1:
//                                outputSettings.save_input = atoi( values);
                                    break;

                                case 2:
//                                outputSettings.save_output = atoi( values);
                                    break;

                                default:
                                    printf( "Control->%s\n", attr->name);
                                    break;
                            }
                            attr = attr->next;
                        }
                        if( cur_node->children) {
                            print_element_names(cur_node->children, xmlUrl, LEVEL_CONTROL);
                        }
                    }
                    else if( !strcmp( (char *)cur_node->name, "Output")) {
                    xmlAttr *attr;

                        attr = cur_node->properties;
                        while( attr) {

                            printf( "Output->%s\n", attr->name);
                            attr = attr->next;
                        }
                        if( cur_node->children) {
                            print_element_names(cur_node->children, xmlUrl, LEVEL_OUTPUT);
                        }
                    }
                    else if( !strcmp( (char *)cur_node->name, "Inputs")) {
                    xmlAttr *attr;

                        attr = cur_node->properties;
                        while( attr) {

                            printf( "Inputs->%s\n", attr->name);
                            attr = attr->next;
                        }
                        if( cur_node->children) {
                            print_element_names(cur_node->children, xmlUrl, LEVEL_INPUTS);
                        }
                    }
                    break;

                case LEVEL_OUTPUT:
                    if( !strcmp( (char *)cur_node->name, "mosaic")) {
                    xmlAttr *attr;
                    char *vals[NUMBER_OF_MOSAICS] = { NULL, NULL, "1", NULL, NULL, NULL, NULL, NULL, "auto", "0", "H264", "AAC", "#" YCrCb_BLACK_S, "A", NULL, "3", "3"};
                    int mask = 0;

                        // defaults
                        attr = cur_node->properties;
                        while( attr) {
                        xmlNode *values = attr->children;
                        int index = localFindString( (char *)attr->name, mosaicsStrings);

                            if( index>-1) {
                                vals[index] = (char *)values->content;
                                mask |= (1<<index);
                            }
                            else {
                                printf( "mosaic->%s\n", attr->name);
                            }
                            attr = attr->next;
                        }
                          mask &= 1+2+8+16+32+64+128;
                        if (mask==1+2+8+16+32+64+128) {
                        OutputInfo *outputSettings;

                            if( !outputMosaics) {
                                outputMosaics = calloc(1 , sizeof( OutputInfo *));
                                outputMosaicsCnt = 0;
                            }
                            else {
                                outputMosaics = realloc( outputMosaics, (outputMosaicsCnt+1)*sizeof( OutputInfo *));
                            }
                            outputSettings = outputMosaics[ outputMosaicsCnt] = calloc( 1, sizeof(OutputInfo));
                            outputMosaicsCnt++;
                            if( sscanf( vals[0], "%d,%d", &outputSettings->screen_width, &outputSettings->screen_height)==2) {
                            char codecFormat[64];

                                outputSettings->filename               = strdup( vals[1]);
                                outputSettings->frames_count           = atoi( vals[2]);    if( !outputSettings->frames_count) outputSettings->frames_count++;
                                outputSettings->video_bitrate          = atoi( vals[3]);
                                outputSettings->video_frame_rate       = atoi( vals[4]);
                                outputSettings->gop_size               = atoi( vals[5]);
                                outputSettings->x264_preset            = strdup( vals[6]);
                                if( sscanf( vals[7], "%d,%d,%d,%s", &outputSettings->audio_bit_rate, &outputSettings->audio_channels, &outputSettings->audio_sample_rate, &codecFormat[0])>=3) {
                                    outputSettings->audio_format       = localGetId( codecFormat, audioFormats);
                                    outputSettings->x264_threads       = strdup( vals[8]);
                                    outputSettings->border_width       = atoi( vals[9]);
                                    outputSettings->video_encoding     = localGetId( vals[10], videoCodecs);
                                    outputSettings->audio_encoding     = localGetId( vals[11], audioCodecs);
                                    if( vals[12][0]=='#') {
                                    int col;

                                        if(sscanf(vals[12]+1, "%06x", &col)!=1) {
                                            printf( "-->error %s\n", vals[12]);
                                            col = YCrCb_BLACK;
                                        }
                                        outputSettings->fillColourY  = col>>16;
                                        outputSettings->fillColourCb = col>>8;
                                        outputSettings->fillColourCr = col;
                                    }
                                    else {
                                        outputSettings->background      = strdup( vals[12]);
                                    }
                                    if( vals[13][0]=='A') {
                                        outputSettings->mode = -1;
                                    }
                                    else if( vals[13][0]=='K') {
                                        outputSettings->mode = -2;
                                    }
                                    else if( isdigit( vals[13][0])) {
                                        outputSettings->mode = atoi( vals[13]);
                                        if( !outputSettings->mode)
                                            outputSettings->mode = 1;
                                        else if( outputSettings->mode>50)
                                            outputSettings->mode = 50;
                                    }
                                    if( vals[14]) {
                                    int w, h;

                                        if( sscanf( vals[14], "%d,%d", &w, &h)==2) {
                                            outputSettings->final_width        = w;
                                            outputSettings->final_height       = h;
                                        }
                                        else {
                                            printf( "-->error %s\n", vals[14]);
                    					}
                                    }
                                    else {
                                        outputSettings->final_width        = 0;
                                        outputSettings->final_height       = 0;
        				            }
                                    if( outputSettings->audio_channels!=2 || localGetId( codecFormat, audioFormats)!=AV_SAMPLE_FMT_S16) {
                                        printf( "**** CAN ONLY HAVE S16 and 2 Channels\r\n");
                                        outputSettings->audio_channels = 2;
                                        outputSettings->audio_format = AV_SAMPLE_FMT_S16;
                                    }
                                    outputSettings->tiles_across = atoi( vals[15]);
                                    outputSettings->tiles_down = atoi( vals[16]);
                                    if( verbose) {
                                        printf( "%4d,%4d '%s' %5d %5d %2d %2d '%s' %5d %5d %s\n", outputSettings->screen_width, outputSettings->screen_height,
                                            outputSettings->filename, outputSettings->frames_count, outputSettings->video_bitrate,
                                            outputSettings->video_frame_rate, outputSettings->gop_size, outputSettings->x264_preset,
                                            outputSettings->audio_bit_rate, outputSettings->audio_sample_rate, codecFormat);
                                    }
                                }
                                else {
                                    printf( "-->error %s\n", vals[7]);
                                }
                            }
                            else {
                                printf( "-->error %s\n", vals[0]);
                            }
                        }
                        else {
                            printf( "mosaic:%d '%s' '%s' '%s' '%s' '%s' '%s' '%s' '%s'\n", mask, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7]);
                        }
                    }
#if 0
                    else if( !strcmp( (char *)cur_node->name, "stream")) {
                    xmlAttr *attr;
                    char *vals[NUMBER_OF_STREAMS] = { NULL, NULL, "0", "0", "", "", "", "25.00" };
                    int mask = 0;

                        attr = cur_node->properties;
                        while( attr) {
                        xmlNode *values = attr->children;
                        int index = localFindString( (char *)attr->name, streamStrings);

                            if( index>-1) {
                                vals[index] = (char *)values->content;
                                mask |= (1<<index);
                            }
                            else {
                                printf( "stream->%s\n", attr->name);
                            }
                            attr = attr->next;
                        }
                        mask &= 1+2;
                        if (mask==1+2) {
                            outputSettings->inputs = realloc( outputSettings->inputs, sizeof( inputMosaic *)*(outputSettings->inputs_count+1));
                            outputSettings->inputs[ outputSettings->inputs_count] = calloc( 1, sizeof( inputMosaic));
                            outputSettings->inputs[ outputSettings->inputs_count]->name = strdup( vals[0]);
                            outputSettings->inputs[ outputSettings->inputs_count]->src_filename = malloc( strlen( vals[1])+128);
                            strcpy( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, vals[1]);
                            if( !memcmp( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "udp", 3)) {
                            char *sep = "?";

                                if( strchr( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, '?'))
                                    sep = "&";
                                if( !strstr( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "reuse")) {
                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, sep);
                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "reuse");
                                    sep = "&";
                                }
                                if( !strstr( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "fifo_size")) {
                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, sep);
                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "fifo_size=50000");
//                                    sep = "&";
                                }
//                                sep = sep;
//                                if( !strstr( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "buffer_size")) {
//                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, sep);
//                                    stradd( outputSettings->inputs[ outputSettings->inputs_count]->src_filename, "buffer_size=1M");
//                                    sep = "&";
//                                }
                            }
                            outputSettings->inputs[ outputSettings->inputs_count]->adult        = atoi( vals[2]);
                            outputSettings->inputs[ outputSettings->inputs_count]->skip_frames  = atoi( vals[3]);
                            outputSettings->inputs[ outputSettings->inputs_count]->artist       = NULL;
                            outputSettings->inputs[ outputSettings->inputs_count]->album        = NULL;
                            outputSettings->inputs[ outputSettings->inputs_count]->year         = 2015;
                            outputSettings->inputs[ outputSettings->inputs_count]->fps          = atof( vals[7]);
                            if( verbose) {
                                printf( "%d. '%s' '%s A:%d\n", outputSettings->inputs_count, outputSettings->inputs[ outputSettings->inputs_count]->name,
                                    outputSettings->inputs[ outputSettings->inputs_count]->src_filename, outputSettings->inputs[ outputSettings->inputs_count]->adult);
                            }
                            outputSettings->inputs_count++;
                        }
                        else {
                            printf( "stream:%d '%s' '%s' '%s'\n", mask, vals[0], vals[1], vals[2]);
                        }
                    }
#endif
                    else if( !strcmp( (char *)cur_node->name, "tile")) {
                    xmlAttr *attr;
                    char *vals[NUMBER_OF_TILES] = { NULL, "0", NULL, "0", "K", "0", "0", "0", "0", NULL, NULL };
                    int mask = 0;

                        attr = cur_node->properties;
                        while( attr) {
                        xmlNode *values = attr->children;
                        int index = localFindString( (char *)attr->name, tileStrings);

                            if( index>-1) {
                                vals[index] = (char *)values->content;
                                mask |= (1<<index);
                            }
                            else {
                                printf( "tile->%s\n", attr->name);
                            }
                            attr = attr->next;
                        }
                          mask &= 1+4;
                        if (mask==1+4) {
                        int x, y, w, h;
                        OutputInfo *outputSettings;

                            if( outputMosaicsCnt) {
                                outputSettings = outputMosaics[outputMosaicsCnt-1];
                                if( sscanf( vals[0], "%d,%d,%d,%d", &x, &y, &w, &h)==4) {
                                int popstart=0, poplength=0;

                                    if( !vals[10] || sscanf( vals[10], "%d,%d", &popstart, &poplength)==2) {
                                        outputSettings->tiles    = realloc( outputSettings->tiles, (outputSettings->tiles_count+1)*sizeof( Tiles *));
                                        outputSettings->tile_map = realloc( outputSettings->tile_map, (outputSettings->tiles_count+1)*sizeof( TILE_MAP));
                                        outputSettings->tiles[ outputSettings->tiles_count]             = calloc( 1, sizeof( Tiles));
                                        outputSettings->tiles[ outputSettings->tiles_count]->x          = x + (outputSettings->border_width/2);
                                        outputSettings->tiles[ outputSettings->tiles_count]->y          = y + (outputSettings->border_width/2);
                                        outputSettings->tiles[ outputSettings->tiles_count]->w          = w - (outputSettings->border_width&~1);
                                        outputSettings->tiles[ outputSettings->tiles_count]->h          = h - (outputSettings->border_width&~1);
                                        outputSettings->tiles[ outputSettings->tiles_count]->fixed      = atoi( vals[1]);
                                        outputSettings->tile_map[ outputSettings->tiles_count]          = atoi( vals[2]);
                                        outputSettings->tiles[ outputSettings->tiles_count]->want_audio = atoi( vals[3]);
                                        outputSettings->tiles[ outputSettings->tiles_count]->frames     =       vals[4][0];
                                        outputSettings->tiles_count++;
                                    }
                                }
                            }
                            else {
                                printf( "No OUTPUT defined\r\n");
                            }
                        }
                        else {
                            printf( "tile:%d '%s' '%s' '%s'\n", mask, vals[0], vals[1], vals[2]);
                        }
                    }
                    else {
                        printf( "output:%s\n", cur_node->name);
                    }
                    break;

                case LEVEL_INPUTS:
                    if( !strcmp( (char *)cur_node->name, "stream")) {
                    xmlAttr *attr;
                    char *vals[NUMBER_OF_STREAMS] = { NULL, NULL, "0", "0", "", "", "", "25.00" };
                    int mask = 0;

                        attr = cur_node->properties;
                        while( attr) {
                        xmlNode *values = attr->children;
                        int index = localFindString( (char *)attr->name, streamStrings);

                            if( index>-1) {
                                vals[index] = (char *)values->content;
                                mask |= (1<<index);
                            }
                            else {
                                printf( "stream->%s\n", attr->name);
                            }
                            attr = attr->next;
                        }
                        mask &= 1+2;
                        if (mask==1+2) {
                            inputs = realloc( inputs, sizeof( inputMosaic *)*(inputs_count+1));
                            inputs[ inputs_count] = calloc( 1, sizeof( inputMosaic));
                            inputs[ inputs_count]->name = strdup( vals[0]);
                            inputs[ inputs_count]->src_filename = malloc( strlen( vals[1])+128);
                            strcpy( inputs[ inputs_count]->src_filename, vals[1]);
                            if( !memcmp( inputs[ inputs_count]->src_filename, "udp", 3)) {
                            char *sep = "?";

                                if( strchr( inputs[ inputs_count]->src_filename, '?'))
                                    sep = "&";
                                if( !strstr( inputs[ inputs_count]->src_filename, "reuse")) {
                                    stradd( inputs[ inputs_count]->src_filename, sep);
                                    stradd( inputs[ inputs_count]->src_filename, "reuse");
                                    sep = "&";
                                }
                                if( !strstr( inputs[ inputs_count]->src_filename, "fifo_size")) {
                                    stradd( inputs[ inputs_count]->src_filename, sep);
                                    stradd( inputs[ inputs_count]->src_filename, "fifo_size=50000");
//                                    sep = "&";
                                }
//                                sep = sep;
//                                if( !strstr( inputs[ inputs_count]->src_filename, "buffer_size")) {
//                                    stradd( inputs[ inputs_count]->src_filename, sep);
//                                    stradd( inputs[ inputs_count]->src_filename, "buffer_size=1M");
//                                    sep = "&";
//                                }
                            }
                            inputs[ inputs_count]->adult        = atoi( vals[2]);
                            inputs[ inputs_count]->skip_frames  = atoi( vals[3]);
                            inputs[ inputs_count]->artist       = NULL;
                            inputs[ inputs_count]->album        = NULL;
                            inputs[ inputs_count]->year         = 2015;
                            inputs[ inputs_count]->fps          = atof( vals[7]);
                            if( verbose) {
                                printf( "%d. '%s' '%s A:%d\n", inputs_count, inputs[ inputs_count]->name,
                                    inputs[ inputs_count]->src_filename, inputs[ inputs_count]->adult);
                            }
                            inputs_count++;
                        }
                        else {
                            printf( "stream:%d '%s' '%s' '%s'\n", mask, vals[0], vals[1], vals[2]);
                        }
                    }
                    else {
                        printf( "inputs:%s\n", cur_node->name);
                    }
                    break;

                default:
                    printf("node type: %d Element, name: %s\n", level, cur_node->name);
                    break;
            }
        }
    }
}
#endif

static void localClearTile( Tiles *tile, int colour, AVFrame *pict)
{
int y;
uint8_t *Y  = pict->data[0] + tile->x     + (tile->y*pict->linesize[0]);
uint8_t *Cr = pict->data[1] + (tile->x/2) + ((tile->y/2)*pict->linesize[1]);
uint8_t *Cb = pict->data[2] + (tile->x/2) + ((tile->y/2)*pict->linesize[2]);

    for( y=0; y<tile->h; ) {
        memset( Y, colour>>16, tile->w);
        memset( Cr, colour>>8, tile->w/2);
        memset( Cb, colour,    tile->w/2);
        Y  += pict->linesize[0];
        y++;
        if( !(y&1)) {
            Cr += pict->linesize[1];
            Cb += pict->linesize[2];
        }
    }
}

static void localClearArea( Tiles *tile, int x0, int y0, int w0, int h0, int colour, AVFrame *pict)
{
int y;
//uint8_t *Y  = pict->data[0] + x0 + tile->x + ((tile->y+y0)*pict->linesize[0]);
//uint8_t *Cr = pict->data[1] + (x0+tile->x)/2 + (((tile->y+y0)/2)*pict->linesize[1]);
//uint8_t *Cb = pict->data[2] + (x0+tile->x)/2 + (((tile->y+y0)/2)*pict->linesize[2]);
uint8_t *Y  = pict->data[0] + x0 + tile->x + ((tile->y+y0)*pict->linesize[0]);
uint8_t *Cr = pict->data[1] + (x0+tile->x)/2 + (((tile->y+y0)/2)*pict->linesize[1]);
uint8_t *Cb = pict->data[2] + (x0+tile->x)/2 + (((tile->y+y0)/2)*pict->linesize[2]);

    for( y=0; y<h0; ) {
        memset( Y, colour>>16, w0);
        memset( Cr, colour>>8, w0/2);
        memset( Cb, colour,    w0/2);
        y++;
        Y += pict->linesize[0];
        if( !(y&1)) {
            Cr += pict->linesize[1];
            Cb += pict->linesize[2];
        }
    }
}

static void localDrawString( Tiles *tile, int x0, int y0, const char *string, int colour, AVFrame *pict)
{
uint8_t *Y, *Cr, *Cb;
int  height = (anonymousPro_9ptDescriptors[1].offset - anonymousPro_9ptDescriptors[0].offset)+1;
int bw = 0;
int bh = 0;

    if( !string)
        return;

    if( x0<0) {
    const char *s = string;
    int w = 0;

        while( *s) {
        int off = *s;
        int width = 0;

            if(off>=anonymousPro_9ptFontInfo.start_character && off<=anonymousPro_9ptFontInfo.end_character) {
                off -= anonymousPro_9ptFontInfo.start_character;
                width = anonymousPro_9ptDescriptors[off].width;
            }
            w += width+2;
            s++;
        }
        if( x0==-1) {
            x0 = (tile->w-w)/2;
        }
        else if( x0==-2) {
            x0 = 0;
            bw = w;
        }
    }
    if( y0==-1) {
        y0 = (tile->h-height)/2;
    }
    else if( y0==-2) {
        y0 = 0;
        bh = height;
    }
    if( bh && bw) {
        localClearArea( tile, x0, y0, bw, bh, YCrCb_GREY /*YCrCb_BLACK*/, pict);
    }
    Y  = pict->data[0] + ((y0+tile->y)/1*pict->linesize[0]);
    Cr = pict->data[1] + ((y0+tile->y)/2*pict->linesize[1]);
    Cb = pict->data[2] + ((y0+tile->y)/2*pict->linesize[2]);
    while( *string) {
    int off = *string++;
    int width = 0;
    uint8_t *_Y  = Y;
    uint8_t *_Cr = Cr;
    uint8_t *_Cb = Cb;

        if(off>=anonymousPro_9ptFontInfo.start_character && off<=anonymousPro_9ptFontInfo.end_character) {
        const uint8_t *data = anonymousPro_9ptBitmaps+anonymousPro_9ptDescriptors[off-anonymousPro_9ptFontInfo.start_character].offset;
        int h = height;
        int yy = 0;

            width = anonymousPro_9ptDescriptors[off-anonymousPro_9ptFontInfo.start_character].width;
            while( h--) {
            int w = width;
            short mask = *data++<<8;
            int pos = tile->x+x0;

                if(w>7) {
                     mask |= *data++;
                }
                while( w) {
                    if( mask & 0x8000) {
                        _Y[pos]    = colour>>16;
                        _Cr[pos/2] = colour>>8;
                        _Cb[pos/2] = colour;
                    }
                    mask <<= 1;
                    pos++;
                    w--;
                }
                _Y   += pict->linesize[0];
                yy++;
                if( !(yy&1)) {
                    _Cr  += pict->linesize[1];
                    _Cb  += pict->linesize[2];
                }
            }
        }
        x0  += width+2;
    }
}

static void localBlockFill( Tiles *tile, int width, int colour)
{
int y;
uint8_t *Y   = tile->video_dst_data[0];
uint8_t *Y1  = tile->video_dst_data[0] + ((tile->h-width)*tile->video_dst_linesize[0]);
uint8_t *Cr  = tile->video_dst_data[1];
uint8_t *Cr1 = tile->video_dst_data[1] + ((tile->h-width)/2*tile->video_dst_linesize[1]);
uint8_t *Cb  = tile->video_dst_data[2];
uint8_t *Cb1 = tile->video_dst_data[2] + ((tile->h-width)/2*tile->video_dst_linesize[2]);

    for( y=0; y<width; y++) {
        memset( Y, colour>>16, tile->w);
        Y += tile->video_dst_linesize[0];
        memset( Y1, colour>>16, tile->w);
        Y1 += tile->video_dst_linesize[0];
    }
    for( y=0; y<width/2; y++) {
        memset( Cr, colour>>8, tile->w/2);
        Cr += tile->video_dst_linesize[1];
        memset( Cr1, colour>>8, tile->w/2);
        Cr1 += tile->video_dst_linesize[1];
        memset( Cb, colour, tile->w/2);
        Cb += tile->video_dst_linesize[2];
        memset( Cb1, colour, tile->w/2);
        Cb1 += tile->video_dst_linesize[2];
    }

    Y = tile->video_dst_data[0];
    Cr = tile->video_dst_data[1];
    Cb = tile->video_dst_data[2];
    for( y=0; y<tile->h; y++) {
        memset( Y, colour>>16, width);
//        memset( Y + tile->w - width, colour>>16, width);
        Y += tile->video_dst_linesize[0];
    }
    for( y=0; y<tile->h/2; y++) {
        memset( Cr, colour>>8, width/2);
//        memset( Cr + (tile->w - width)/2, colour>>8, width);
        Cr += tile->video_dst_linesize[1];
        memset( Cb, colour, width/2);
//        memset( Cb + (tile->w - width)/2, colour, width);
        Cb += tile->video_dst_linesize[2];
    }
}

static int localNumberOfPackets( inputMosaic *inputSource, const int index)
{
videoFrames *here;
int cnt = 0;

    pthread_mutex_lock( &inputSource->list_mutex);
    here = inputSource->packets_list[index];
    while( here) {
        cnt++;
        here = here->next;
    }
    pthread_mutex_unlock( &inputSource->list_mutex);

    return cnt;
}

static int localClearPackets( inputMosaic *inputSource, const int index)
{
int cnt = 0;

    pthread_mutex_lock( &inputSource->list_mutex);
    while( inputSource->packets_list[index]) {
    videoFrames *here = inputSource->packets_list[index];

        inputSource->packets_list[index] = here->next;
#if !API_MODE
        avcodec_free_frame(&here->frame);
#else
        av_frame_free(&here->frame);
#endif
        free( here);
        cnt++;
    }
    pthread_mutex_unlock( &inputSource->list_mutex);

    return cnt;
}

#define TIMEOFDAY(X)    ((X.tv_sec * 1000000) + X.tv_usec)
#define TIMEOFDAY_S     (1000000)

static void *inputThreadVideo( void *_whichSource)
{
GET_OUTPUT_SETTINGS;
inputMosaic *inputSource = _whichSource;
int skip = inputSource->skip_frames;
int sw = -1, sh = -1;

    while( FULL_TASK_RUN) {
    int cnt = localNumberOfPackets( inputSource, VIDEO_INDEX);

        if( frame_ready) {
            usleep(100);
        } 
        else if( cnt) { // >inputSource->video_dec_ctx->delay) {
            if( skip) {
            int ret = localClearPackets( inputSource, VIDEO_INDEX);

                skip -= FFMIN( skip, ret);
            }
            else {
            videoFrames *here;
            AVFrame *frame;
            int t;

                pthread_mutex_lock( &inputSource->list_mutex);
                here  = inputSource->packets_list[VIDEO_INDEX];
                inputSource->packets_list[VIDEO_INDEX] = here->next;
                frame = here->frame;
                pthread_mutex_unlock( &inputSource->list_mutex);

                if (frame->key_frame) {
                    if (verbose) {
                        printf("%s video_frame n:%d coded_n:%d pts:%s %f %c\n",
                               inputSource->name, inputSource->video_frame_count, frame->display_picture_number,
                                av_ts2timestr(frame->pts, &inputSource->video_dec_ctx->time_base), here->pts,
                                frame->key_frame ? 'K':' ');
                    }
                }

                pthread_mutex_lock( &outputSettings->buffer_mutex);
                for(t=0; t<MAX_TILES_PER_INPUT; t++) {
                Tiles *tile = outputSettings->tiles[outputSettings->thumbnail_count];
		        static int frame_counter = 0;
                int add;

                    add  = (outputSettings->mode==-1);
                    add |= (outputSettings->mode==-2 && frame->key_frame);
                    add |= (outputSettings->mode>0 && ++frame_counter>=outputSettings->mode);
                    if( add) {
                        frame_counter = 0;
                        if( inputSource->running) {
                            if( sw!=tile->w || sh!=tile->h) {
                                sw = tile->w;
                                sh = tile->h;
                                sws_freeContext(inputSource->scale_sws_ctx[t]);
                                inputSource->scale_sws_ctx[t] = NULL;
                            }
                            if( !inputSource->scale_sws_ctx[t]) {
                                if( verbose) {
                                    printf( "%d:%d '%s' needed scale from %dx%d to %dx%d, type %d\n", t, inputSource->tile_number[t], inputSource->name, 
                                        inputSource->video_dec_ctx->width, inputSource->video_dec_ctx->height, tile->w, tile->h, inputSource->video_dec_ctx->pix_fmt);
                                    fflush( stdout);
                                }

                                /* create scaling context */
                                inputSource->scale_sws_ctx[t] = sws_getContext(inputSource->video_dec_ctx->width, inputSource->video_dec_ctx->height, inputSource->video_dec_ctx->pix_fmt,
                                                     tile->w, tile->h, STREAM_PIX_FMT, SCALE_FLAGS, NULL, NULL, NULL);
                                if (!inputSource->scale_sws_ctx[t]) {
                                    fprintf(stderr,
                                            "Impossible to create scale context for the conversion "
                                            "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                                            av_get_pix_fmt_name(inputSource->video_dec_ctx->pix_fmt), inputSource->video_dec_ctx->width, inputSource->video_dec_ctx->height,
                                            av_get_pix_fmt_name(STREAM_PIX_FMT), tile->w, tile->h);
                                    goto end;
                                }
                            }

                            sws_scale(inputSource->scale_sws_ctx[t],
                                (const uint8_t * const*)frame->data, frame->linesize, 0,
                                inputSource->video_dec_ctx->height, tile->video_dst_data, tile->video_dst_linesize);

                            tile->video_dst_dirty++;
                            tile->updates_per_second++;
                            if( ++outputSettings->thumbnail_count==outputSettings->tiles_count) {
                                outputSettings->thumbnail_count = 0;
                                frame_ready = outputSettings->frames_count;
                            }
                        }
                        else {
                            localBlockFill( tile, 4, YCrCb_WHITE);
                        }
                    }
            	}
end:;
                pthread_mutex_unlock( &outputSettings->buffer_mutex);
#if !API_MODE
                avcodec_free_frame(&here->frame);
#else
                av_frame_free(&here->frame);
#endif
                free( here);
            }
        }
        else {
            usleep(1000);
        }
    }

    return NULL;
}

static int decode_packet(int *got_frame, int cached, inputMosaic *inputSource)
{
    int decoded = inputSource->pkt.size;
    AVFrame *frame;
    int index = MAX_INDEX;

    *got_frame = 0;

#if !API_MODE
    frame = avcodec_alloc_frame();
#else
    frame = av_frame_alloc();
#endif

    if (inputSource->pkt.stream_index == inputSource->video_stream_idx) {
    int ret = avcodec_decode_video2(inputSource->video_dec_ctx, frame, got_frame, &inputSource->pkt);

        /* decode video frame */
        if (ret < 0) {
            fprintf(stderr, "Error decoding video frame (%s)\n", av_err2str(ret));
            return ret;
        }
        index = VIDEO_INDEX;
    } 
    if( *got_frame && index<MAX_INDEX) {
    videoFrames *here;
    videoFrames *current = calloc( 1, sizeof( videoFrames));
    double pts;

        if( (pts = av_frame_get_best_effort_timestamp( frame))==AV_NOPTS_VALUE) {
            pts = 0;
        }
        if( index==VIDEO_INDEX) {
            pts *= av_q2d(inputSource->video_dec_ctx->time_base);
        }
        inputSource->pts[index] = pts;
//        printf( "--> %s V:%8.2f A:%8.2f\r\n", index ? "Audio":"Video", inputSource->pts[0], inputSource->pts[1]);

        current->frame = av_frame_clone( frame);
        current->pts   = pts;
        current->next  = NULL;
        pthread_mutex_lock( &inputSource->list_mutex);
        here = inputSource->packets_list[index];
#if 0
        if( !here) {
            inputSource->packets_list[index] = current;
        }
        else {
            while( here) {
                if( !here->next) {
                    here->next = current;
                    here = NULL;
                }
                else {
                    here = here->next;
                }
            }
        }
#else
        if( !here) {
            inputSource->packets_list[index] = current;
        }
        else {
        videoFrames *last = NULL;

            while( here) {
                if( current->pts<here->pts) {
                    if( !last) {
                        current->next = here;
                        inputSource->packets_list[index] = current;
                    }
                    else {
                        current->next = last->next;
                        last->next = current;
                    }
                    goto used;
                }
                last = here;
                here = last->next;
            }
            if( !here) {
                last->next = current;
            }
        }
#endif
used:   pthread_mutex_unlock( &inputSource->list_mutex);
    }
#if !API_MODE
    avcodec_free_frame(&frame);
#else
    av_frame_free(&frame);
#endif

    return decoded;
}

static int open_codec_context(int *stream_idx,
                              AVFormatContext *fmt_ctx, enum AVMediaType type,
                              inputMosaic *inputSource)
{
    int ret;
    AVStream *st;
    AVCodecContext *dec_ctx = NULL;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), inputSource->src_filename);
        return ret;
    } else {
        *stream_idx = ret;
        st = fmt_ctx->streams[*stream_idx];

        /* find decoder for the stream */
        dec_ctx = st->codec;
        dec = avcodec_find_decoder(dec_ctx->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Init the decoders, with or without reference counting */
#if API_MODE == API_MODE_NEW_API_REF_COUNT
        av_dict_set(&opts, "refcounted_frames", "1", 0);
#endif
        av_dict_set( &opts, "threads", "auto", 0);
        if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
    }

    return 0;
}

#if 0
static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}
#endif

static int interrupt_cb(void *ctx)
{
    inputMosaic *inputSource = ctx;

//    printf( "."); fflush(stdout);

    if (inputSource->quit || stop_all_tasks)
        return 1;

    return 0;
} 

static void *inputThread( void *_whichSource)
{
    inputMosaic *inputSource = _whichSource;
    int ret = 0, got_frame;
    AVDictionary *d = NULL;
 //   Tiles *tile = outputSettings->tiles[ inputSource->tile_number&~TILE_MASK];
    int t;

    inputSource->quit = 0;
    inputSource->finished = 0;
    inputSource->fmt_ctx = avformat_alloc_context();
    if( inputSource->fmt_ctx) {
    float myFps;

        inputSource->fmt_ctx->interrupt_callback.callback = interrupt_cb;
        inputSource->fmt_ctx->interrupt_callback.opaque = inputSource;
        inputSource->fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;

        av_dict_set( &d, "loglevel", "quiet", 0);

        avio_open2( &inputSource->fmt_ctx->pb, inputSource->src_filename, AVIO_FLAG_READ, &inputSource->fmt_ctx->interrupt_callback, &d);

        /* open input file, and allocate format context */
        if (avformat_open_input(&inputSource->fmt_ctx, inputSource->src_filename, NULL, NULL) < 0) {
            fprintf(stderr, "%s:Could not open source file %s\n", inputSource->name, inputSource->src_filename);
            goto end;
        }

        /* retrieve stream information */
        if (avformat_find_stream_info(inputSource->fmt_ctx, NULL) < 0) {
            fprintf(stderr, "Could not find stream information\n");
            goto end;
        }

        if (open_codec_context(&inputSource->video_stream_idx, inputSource->fmt_ctx, AVMEDIA_TYPE_VIDEO, inputSource) >= 0) {
            inputSource->video_stream = inputSource->fmt_ctx->streams[inputSource->video_stream_idx];
            inputSource->video_dec_ctx = inputSource->video_stream->codec;
        }

        /* dump input information to stderr */
        av_dump_format(inputSource->fmt_ctx, 0, inputSource->src_filename, 0);

        myFps = 25.0;
        if( !inputSource->video_dec_ctx->framerate.num && inputSource->video_dec_ctx->framerate.den==1) {
            if( inputSource->video_dec_ctx->pkt_timebase.num==1) {
                myFps = ((double)(inputSource->video_dec_ctx->pkt_timebase.num/10))/100.0;
            }
        }
        else {
            myFps = (double)inputSource->video_dec_ctx->framerate.num/(double)inputSource->video_dec_ctx->framerate.den;
        }

        if( myFps!=inputSource->fps) {
            printf( "%s (%2.3f - %2.3f) time_base:%d/%d (%f) ticks_per_frame:%d delay:%d framerate:%d/%d (%f) pkt_timebase:%d/%d (%f)\r\n", 
                    inputSource->name, inputSource->fps, myFps,
                    inputSource->video_dec_ctx->time_base.num, inputSource->video_dec_ctx->time_base.den,
                    (double)inputSource->video_dec_ctx->time_base.den/inputSource->video_dec_ctx->time_base.num,
                    inputSource->video_dec_ctx->ticks_per_frame, inputSource->video_dec_ctx->delay, 
                    inputSource->video_dec_ctx->framerate.num, inputSource->video_dec_ctx->framerate.den,
                    (double)inputSource->video_dec_ctx->framerate.num/inputSource->video_dec_ctx->framerate.den,
                    inputSource->video_dec_ctx->pkt_timebase.num, inputSource->video_dec_ctx->pkt_timebase.den,
                    (double)inputSource->video_dec_ctx->pkt_timebase.den/inputSource->video_dec_ctx->pkt_timebase.num);

        }

       /* When using the new API, you need to use the libavutil/frame.h API, while
         * the classic frame management is available in libavcodec */
        if ( 1) { // inputSource->frame) {
        int error;

            /* initialize packet, set data to NULL, let the demuxer fill it */
            av_init_packet(&inputSource->pkt);
            inputSource->pkt.data = NULL;
            inputSource->pkt.size = 0;

            /* read frames from the file */
            pthread_mutex_init( &inputSource->list_mutex, NULL);

            if( !inputSource->videoThread) {
                pthread_mutex_init( &inputSource->av_mutex, NULL);
                error = pthread_create( &inputSource->videoThread, NULL, inputThreadVideo, (void *)inputSource);
                if (!error) {
                    pthread_detach( inputSource->videoThread);
                }
                else {
                    error = -1;
                    printf( "Could not create display thread\r\n");
                }
            }
            else {
                error = 0;
            }

            if (!error) {
            int x = 0;

                inputSource->running = 1;
                while (FULL_TASK_RUN && (av_read_frame(inputSource->fmt_ctx, &inputSource->pkt) >= 0)) {
                int cnt;

                    do {
                        ret = decode_packet(&got_frame, 0, inputSource);
                        if (ret < 0)
                            break;
                        inputSource->pkt.data += ret;
                        inputSource->pkt.size -= ret;
                    } while( FULL_TASK_RUN && inputSource->pkt.size > 0);
#define PACKETS_HIGH    (inputSource->video_dec_ctx->delay*8)
#define PACKETS_LOW     (inputSource->video_dec_ctx->delay*2)
                    cnt = localNumberOfPackets( inputSource, VIDEO_INDEX);
                    if( cnt>PACKETS_HIGH) {
                        //printf( "inputThread PAUSED %d\r\n", cnt);
                        do {
                            usleep(1000);
                            cnt = localNumberOfPackets( inputSource, VIDEO_INDEX);
                        } while( FULL_TASK_RUN && cnt>PACKETS_LOW);
                        // printf( "inputThread RESTART %d\r\n, cnt");
                        if( x) {
                            if( !--x) {
                                goto end;
                            }
                        }
                    }
                    else {
                    }
                }
            }
        }
        else {
            printf( "Could not allocate frame\r\n");
        }
end:;
        while( localNumberOfPackets( inputSource, VIDEO_INDEX) || localNumberOfPackets( inputSource, AUDIO_INDEX)) {
            printf( "%s closing down %3d %3d\r", inputSource->name, localNumberOfPackets( inputSource, VIDEO_INDEX), localNumberOfPackets( inputSource, AUDIO_INDEX));
            sleep( 1);
        }
        inputSource->running = 0;
        if( inputSource->video_dec_ctx)
            avcodec_close(inputSource->video_dec_ctx);
#if !API_MODE
//        avcodec_free_frame(&inputSource->frame);
#else
//        av_frame_free(&inputSource->frame);
#endif

        for(t=0;t<MAX_TILES_PER_INPUT;t++) {
            if( inputSource->scale_sws_ctx[t]) {
                sws_freeContext(inputSource->scale_sws_ctx[t]);
                inputSource->scale_sws_ctx[t] = NULL;
            }
        }

        avio_close(inputSource->fmt_ctx->pb);
        avformat_close_input(&inputSource->fmt_ctx);

        av_dict_free( &d);

        avformat_free_context (inputSource->fmt_ctx);
    }

    inputSource->finished = 1;

    return NULL;
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    if (!pkt->stream_index && verbose) {
        AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

        printf("pts_time:%-12s dts_time:%-12s duration_time:%s stream_index:%d [pts:%s dts:%s duration:%s]\n",
           av_ts2timestr(pkt->pts, time_base), av_ts2timestr(pkt->dts, time_base), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index,
           av_ts2str(pkt->pts), av_ts2str(pkt->dts), av_ts2str(pkt->duration));
    }
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;

    if( !st->index) {
        encoded_frames++;
    }
    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    return av_interleaved_write_frame(fmt_ctx, pkt);
}

/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       AVCodec **codec,
                       enum AVCodecID codec_id,
                       OutputInfo *outputSettings)
{
    AVCodecContext *c;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        exit(1);
    }

    ost->st = avformat_new_stream(oc, *codec);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = ost->st->codec;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt     = outputSettings->audio_format;
        c->bit_rate       = outputSettings->audio_bit_rate;        // AUDIO_BIT_RATE; // 64000
        c->sample_rate    = outputSettings->audio_sample_rate;     // AUDIO_SAMPLE_RATE; // 44100;
        c->channels       = outputSettings->audio_channels;
        c->channel_layout = av_get_default_channel_layout(c->channels);
        if ((*codec)->supported_samplerates) {
        int i;

            c->sample_rate = (*codec)->supported_samplerates[0];
            for (i = 0; (*codec)->supported_samplerates[i]; i++) {
                if ((*codec)->supported_samplerates[i] == outputSettings->audio_sample_rate)
                    c->sample_rate = outputSettings->audio_sample_rate;
            }
        }
        c->channels       = av_get_channel_layout_nb_channels(c->channel_layout);
        c->channel_layout = AV_CH_LAYOUT_STEREO;
        if ((*codec)->channel_layouts) {
        int i;

            c->channel_layout = (*codec)->channel_layouts[0];
            for (i = 0; (*codec)->channel_layouts[i]; i++) {
                if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                    c->channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
        c->channels        = av_get_channel_layout_nb_channels(c->channel_layout);
//        ost->st->time_base = (AVRational){ 1, c->sample_rate };
//        c->time_base       = ost->st->time_base;
        c->time_base       = (AVRational){ 1, c->sample_rate };
        break;

    case AVMEDIA_TYPE_VIDEO:
        c->codec_id = outputSettings->video_encoding;
        c->bit_rate = outputSettings->video_bitrate;
        c->width    = outputSettings->screen_width;      /* Resolution must be a multiple of two. */
        c->height   = outputSettings->screen_height;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, outputSettings->video_frame_rate };
        c->time_base     = ost->st->time_base;
        c->gop_size      = outputSettings->gop_size;    // 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = STREAM_PIX_FMT;
        if (c->codec_id == AV_CODEC_ID_H264) {
            av_opt_set( c->priv_data, "preset", outputSettings->x264_preset, 0);
        }
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B frames */
            c->max_b_frames = 2;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            c->mb_decision = 2;
        }
        break;

    default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;
}

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}


static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg, OutputInfo *outputSettings)
{
    int ret;
    AVCodecContext *c = ost->st->codec;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    ost->final_video_frame = NULL;
    if (outputSettings->final_width && outputSettings->final_height) {
        /* open the codec */
        ret = avcodec_open2(c, codec, &opt);
        av_dict_free(&opt);
        if (ret < 0) {
            fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    /* allocate and init a re-usable frame */
    ost->video_frame = alloc_picture(c->pix_fmt, c->width, c->height);
    if (!ost->video_frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

   /* If the output format is not YUV420P, then a temporary YUV420P
    * picture is needed too. It is then converted to the required
    * output format. */
    ost->tmp_video_frame = NULL;
    if (c->pix_fmt != STREAM_PIX_FMT) {
        ost->tmp_video_frame = alloc_picture(STREAM_PIX_FMT, c->width, c->height);
        if (!ost->tmp_video_frame) {
            fprintf(stderr, "Could not allocate temporary picture\n");
            exit(1);
       }
    }
}

/* Prepare a dummy image. */
static void localCreateVideoFrame(AVFrame *pict, int frame_index,
                           int width, int height)
{
    GET_OUTPUT_SETTINGS;
    int x, y, ret;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(pict);
    if (ret < 0)
        exit(1);

    if( outputSettings->background) {
        av_image_copy( pict->data, pict->linesize,
            (const uint8_t **)(outputSettings->background_frame->data), outputSettings->background_frame->linesize,
            outputSettings->background_frame->format, outputSettings->background_frame->width, outputSettings->background_frame->height);
    }
    else if( !outputSettings->fillColourY) {
    int i = frame_index;

        /* Y */
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

        /* Cb and Cr */
        for (y = 0; y < height / 2; y++) {
            for (x = 0; x < width / 2; x++) {
                pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
                pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
            }
        }
    }
    else {
        /* Y */
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                pict->data[0][y * pict->linesize[0] + x] = outputSettings->fillColourY;

        /* Cb and Cr */
        for (y = 0; y < height / 2; y++) {
            for (x = 0; x < width / 2; x++) {
                pict->data[1][y * pict->linesize[1] + x] = outputSettings->fillColourCb;
                pict->data[2][y * pict->linesize[2] + x] = outputSettings->fillColourCr;
            }
        }
    }

    if(1) {
    time_t tt;
    int t;
    int l;

        time(&tt);
        pthread_mutex_lock( &outputSettings->buffer_mutex);
        for( l=0; l<inputs_count; l++) {
        inputMosaic *inputSource = inputs[l];

            for(t=0;t<MAX_TILES_PER_INPUT;t++) {
                for( tt=0; tt<outputSettings->tiles_count; tt++) {
                Tiles *tile = outputSettings->tiles[tt];

                    if( tile->video_dst_dirty) {
                    uint8_t *newPos[4];

                        newPos[0] = pict->data[0];
                        newPos[1] = pict->data[1];
                        newPos[2] = pict->data[2];
                        newPos[3] = pict->data[3];

                        newPos[0] += tile->x;
                        newPos[0] += (tile->y*pict->linesize[0]);
                        newPos[1] += tile->x/2;
                        newPos[1] += (tile->y*pict->linesize[1]/2);
                        newPos[2] += tile->x/2;
                        newPos[2] += (tile->y*pict->linesize[2]/2);
                        av_image_copy( newPos, pict->linesize,
                            (const uint8_t **)(tile->video_dst_data), tile->video_dst_linesize,
                            inputSource->video_dec_ctx->pix_fmt, tile->w, tile->h);
                    }
                }
            }
        }
        pthread_mutex_unlock( &outputSettings->buffer_mutex);
    }
}

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->st->codec;

#if STREAM_DURATION
    /* check if we want to generate more frames */
    if (av_compare_ts(ost->next_pts, ost->st->codec->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)
        return NULL;
#else
    if(stop_all_tasks)
        return NULL;
#endif

//    printf( "get_video_frame %d,%d - %d,%d\r\n", c->width, c->height, ost->final_width, ost->final_height);

    if (c->pix_fmt != STREAM_PIX_FMT) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if( !ost->out_sws_ctx) {
            ost->out_sws_ctx = sws_getContext(c->width, c->height,
                                      STREAM_PIX_FMT,
                                      c->width, c->height,
                                      c->pix_fmt,
                                      SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->out_sws_ctx) {
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        localCreateVideoFrame(ost->tmp_video_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->out_sws_ctx,
                  (const uint8_t * const *)ost->tmp_video_frame->data, ost->tmp_video_frame->linesize,
                  0, c->height, ost->video_frame->data, ost->video_frame->linesize);
    } else if( ost->final_width || ost->final_height) {
        if( !ost->final_sws_ctx) {
            ost->final_sws_ctx = sws_getContext( c->width, c->height, c->pix_fmt,
                                      ost->final_width, ost->final_height, c->pix_fmt,
                                      SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->final_sws_ctx) {
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        localCreateVideoFrame(ost->final_video_frame, ost->next_pts, ost->final_width, ost->final_height);
        sws_scale(ost->out_sws_ctx,
                  (const uint8_t * const *)ost->final_video_frame->data, ost->final_video_frame->linesize,
                  0, c->height, ost->video_frame->data, ost->video_frame->linesize);
    }
    else {
        localCreateVideoFrame(ost->video_frame, ost->next_pts, c->width, c->height);
    }

    ost->video_frame->pts = ost->next_pts++;

    return ost->video_frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
    int ret;
    AVCodecContext *c;
    AVFrame *frame;
    int got_packet = 0;

    c = ost->st->codec;

    frame = get_video_frame(ost);

    if (oc->oformat->flags & AVFMT_RAWPICTURE) {
        /* a hack to avoid data copy with some raw video muxers */
        AVPacket pkt;
        av_init_packet(&pkt);

        if (!frame)
            return 1;

        pkt.flags        |= AV_PKT_FLAG_KEY;
        pkt.stream_index  = ost->st->index;
        pkt.data          = (uint8_t *)frame;
        pkt.size          = sizeof(AVPicture);

        pkt.pts = pkt.dts = frame->pts;
        av_packet_rescale_ts(&pkt, c->time_base, ost->st->time_base);

        ret = av_interleaved_write_frame(oc, &pkt);
    } else {
        AVPacket pkt = { 0 };
        av_init_packet(&pkt);

        /* encode the image */
        ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
        if (ret < 0) {
            fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
            exit(1);
        }

        if (got_packet) {
            ret = write_frame(oc, &c->time_base, ost->st, &pkt);
        } else {
            ret = 0;
        }
    }

    if (ret < 0) {
        fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_close(ost->st->codec);
    av_frame_free(&ost->video_frame);
    av_frame_free(&ost->tmp_video_frame);
    sws_freeContext(ost->out_sws_ctx);
}

/**************************************************************/
/* media file output */
void *outputThread( void *_outputSettings)
{
    OutputInfo *outputSettings = _outputSettings;
    AVOutputFormat *fmt;
    int ret;
    int have_video = 0;
    int encode_video = 0;
    AVDictionary *opt = NULL;

   /* allocate the output media context */
    avformat_alloc_output_context2(&outputSettings->oc, NULL, NULL, outputSettings->filename);
    if (!outputSettings->oc) {
        printf("Could not deduce output format from file extension: using mpegts.\n");
        avformat_alloc_output_context2(&outputSettings->oc, NULL, "mpegts", outputSettings->filename);
    }
    if (!outputSettings->oc)
        return NULL;

    fmt = outputSettings->oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE) {
        add_stream(&outputSettings->video_st, outputSettings->oc, &outputSettings->video_codec, outputSettings->video_encoding, outputSettings);
        have_video = 1;
        encode_video = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        open_video(outputSettings->oc, outputSettings->video_codec, &outputSettings->video_st, opt, outputSettings);

    av_dump_format(outputSettings->oc, 0, outputSettings->filename, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputSettings->oc->pb, outputSettings->filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s': %s\n", outputSettings->filename,
                    av_err2str(ret));
            return NULL;
        }
    }

    /* Write the stream header, if any. */
    ret = avformat_write_header(outputSettings->oc, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        return NULL;
    }

    while (!stop_all_tasks && (encode_video)) {
        if( frame_ready) {
            encode_video = !write_video_frame(outputSettings->oc, &outputSettings->video_st);
            frame_ready--;
        }
        usleep( 500);
    }

    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(outputSettings->oc);

    /* Close each codec. */
    if (have_video)
        close_stream(outputSettings->oc, &outputSettings->video_st);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        avio_close(outputSettings->oc->pb);

    /* free the stream */
    avformat_free_context(outputSettings->oc);

    return NULL;
}

void *mainThread( void *_outputMosaic)
{
OutputInfo *outputSettings = _outputMosaic;
int error;
int t;
int tile_replace;

    for( tile_replace=0; tile_replace<inputs_count; tile_replace++) {
    inputMosaic *thisOne = inputs[ tile_replace];

        // Create thread
        error = pthread_create( &thisOne->pulledThread, NULL, inputThread, (void *)thisOne);
        if (!error) {
            pthread_detach( thisOne->pulledThread);
        }
        else {
            thisOne->running = 0;
        }
        sleep(1);
    }

    while(!stop_all_tasks) {
    int timedOut = 0;

        if( timedOut) {
            pthread_mutex_lock( &outputSettings->buffer_mutex);
            pthread_mutex_unlock( &outputSettings->buffer_mutex);
        }
        encoded_frames = 0;
        {
        int t;

            sleep( 1);
            printf( "Number of encoded frames %d  ", encoded_frames);
            for(t=0;t<outputSettings->tiles_count;t++) {
                printf( "%d ", outputSettings->tiles[t]->updates_per_second);
                outputSettings->tiles[t]->updates_per_second = 0;
            }
            printf( "\r\n");
        }
    }
    sleep(5);

    return NULL;
}

int main( int argc, char **argv)
{
int ret = 0;
int error;
struct sigaction sa;
int tile_replace;
int m;
int o;

OutputInfo *outputSettings;

#if defined( LIBXML_DOTTED_VERSION)
xmlDoc *doc = NULL;
xmlNode *root_element = NULL;
#endif

    // prepare to call sigaction()
    sa.sa_handler = signal_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset( &sa.sa_mask );
    // catch ctrl+C
    sigaction( SIGINT, &sa, 0 );

    signal(SIGPIPE, SIG_IGN);

   /* register all formats and codecs */
    av_register_all();

    avformat_network_init();

    doc = xmlReadFile( "thumbnail.xml", NULL, 0);
    if( doc) {
        /* Get the root element node */
        root_element = xmlDocGetRootElement(doc);
        print_element_names(root_element, "mosaic.xml", LEVEL_TV);

        /* free the document */
        xmlFreeDoc(doc);;
    }
    xmlCleanupParser();

    for( o=0;o<outputMosaicsCnt; o++) {
        outputSettings = outputMosaics[o];

        if( outputSettings->background) {
            outputSettings->background_frame = av_frame_alloc();
            if ( outputSettings->background_frame) {
            int ret = ff_load_image( outputSettings->background_frame->data, outputSettings->background_frame->linesize,
                            &outputSettings->background_frame->width, &outputSettings->background_frame->height,
                            &outputSettings->background_frame->format, outputSettings->background, NULL);
                if( verbose) {
                    printf( "LoadImage %d %dx%d %d\r\n", ret, outputSettings->background_frame->width, outputSettings->background_frame->height,
                            outputSettings->background_frame->format);
                }
            }
        }

        if( !outputSettings->tiles_count) {
        int a, d, ta, td;
        int w, h, px, py;

            ta = outputSettings->tiles_across;
            td = outputSettings->tiles_down;
            w = (outputSettings->screen_width/ta)&~0x03;
            h = (outputSettings->screen_height/td)&~0x03;
            px = ((outputSettings->screen_width-(w*ta))/(ta+1))&~0x01;
            py = ((outputSettings->screen_height-(h*td))/(td+1))&~0x01;
            for( d=0; d<td; d++) {
                for( a=0; a<ta; a++) {
                    outputSettings->tiles                                           = realloc( outputSettings->tiles, (outputSettings->tiles_count+1)*sizeof( Tiles *));
                    outputSettings->tile_map                                        = realloc( outputSettings->tile_map, (outputSettings->tiles_count+1)*sizeof( TILE_MAP));
                    outputSettings->tiles[ outputSettings->tiles_count]             = calloc( 1, sizeof( Tiles));
                    outputSettings->tiles[ outputSettings->tiles_count]->x          = px + (w * a) + (px * a);
                    outputSettings->tiles[ outputSettings->tiles_count]->y          = py + (h * d) + (py * d);
                    outputSettings->tiles[ outputSettings->tiles_count]->w          = w;
                    outputSettings->tiles[ outputSettings->tiles_count]->h          = h;
                    outputSettings->tile_map[ outputSettings->tiles_count]          = outputSettings->tiles_count;
                    outputSettings->tiles[ outputSettings->tiles_count]->frames     = 'A';
                    outputSettings->tiles[ outputSettings->tiles_count]->index      = outputSettings->tiles_count;
                    outputSettings->tiles_count++;

                }
            }
        }
        // Allocate space for the resized video frame
        for( tile_replace=0; tile_replace<outputSettings->tiles_count; tile_replace++) {
        Tiles *thisOne = outputSettings->tiles[tile_replace];

            ret = av_image_alloc( thisOne->video_dst_data, thisOne->video_dst_linesize,
                                    thisOne->w, thisOne->h, STREAM_PIX_FMT, 16);
            thisOne->video_dst_bufsize = ret;
        }

        pthread_mutex_init( &outputSettings->tile_mutex, NULL);
        pthread_mutex_init( &outputSettings->buffer_mutex, NULL);
        error = pthread_create( &outputSettings->outputThread, NULL, outputThread, (void *)outputSettings);
        if (!error) {
            pthread_detach( outputSettings->outputThread);
        }
        else {
            printf( "Output thread could not be created\n");
        }

        m = 0;
        for( tile_replace=0; tile_replace<inputs_count; tile_replace++) {
        int t;

            while( m<outputSettings->tiles_count && !IS_TILE_nF_FnC(outputSettings->tiles[m])) {
                m++;
            }
            inputs[tile_replace]->skip = 0;
            inputs[tile_replace]->tile_number[0] = m<outputSettings->tiles_count ? outputSettings->tile_map[m]+1:0;
            for(t=1;t<MAX_TILES_PER_INPUT;t++) {
                inputs[tile_replace]->tile_number[t] = 0;
            }
            m++;
        }
        error = pthread_create( &outputSettings->threadMain, NULL, mainThread, (void *)outputSettings);
        if (error) {
            printf( "Main thread could not be created\n");
        }
    }

    for( o=0;o<outputMosaicsCnt; o++) {
        outputSettings = outputMosaics[o];

        pthread_join( outputSettings->threadMain, NULL);

        if (outputSettings->background_frame)
            av_freep(&outputSettings->background_frame->data[0]);
        av_frame_free(&outputSettings->background_frame);

        free( (void *)outputSettings->filename);
        free( (void *)outputSettings->x264_preset);
        free( (void *)outputSettings->x264_threads);
        if( outputSettings->tiles) {
            for( tile_replace=0; tile_replace<outputSettings->tiles_count; tile_replace++) {
                free( outputSettings->tiles[ tile_replace]);
            }
            free( outputSettings->tiles);
            free( outputSettings->tile_map);
        }
        if( inputs) {
            for( tile_replace=0; tile_replace<inputs_count; tile_replace++) {
                free( inputs[ tile_replace]->name);
                free( inputs[ tile_replace]->src_filename);
                free( inputs[ tile_replace]);
            }
            free( inputs);
        }
    }

    avformat_network_deinit();

    return ret < 0;
}
