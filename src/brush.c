#include "brush.h"
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void brush_free(Brush *b) {
    free(b->base);
    free(b->coverage);
    memset(b, 0, sizeof(*b));
}
bool brush_begin(Brush *b, const Canvas *c, double diameter, double hardness, uint32_t rgb) {
    if (!isfinite(diameter) || diameter <= 0 || !isfinite(hardness) ||
        hardness < 0 || hardness > 1)
        return false;
    if (b->w != c->w || b->h != c->h || !b->base || !b->coverage) {
        brush_free(b);
        size_t pixels = (size_t)c->w * c->h;
        b->base = malloc(pixels * 4);
        b->coverage = malloc(pixels);
        if (!b->base || !b->coverage) {
            brush_free(b);
            return false;
        }
        b->w = c->w;
        b->h = c->h;
    }
    memset(b->coverage, 0, (size_t)b->w * b->h);
    b->radius = diameter * .5;
    b->hardness = hardness;
    b->rgb = rgb;
    return true;
}
/* Integral of the radial tip profile: solid to hardness * radius, followed
 * by a cubic smoothstep feather. A one-pixel box filter antialiases its edge.
 * Filtering the closest distance (rather than accumulating dabs) makes both
 * event subdivision and retracing within one stroke exactly idempotent. */
static double tip_integral(double distance, double core, double radius) {
    if (distance <= core)
        return distance;
    double feather = radius - core;
    if (distance >= radius)
        return core + feather * .5;
    double t = (distance - core) / feather;
    return core + feather * (t - t * t * t + .5 * t * t * t * t);
}
void brush_line(Brush *b, Canvas *c, Point from, Point to) {
    if (!b->coverage || b->w != c->w || b->h != c->h ||
        !isfinite(from.x) || !isfinite(from.y) || !isfinite(to.x) || !isfinite(to.y))
        return;
    double r = b->radius, core = r * b->hardness;
    double dx = to.x - from.x, dy = to.y - from.y, length2 = dx * dx + dy * dy;
    int x0 = (int)MAX(0, MIN(c->w, floor(MIN(from.x, to.x) - r)));
    int x1 = (int)MAX(0, MIN(c->w, ceil(MAX(from.x, to.x) + r)));
    int y0 = (int)MAX(0, MIN(c->h, floor(MIN(from.y, to.y) - r)));
    int y1 = (int)MAX(0, MIN(c->h, ceil(MAX(from.y, to.y) + r)));
    for (int y = y0; y < y1; y++) {
        /* Restrict diagonal segments to a narrow band, not their full box. */
        double t0 = 0, t1 = 1;
        if (fabs(dy) > 1e-12) {
            double a = (y - r - from.y) / dy, z = (y + 1 + r - from.y) / dy;
            t0 = MAX(0, MIN(1, MIN(a, z)));
            t1 = MAX(0, MIN(1, MAX(a, z)));
        }
        double ax = from.x + dx * t0, bx = from.x + dx * t1;
        int xa = (int)MAX(x0, MIN(x1, floor(MIN(ax, bx) - r)));
        int xb = (int)MAX(x0, MIN(x1, ceil(MAX(ax, bx) + r)));
        for (int x = xa; x < xb; x++) {
            double px = x + .5 - from.x, py = y + .5 - from.y;
            double t = length2 > 1e-12 ? MAX(0, MIN(1, (px * dx + py * dy) / length2)) : 0;
            double ex = px - t * dx, ey = py - t * dy, distance2 = ex * ex + ey * ey;
            if (distance2 >= (r + .5) * (r + .5))
                continue;
            double distance = sqrt(distance2);
            double coverage = tip_integral(distance + .5, core, r) -
                              tip_integral(distance - .5, core, r);
            unsigned alpha = (unsigned)(MAX(0, MIN(1, coverage)) * 255 + .5);
            size_t at = (size_t)y * c->w + x;
            if (alpha <= b->coverage[at])
                continue;
            uint8_t *pixel = c->pixels + at * 4, *base = b->base + at * 4;
            if (!b->coverage[at])
                memcpy(base, pixel, 4);
            else
                memcpy(pixel, base, 4);
            b->coverage[at] = (uint8_t)alpha;
            canvas_paint(c, x, y, b->rgb, alpha);
        }
    }
}
