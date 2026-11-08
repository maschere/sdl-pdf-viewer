#ifndef RASTER_H
#define RASTER_H
#include "ink.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    double a, b, c, d, e, f;
} Matrix;
typedef struct {
    double x0, y0, x1, y1;
} Box;
typedef struct {
    Point a, b;
    bool begin;
} Edge;
typedef struct {
    Edge *edges;
    size_t count, cap;
    Point current, start;
    bool started, new_subpath;
} Path;
typedef struct {
    int w, h;
    uint8_t *pixels;
    Box clip;
    const uint8_t *mask, *softmask;
    double opacity;
} Canvas;
Matrix matrix_mul(Matrix outer, Matrix inner);
Point matrix_point(Matrix m, Point p);
bool matrix_inverse(Matrix m, Matrix *out);
bool canvas_init(Canvas *c, int w, int h);
void canvas_free(Canvas *c);
void canvas_clear(Canvas *c, uint32_t rgb);
void path_free(Path *p);
bool path_move(Path *p, Point a);
bool path_line(Path *p, Point b);
bool path_close(Path *p);
bool path_curve(Path *p, Point b, Point c, Point d);
bool canvas_fill(Canvas *c, const Path *p, uint32_t rgb, bool evenodd);
bool canvas_stroke(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                   double miter);
/* As canvas_stroke, for an elliptical pen: the linear part of `pen`, which must
 * be invertible, maps a circular pen of diameter `width` to the output. */
bool canvas_stroke_pen(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                       double miter, Matrix pen);
void canvas_image(Canvas *c, const uint8_t *rgba, int w, int h, Matrix unit_to_canvas);
/* As canvas_image, with bicubic resampling instead of nearest-neighbor. */
void canvas_image_bicubic(Canvas *c, const uint8_t *rgba, int w, int h, Matrix unit_to_canvas);
typedef struct {
    int first, count;
} ImageFilterSpan;
typedef struct {
    ImageFilterSpan *spans;
    float *weights; /* `taps` entries per output sample. */
    size_t taps;
} ImageFilter;
typedef struct {
    int w, h, source_w, source_h;
    ImageFilter x, y;
    float *rows, *line;
    uint8_t *pixels; /* The latest result: straight-alpha RGBA, w by h. */
    size_t rows_capacity, line_capacity, pixels_capacity;
} ImageScaler;
/* Separable Catmull-Rom resampling of straight-alpha RGBA, filtered premultiplied.
 * Kernels are kept for repeated use at the same sizes, as for animation frames,
 * and buffers for reuse at any size. False when out of memory or beyond 16
 * million pixels. */
bool image_scale_bicubic(ImageScaler *s, const uint8_t *rgba, int source_w, int source_h, int w,
                         int h);
void image_scaler_free(ImageScaler *s);
/* As canvas_image_bicubic, reusing the working buffers of `scaler`. */
void canvas_image_scaled(Canvas *c, ImageScaler *scaler, const uint8_t *rgba, int w, int h,
                         Matrix unit_to_canvas);
void canvas_paint(Canvas *c, int x, int y, uint32_t rgb, unsigned alpha);
void canvas_disk(Canvas *c, Point center, double radius, uint32_t rgb);
#endif
