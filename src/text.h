#ifndef TEXT_H
#define TEXT_H
#include "raster.h"

/* Minimal TrueType (glyf) text renderer. Glyph outlines are filled by the
 * application's own coverage rasterizer. A light vertical hint snaps the
 * baseline, x-height and cap height to whole output pixels; horizontal
 * positions keep their fractional advances and pair kerning. */
typedef struct TextFont TextFont;
typedef struct {
    double ascent, descent, line_height, cap_height, x_height;
} TextMetrics;

TextFont *text_font_from_memory(const uint8_t *data, size_t size);
TextFont *text_font_load(const char *path);
/* Tries $SDL_PDF_VIEWER_FONT, then common system sans-serif fonts. */
TextFont *text_font_open_default(void);
void text_font_free(TextFont *f);
/* Hinted metrics in pixels for a font size (em size) in pixels. */
TextMetrics text_metrics(const TextFont *f, double size);
/* Appends the outlines of UTF-8 text to `out`. `origin` is the left end of
 * the first baseline (rounded to the pixel grid); '\n' starts a new line.
 * `caret`, if given, receives the baseline point after the last glyph. */
bool text_path(const TextFont *f, const char *utf8, double size, Point origin, Path *out,
               Point *caret);
#endif
