#include "brush.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned red(const Canvas *c, int x, int y) {
    return c->pixels[((size_t)y * c->w + x) * 4];
}
static void rectangle(Path *p, double x0, double y0, double x1, double y1) {
    CHECK(path_move(p, (Point){x0, y0}));
    CHECK(path_line(p, (Point){x1, y0}));
    CHECK(path_line(p, (Point){x1, y1}));
    CHECK(path_line(p, (Point){x0, y1}));
    CHECK(path_close(p));
}
static void coverage_tests(Canvas *c) {
    Path p = {0};
    canvas_clear(c, 0xffffff);
    rectangle(&p, 10.25, 10.25, 20.75, 20.75);
    CHECK(canvas_fill(c, &p, 0, false));
    CHECK(abs((int)red(c, 10, 10) - 112) <= 1); /* 0.75 * 0.75 coverage */
    CHECK(abs((int)red(c, 10, 15) - 64) <= 1);
    CHECK(red(c, 11, 11) == 0 && red(c, 9, 10) == 255);
    path_free(&p);
    canvas_clear(c, 0xffffff);
    CHECK(path_move(&p, (Point){5, 5}));
    CHECK(path_line(&p, (Point){45, 45}));
    CHECK(path_line(&p, (Point){5, 45}));
    /* Implicit closure and a half-covered diagonal pixel. */
    CHECK(canvas_fill(c, &p, 0, false));
    for (int i = 6; i < 44; i++)
        CHECK(abs((int)red(c, i, i) - 128) <= 1);
    CHECK(red(c, 10, 20) == 0 && red(c, 20, 10) == 255);
    path_free(&p);
    canvas_clear(c, 0xffffff);
    rectangle(&p, 5, 5, 40, 40);
    rectangle(&p, 15, 15, 30, 30);
    CHECK(canvas_fill(c, &p, 0, true));
    CHECK(red(c, 20, 20) == 255 && red(c, 10, 10) == 0);
    canvas_clear(c, 0xffffff);
    CHECK(canvas_fill(c, &p, 0, false));
    CHECK(red(c, 20, 20) == 0);
    /* Fractional clipping, opacity and clip masks compose with coverage. */
    canvas_clear(c, 0xffffff);
    c->clip = (Box){10.25, 10.25, 20.75, 20.75};
    c->opacity = .5;
    CHECK(canvas_fill(c, &p, 0, false));
    CHECK(abs((int)red(c, 10, 10) - 183) <= 1);
    CHECK(red(c, 9, 15) == 255);
    c->clip = (Box){0, 0, c->w, c->h};
    c->opacity = 1;
    path_free(&p);
    /* A subpixel-wide filled shape must retain its area. */
    canvas_clear(c, 0xffffff);
    rectangle(&p, 12.4, 8, 12.6, 40);
    CHECK(canvas_fill(c, &p, 0, false));
    CHECK(abs((int)red(c, 12, 20) - 204) <= 1);
    path_free(&p);
}
static void stroke_tests(Canvas *c) {
    Path p = {0};
    canvas_clear(c, 0xffffff);
    c->opacity = .5;
    CHECK(path_move(&p, (Point){10, 20}));
    CHECK(path_line(&p, (Point){25, 20}));
    CHECK(path_line(&p, (Point){40, 20}));
    CHECK(canvas_stroke(c, &p, 0, 7, 1, 1, 10));
    for (int x = 10; x <= 39; x++)
        CHECK(abs((int)red(c, x, 20) - 127) <= 1);
    CHECK(red(c, 7, 17) > 127 && red(c, 7, 17) < 255);
    path_free(&p);
    c->opacity = 1;
    canvas_clear(c, 0xffffff);
    CHECK(path_move(&p, (Point){10, 30.5}));
    CHECK(path_line(&p, (Point){40, 30.5}));
    CHECK(canvas_stroke(c, &p, 0, .2, 0, 0, 10));
    CHECK(abs((int)red(c, 20, 30) - 204) <= 1);
    path_free(&p);
}
static void brush_tests(Canvas *c) {
    Brush b = {0};
    Point center = {20.5, 20.5};
    canvas_clear(c, 0xffffff);
    CHECK(brush_begin(&b, c, 7, .7, 0));
    brush_line(&b, c, center, center);
    CHECK(red(c, 20, 20) == 0 && red(c, 21, 20) == 0);
    CHECK(red(c, 22, 20) <= 5);
    CHECK(red(c, 23, 20) > 100 && red(c, 23, 20) < 220);
    CHECK(red(c, 24, 20) == 255);
    for (int i = 0; i <= 4; i++) {
        CHECK(red(c, 20 + i, 20) == red(c, 20 - i, 20));
        CHECK(red(c, 20 + i, 20) == red(c, 20, 20 + i));
    }
    uint8_t saved[64 * 64 * 4];
    memcpy(saved, c->pixels, sizeof(saved));
    for (int i = 0; i < 50; i++)
        brush_line(&b, c, center, center);
    CHECK(!memcmp(saved, c->pixels, sizeof(saved))); /* no buildup in a stroke */
    /* Splitting a straight segment into mouse events keeps its profile. */
    canvas_clear(c, 0xffffff);
    CHECK(brush_begin(&b, c, 7, .7, 0));
    brush_line(&b, c, (Point){8.5, 20.5}, (Point){50.5, 20.5});
    memcpy(saved, c->pixels, sizeof(saved));
    canvas_clear(c, 0xffffff);
    CHECK(brush_begin(&b, c, 7, .7, 0));
    for (int i = 8; i < 50; i++)
        brush_line(&b, c, (Point){i + .5, 20.5}, (Point){i + 1.5, 20.5});
    for (size_t i = 0; i < sizeof(saved); i++)
        CHECK(abs((int)saved[i] - c->pixels[i]) <= 1);
    canvas_clear(c, 0xffffff);
    CHECK(brush_begin(&b, c, 7, .7, 0));
    brush_line(&b, c, (Point){8.5, 10.5}, (Point){50.5, 31.5});
    memcpy(saved, c->pixels, sizeof(saved));
    canvas_clear(c, 0xffffff);
    CHECK(brush_begin(&b, c, 7, .7, 0));
    for (int i = 0; i < 21; i++)
        brush_line(&b, c, (Point){8.5 + 2 * i, 10.5 + i},
                   (Point){10.5 + 2 * i, 11.5 + i});
    for (size_t i = 0; i < sizeof(saved); i++)
        CHECK(abs((int)saved[i] - c->pixels[i]) <= 1);
    /* Page clipping and strokes crossing the canvas border. */
    canvas_clear(c, 0xffffff);
    c->clip = (Box){10, 10, 50, 50};
    CHECK(brush_begin(&b, c, 7, .7, 0));
    brush_line(&b, c, (Point){-100, -100}, (Point){100, 100});
    CHECK(red(c, 5, 5) == 255 && red(c, 30, 30) == 0);
    c->clip = (Box){0, 0, c->w, c->h};
    brush_free(&b);
}
int main(void) {
    Canvas c = {0};
    CHECK(canvas_init(&c, 64, 64));
    coverage_tests(&c);
    stroke_tests(&c);
    brush_tests(&c);
    canvas_free(&c);
    puts("Subpixel fills, diagonal coverage, fill rules, clipping, translucent stroke unions "
         "and the 7px / 70% soft brush passed.");
    return 0;
}
