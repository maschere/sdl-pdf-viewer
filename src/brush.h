#ifndef BRUSH_H
#define BRUSH_H
#include "raster.h"

/* Coverage is accumulated with max, then composited over the saved pixel.
 * A stroke's softness is independent of mouse-event frequency and overlap. */
typedef struct {
    uint8_t *base, *coverage;
    int w, h;
    int x0, y0, x1, y1; /* Bounds of the pixels touched since the last reset. */
    double radius, hardness;
    uint32_t rgb;
} Brush;
bool brush_begin(Brush *b, const Canvas *c, double diameter, double hardness, uint32_t rgb);
void brush_line(Brush *b, Canvas *c, Point from, Point to);
/* Restores every pixel touched since brush_begin or the last restore and
 * empties the coverage, so a shape can be redrawn from scratch. */
void brush_restore(Brush *b, Canvas *c);
void brush_free(Brush *b);
#endif
