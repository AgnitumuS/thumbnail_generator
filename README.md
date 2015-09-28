# thumbnail_generator
Creates a video with the input video tiled to the users requirements

This program will take in a video file and produce another video file which contains either every frame, every key frame or every X frame and create a tiled screen.

The input configuration is an xml which allows you to either specify a generic row*cols for tiling or you can specify your own tile set

Options to be added 

1. Which Frames to be added
  1.1 Every Frame
  1.2 Key Frames
  1.3 Every X frame
  1.4 Every X+Y frames
  1.5 Every X seconds

2. Frame Created Duration, so once a frame is crated this will be encoded X times

3. Audio input


XML Breakdown

<MosaicControl>
	<Control verbose="0" save_input="0" save_output="0"/>

	<Output>
		<mosaic size="720,576"										Size of new video
		 url="a0.ts" 												Output for new video

Video encoding
		 video_encoding="H264" 			
		 video_bitrate="3000000" 
		 video_framerate="25" 
		 gop_size="75" 
		 x264_preset="faster" 
		 x264_threads="4" 

Audio encoder TODO
		 audio_encoding="AAC" 
		 audio_bitrate="128000,2,32000,AV_SAMPLE_FMT_S16" 

		 border="0" 												Adds a border around the tiles automatically
		 final_size="720,288" 										TODO
		 tiles_across="5" 											Number of tiles across     OVERRIDDEN BY TILES DEFINITIONS
		 tiles_down="5" 											Number of tiles down 	   OVERRIDDEN BY TILES DEFINITIONS
		 mode="25" or "A" or "K"									Choose either All, Key or every X
		 frame_count="1" 											Encode the output frame X times
		 fill_colour="<filename>" or "#YYUUVV"						Either fill background with a user defined image or colour
		 />

		<tile position="8,8,344,560" map="0"/>
		<tile position="368,8,344,560" map="1"/>
	</Output>

	<Inputs>
		<stream name="MTV3.ts" url="/media/encoder/My Passport/Terminator.Genisys.2015.720p.BluRay.x264.YIFY.mp4" fps="25.00"/> 
	</Inputs>
</MosaicControl>
