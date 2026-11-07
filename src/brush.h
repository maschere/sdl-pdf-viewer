#ifndef BRUSH_H
#define BRUSH_H
#include "raster.h"

/* Coverage is accumulated with max, then composited over the saved pixel.
 * A stroke's softness is independent of mouse-event frequency and overlap. */
typedef struct {
    uint8_t *base, *coverage;
    int w, h;
    double radius, hardness;
    uint32_t rgb;
} Brush;
bool brush_begin(Brush *b, const Canvas *c, double diameter, double hardness, uint32_t rgb);
void brush_line(Brush *b, Canvas *c, Point from, Point to);
void brush_free(Brush *b);
#endif
