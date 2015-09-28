// Font data for Anonymous Pro 8pt
typedef unsigned char uint8_t;

typedef struct _FONT_CHAR_INFO {
    uint8_t width;
    short offset;
} FONT_CHAR_INFO;

typedef struct _FONT_INFO {
    int char_height;
    int start_character;
    int end_character;
    int space_width;
    const FONT_CHAR_INFO *descriptors;
    const uint8_t *bitmap;
} FONT_INFO;

// Font data for Anonymous Pro 9pt
extern const uint8_t anonymousPro_9ptBitmaps[];
extern const FONT_INFO anonymousPro_9ptFontInfo;
extern const FONT_CHAR_INFO anonymousPro_9ptDescriptors[];
