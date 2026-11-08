#include "raster.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SCALE_MAX_SIDE 16384
#define SCALE_MAX_PIXELS 16000000u
Matrix matrix_mul(Matrix o, Matrix i) {
    return (Matrix){o.a * i.a + o.c * i.b,       o.b * i.a + o.d * i.b,
                    o.a * i.c + o.c * i.d,       o.b * i.c + o.d * i.d,
                    o.a * i.e + o.c * i.f + o.e, o.b * i.e + o.d * i.f + o.f};
}
Point matrix_point(Matrix m, Point p) {
    return (Point){m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f};
}
bool matrix_inverse(Matrix m, Matrix *o) {
    double d = m.a * m.d - m.b * m.c;
    if (!isfinite(d) || fabs(d) < 1e-12)
        return false;
    *o = (Matrix){m.d / d,
                  -m.b / d,
                  -m.c / d,
                  m.a / d,
                  (m.c * m.f - m.d * m.e) / d,
                  (m.b * m.e - m.a * m.f) / d};
    return true;
}
bool canvas_init(Canvas *c, int w, int h) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384 || (size_t)w * h > 64000000)
        return false;
    *c = (Canvas){.w = w, .h = h, .clip = {0, 0, w, h}, .opacity = 1};
    c->pixels = malloc((size_t)w * h * 4);
    return c->pixels != NULL;
}
void canvas_free(Canvas *c) {
    free(c->pixels);
    memset(c, 0, sizeof(*c));
}
/* An opaque pixel as stored in memory, whatever the byte order. Pixel buffers
 * come from malloc, so they are suitably aligned for whole-pixel access. */
static uint32_t opaque_word(uint32_t rgb) {
    uint8_t bytes[4] = {(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255};
    uint32_t word;
    memcpy(&word, bytes, sizeof(word));
    return word;
}
static void fill_pixels(uint8_t *pixels, size_t count, uint32_t word) {
    uint32_t *out = (uint32_t *)(void *)pixels;
    for (size_t i = 0; i < count; i++)
        out[i] = word;
}
void canvas_clear(Canvas *c, uint32_t rgb) {
    fill_pixels(c->pixels, (size_t)c->w * c->h, opaque_word(rgb));
}
/* Geometry coverage has already been clipped by the scan converter. */
static void composite(Canvas *c, int x, int y, uint32_t rgb, unsigned alpha) {
    size_t at = (size_t)y * c->w + x;
    unsigned a = c->opacity >= 1 ? alpha : (unsigned)(alpha * MAX(0, c->opacity) + .5);
    if (c->mask)
        a = (a * c->mask[at] + 127) / 255;
    if (c->softmask)
        a = (a * c->softmask[at] + 127) / 255;
    uint8_t *p = c->pixels + at * 4;
    unsigned s[3] = {(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255};
    if (p[3] == 255) {
        for (int k = 0; k < 3; k++)
            p[k] = (uint8_t)((s[k] * a + p[k] * (255 - a) + 127) / 255);
    } else {
        unsigned alpha = a * 255 + p[3] * (255 - a);
        for (int k = 0; k < 3; k++)
            p[k] = alpha ? (uint8_t)((s[k] * a * 255 + p[k] * p[3] * (255 - a) + alpha/2) / alpha) : 0;
        p[3] = (uint8_t)((alpha + 127) / 255);
    }
}
void canvas_paint(Canvas *c, int x, int y, uint32_t rgb, unsigned alpha) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    double coverage = MAX(0, MIN(x + 1., c->clip.x1) - MAX(x, c->clip.x0)) *
                      MAX(0, MIN(y + 1., c->clip.y1) - MAX(y, c->clip.y0));
    if (coverage > 0)
        composite(c, x, y, rgb, (unsigned)(alpha * coverage + .5));
}
void path_free(Path *p) {
    free(p->edges);
    memset(p, 0, sizeof(*p));
}
bool path_move(Path *p, Point a) {
    p->current = p->start = a;
    p->started = true;
    p->new_subpath = true;
    return true;
}
bool path_line(Path *p, Point b) {
    if (!p->started || !isfinite(b.x) || !isfinite(b.y) || fabs(b.x) > 1e8 || fabs(b.y) > 1e8)
        return false;
    if (p->count >= 200000)
        return false;
    if (p->count == p->cap) {
        size_t n = p->cap ? p->cap * 2 : 128;
        Edge *e = realloc(p->edges, n * sizeof(*e));
        if (!e)
            return false;
        p->edges = e;
        p->cap = n;
    }
    p->edges[p->count++] = (Edge){p->current, b, p->new_subpath};
    p->new_subpath = false;
    p->current = b;
    return true;
}
bool path_close(Path *p) { return !p->started || path_line(p, p->start); }
static bool curve(Path *p, Point a, Point b, Point c, Point d, int depth) {
    double dx = d.x - a.x, dy = d.y - a.y;
    double u = fabs((b.x - d.x) * dy - (b.y - d.y) * dx),
           v = fabs((c.x - d.x) * dy - (c.y - d.y) * dx);
    if (depth >= 12 || ((u + v) * (u + v) <= .01 * (dx * dx + dy * dy) &&
                        hypot(b.x - a.x, b.y - a.y) + hypot(c.x - b.x, c.y - b.y) +
                                hypot(d.x - c.x, d.y - c.y) - hypot(dx, dy) <
                            .2))
        return path_line(p, d);
    Point ab = midpoint(a, b), bc = midpoint(b, c), cd = midpoint(c, d), abc = midpoint(ab, bc),
          bcd = midpoint(bc, cd), m = midpoint(abc, bcd);
    return curve(p, a, ab, abc, m, depth + 1) && curve(p, m, bcd, cd, d, depth + 1);
}
bool path_curve(Path *p, Point b, Point c, Point d) {
    return p->started && curve(p, p->current, b, c, d, 0);
}
/* Eight vertical samples and exact horizontal span coverage. Only active edges
 * are visited; range differences fill solid interiors once per output row. */
#define AA_SAMPLES 8

typedef struct {
    double x, y0, y1, slope;
    int dir;
} ScanEdge;
typedef struct {
    double x;
    int dir;
} Hit;
static int hit_compare(const void *a, const void *b) {
    double x = ((const Hit *)a)->x, y = ((const Hit *)b)->x;
    return (x > y) - (x < y);
}
static int edge_compare(const void *a, const void *b) {
    double x = ((const ScanEdge *)a)->y0, y = ((const ScanEdge *)b)->y0;
    return (x > y) - (x < y);
}
static int bound(double v, int limit) {
    if (!isfinite(v) || v <= 0)
        return 0;
    if (v >= limit)
        return limit;
    return (int)v;
}
static void scan_edge(ScanEdge *edges, size_t *n, Point a, Point b) {
    if (a.y == b.y)
        return;
    int dir = b.y > a.y ? 1 : -1;
    if (a.y > b.y) {
        Point tmp = a;
        a = b;
        b = tmp;
    }
    edges[(*n)++] = (ScanEdge){a.x, a.y, b.y, (b.x - a.x) / (b.y - a.y), dir};
}
static void span_coverage(double *coverage, double *delta, double left, double right,
                          double weight) {
    if (right <= left)
        return;
    int a = (int)floor(left), b = (int)ceil(right) - 1;
    if (a == b)
        coverage[a] += (right - left) * weight;
    else {
        coverage[a] += (a + 1 - left) * weight;
        coverage[b] += (right - b) * weight;
        delta[a + 1] += weight;
        delta[b] -= weight;
    }
}
/* Pixels of one output row sharing a single coverage value. */
typedef struct {
    int x, count;
    unsigned alpha;
} CoverageRun;
static void sort_hits(Hit *hits, size_t n) {
    if (n > 32) {
        qsort(hits, n, sizeof(*hits), hit_compare);
        return;
    }
    for (size_t i = 1; i < n; i++) {
        Hit h = hits[i];
        size_t j = i;
        for (; j > 0 && hits[j - 1].x > h.x; j--)
            hits[j] = hits[j - 1];
        hits[j] = h;
    }
}
static size_t build_runs(CoverageRun *runs, const double *coverage, const double *delta, int x0,
                         int x1) {
    size_t n = 0;
    double total = 0;
    for (int x = x0; x < x1; x++) {
        total += delta[x - x0];
        unsigned alpha = (unsigned)(MAX(0, MIN(1, coverage[x - x0] + total)) * 255 + .5);
        if (!alpha)
            continue;
        if (n && runs[n - 1].alpha == alpha && runs[n - 1].x + runs[n - 1].count == x)
            runs[n - 1].count++;
        else
            runs[n++] = (CoverageRun){x, 1, alpha};
    }
    return n;
}
static void paint_runs(Canvas *c, int y, const CoverageRun *runs, size_t n, uint32_t rgb) {
    bool plain = !c->mask && !c->softmask && c->opacity >= 1;
    uint32_t word = opaque_word(rgb);
    for (size_t i = 0; i < n; i++) {
        CoverageRun r = runs[i];
        if (plain && r.alpha == 255)
            fill_pixels(c->pixels + ((size_t)y * c->w + r.x) * 4, (size_t)r.count, word);
        else
            for (int k = 0; k < r.count; k++)
                composite(c, r.x + k, y, rgb, r.alpha);
    }
}
bool canvas_fill(Canvas *c, const Path *p, uint32_t rgb, bool evenodd) {
    if (!p->count)
        return true;
    double xmin = c->w, xmax = 0, ymin = c->h, ymax = 0;
    for (size_t i = 0; i < p->count; i++) {
        Edge e = p->edges[i];
        xmin = MIN(xmin, MIN(e.a.x, e.b.x));
        xmax = MAX(xmax, MAX(e.a.x, e.b.x));
        ymin = MIN(ymin, MIN(e.a.y, e.b.y));
        ymax = MAX(ymax, MAX(e.a.y, e.b.y));
    }
    double left = MAX(0, MAX(xmin, c->clip.x0)), right = MIN(c->w, MIN(xmax, c->clip.x1));
    double top = MAX(0, MAX(ymin, c->clip.y0)), bottom = MIN(c->h, MIN(ymax, c->clip.y1));
    if (right <= left || bottom <= top)
        return true;
    int x0 = bound(floor(left), c->w), x1 = bound(ceil(right), c->w);
    int y0 = bound(floor(top), c->h), y1 = bound(ceil(bottom), c->h);
    size_t capacity = p->count * 2;
    size_t row_bytes = (size_t)(x1 - x0 + 1) * sizeof(double);
    /* One block, ordered so that every array is correctly aligned. */
    size_t edge_bytes = capacity * sizeof(ScanEdge), hit_bytes = capacity * sizeof(Hit);
    size_t active_bytes = capacity * sizeof(ScanEdge *);
    size_t run_bytes = (size_t)(x1 - x0 + 1) * sizeof(CoverageRun);
    unsigned char *block = malloc(edge_bytes + hit_bytes + 2 * row_bytes + active_bytes + run_bytes);
    if (!block)
        return false;
    ScanEdge *edges = (ScanEdge *)(void *)block;
    Hit *hits = (Hit *)(void *)(block + edge_bytes);
    double *coverage = (double *)(void *)(block + edge_bytes + hit_bytes);
    double *delta = (double *)(void *)((unsigned char *)coverage + row_bytes);
    ScanEdge **active = (ScanEdge **)(void *)((unsigned char *)delta + row_bytes);
    CoverageRun *runs = (CoverageRun *)(void *)((unsigned char *)active + active_bytes);
    size_t count = 0, next = 0, live = 0, run_count = 0;
    /* The runs also describe following rows crossed by the same vertical edges. */
    bool row_repeats = false;
    double repeat_end = 0;
    Point start = {0};
    for (size_t i = 0; i < p->count; i++) {
        Edge e = p->edges[i];
        if (e.begin)
            start = e.a;
        scan_edge(edges, &count, e.a, e.b);
        if (i + 1 == p->count || p->edges[i + 1].begin)
            scan_edge(edges, &count, e.b, start);
    }
    qsort(edges, count, sizeof(*edges), edge_compare);
    for (int y = y0; y < y1; y++) {
        double ya = MAX(y, top), yb = MIN(y + 1., bottom);
        bool full_row = ya == y && yb == y + 1.;
        if (row_repeats && full_row && repeat_end >= y + 1. &&
            (next == count || edges[next].y0 >= y + 1.)) {
            paint_runs(c, y, runs, run_count, rgb);
            continue;
        }
        memset(coverage, 0, row_bytes);
        memset(delta, 0, row_bytes);
        double weight = (yb - ya) / AA_SAMPLES;
        bool changed = false;
        for (int sample = 0; sample < AA_SAMPLES; sample++) {
            double fy = ya + (sample + .5) * weight;
            size_t n = 0;
            for (size_t i = 0; i < live; i++)
                if (active[i]->y1 > fy)
                    active[n++] = active[i];
            if (sample && n != live)
                changed = true;
            live = n;
            while (next < count && edges[next].y0 <= fy) {
                if (edges[next].y1 > fy) {
                    active[live++] = &edges[next];
                    if (sample)
                        changed = true;
                }
                next++;
            }
            for (size_t i = 0; i < live; i++) {
                ScanEdge *e = active[i];
                hits[i] = (Hit){e->x + (fy - e->y0) * e->slope, e->dir};
            }
            sort_hits(hits, live);
            int winding = 0;
            double begin = 0;
            for (size_t i = 0; i < live; i++) {
                bool inside = evenodd ? (winding & 1) != 0 : winding != 0;
                winding += hits[i].dir;
                bool after = evenodd ? (winding & 1) != 0 : winding != 0;
                if (!inside && after)
                    begin = hits[i].x;
                else if (inside && !after)
                    span_coverage(coverage, delta, MAX(begin, left) - x0,
                                  MIN(hits[i].x, right) - x0, weight);
            }
        }
        run_count = build_runs(runs, coverage, delta, x0, x1);
        paint_runs(c, y, runs, run_count, rgb);
        row_repeats = full_row && !changed;
        repeat_end = bottom;
        for (size_t i = 0; row_repeats && i < live; i++) {
            row_repeats = active[i]->slope == 0;
            repeat_end = MIN(repeat_end, active[i]->y1);
        }
    }
    free(block);
    return true;
}
/* All pieces of a stroke have the same winding and are filled as one union.
 * This avoids dark seams where antialiased segments, caps and joins overlap. */
static bool polygon(Path *p, const Point *pts, int n) {
    double area = 0;
    for (int i = 0; i < n; i++) {
        Point a = pts[i], b = pts[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    if (!path_move(p, pts[0]))
        return false;
    for (int i = 1; i < n; i++)
        if (!path_line(p, pts[area > 0 ? n - i : i]))
            return false;
    return path_close(p);
}
static bool disk_path(Path *p, Point center, double r) {
    double x = center.x, y = center.y, k = r * .5522847498307936;
    return path_move(p, (Point){x + r, y}) &&
           path_curve(p, (Point){x + r, y - k}, (Point){x + k, y - r}, (Point){x, y - r}) &&
           path_curve(p, (Point){x - k, y - r}, (Point){x - r, y - k}, (Point){x - r, y}) &&
           path_curve(p, (Point){x - r, y + k}, (Point){x - k, y + r}, (Point){x, y + r}) &&
           path_curve(p, (Point){x + k, y + r}, (Point){x + r, y + k}, (Point){x + r, y}) &&
           path_close(p);
}
void canvas_disk(Canvas *c, Point center, double radius, uint32_t rgb) {
    Path p = {0};
    if (radius > 0 && disk_path(&p, center, radius))
        canvas_fill(c, &p, rgb, false);
    path_free(&p);
}
/* Builds the outline with a circular pen around `p`, optionally mapping the
 * finished outline through `pen` before filling. */
static bool stroke_path(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                        double miter, const Matrix *pen) {
    double r = width > 0 ? width * .5 : .5;
    Path outline = {0};
    bool ok = true;
    size_t substart = 0, subend = 0;
    bool closed = false;
    for (size_t i = 0; i < p->count; i++) {
        Edge e = p->edges[i];
        if (e.begin) {
            substart = i;
            subend = i + 1;
            while (subend < p->count && !p->edges[subend].begin)
                subend++;
            Point lastpoint = p->edges[subend - 1].b;
            closed = lastpoint.x == e.a.x && lastpoint.y == e.a.y;
        }
        double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, len = hypot(dx, dy);
        if (len < 1e-9) {
            if (cap == 1 && !disk_path(&outline, e.a, r))
                goto failed;
            continue;
        }
        Point n = {-dy / len * r, dx / len * r};
        bool first = e.begin && !closed;
        bool last = i + 1 == subend && !closed;
        Point a = e.a, b = e.b;
        if (cap == 2) {
            if (first) {
                a.x -= dx / len * r;
                a.y -= dy / len * r;
            }
            if (last) {
                b.x += dx / len * r;
                b.y += dy / len * r;
            }
        }
        Point quad[] = {{a.x + n.x, a.y + n.y},
                        {b.x + n.x, b.y + n.y},
                        {b.x - n.x, b.y - n.y},
                        {a.x - n.x, a.y - n.y}};
        if (!polygon(&outline, quad, 4))
            goto failed;
        if (cap == 1 && first && !disk_path(&outline, e.a, r))
            goto failed;
        if (cap == 1 && last && !disk_path(&outline, e.b, r))
            goto failed;
        if (i + 1 < subend || closed) {
            Edge next = p->edges[i + 1 < subend ? i + 1 : substart];
            double nx = next.b.x - next.a.x, ny = next.b.y - next.a.y, nlen = hypot(nx, ny);
            if (nlen < 1e-9)
                continue;
            if (join == 1) {
                if (!disk_path(&outline, e.b, r))
                    goto failed;
                continue;
            }
            Point nn = {-ny / nlen * r, nx / nlen * r};
            for (int side = -1; side <= 1; side += 2) {
                Point u = {e.b.x + side * n.x, e.b.y + side * n.y},
                      v = {e.b.x + side * nn.x, e.b.y + side * nn.y};
                Point tri[] = {e.b, u, v};
                if (!polygon(&outline, tri, 3))
                    goto failed;
                double cross = dx * ny - dy * nx;
                if (join == 0 && fabs(cross) > 1e-10) {
                    double t = ((v.x - u.x) * ny - (v.y - u.y) * nx) / cross;
                    Point tip = {u.x + t * dx, u.y + t * dy};
                    if (hypot(tip.x - e.b.x, tip.y - e.b.y) <= miter * r) {
                        Point mt[] = {u, tip, v};
                        if (!polygon(&outline, mt, 3))
                            goto failed;
                    }
                }
            }
        }
    }
    /* A linear map flips every piece's winding alike, so the union is preserved. */
    for (size_t i = 0; pen && i < outline.count; i++) {
        outline.edges[i].a = matrix_point(*pen, outline.edges[i].a);
        outline.edges[i].b = matrix_point(*pen, outline.edges[i].b);
    }
    ok = canvas_fill(c, &outline, rgb, false);
    path_free(&outline);
    return ok;
failed:
    path_free(&outline);
    return false;
}
bool canvas_stroke(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                   double miter) {
    return stroke_path(c, p, rgb, width, cap, join, miter, NULL);
}
bool canvas_stroke_pen(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                       double miter, Matrix pen) {
    Matrix linear = {pen.a, pen.b, pen.c, pen.d, 0, 0}, inverse;
    if (!matrix_inverse(linear, &inverse))
        return false;
    if (!p->count)
        return true;
    /* Stroke in the space where the pen is circular. */
    Path local = {0};
    local.edges = malloc(p->count * sizeof(*local.edges));
    if (!local.edges)
        return false;
    local.count = p->count;
    for (size_t i = 0; i < p->count; i++) {
        local.edges[i] = p->edges[i];
        local.edges[i].a = matrix_point(inverse, p->edges[i].a);
        local.edges[i].b = matrix_point(inverse, p->edges[i].b);
    }
    bool ok = stroke_path(c, &local, rgb, width, cap, join, miter, &linear);
    path_free(&local);
    return ok;
}
static bool all_opaque(const uint8_t *rgba, size_t pixels) {
    for (size_t i = 0; i < pixels; i++)
        if (rgba[i * 4 + 3] != 255)
            return false;
    return true;
}
/* canvas_image's sampling for an unrotated image: each output column reads one
 * source column, so it is computed once rather than once per pixel. */
static bool image_axis_aligned(Canvas *c, const uint8_t *rgba, int w, int h, Matrix inv, int xa,
                               int xb, int ya, int yb) {
    int *columns = malloc((size_t)(xb - xa) * sizeof(*columns));
    if (!columns)
        return false;
    for (int x = xa; x < xb; x++) {
        double u = inv.a * (x + .5) + inv.e;
        bool visible = x + .5 >= c->clip.x0 && x + .5 < c->clip.x1 && u >= 0 && u < 1;
        columns[x - xa] = visible ? MIN(w - 1, (int)(u * w)) : -1;
    }
    /* Columns fully inside the clip need no coverage computation. The visible
     * columns form one interval, so trimming its ends leaves no gaps. */
    int inner0 = MAX(xa, (int)ceil(c->clip.x0)), inner1 = MIN(xb, (int)floor(c->clip.x1));
    while (inner0 < inner1 && columns[inner0 - xa] < 0)
        inner0++;
    while (inner0 < inner1 && columns[inner1 - 1 - xa] < 0)
        inner1--;
    bool contiguous = inner1 > inner0 && all_opaque(rgba, (size_t)w * h);
    for (int x = inner0 + 1; contiguous && x < inner1; x++)
        contiguous = columns[x - xa] == columns[x - 1 - xa] + 1;
    uint32_t *out = (uint32_t *)(void *)c->pixels;
    for (int y = ya; y < yb; y++) {
        double v = inv.d * (y + .5) + inv.f;
        if (y + .5 < c->clip.y0 || y + .5 >= c->clip.y1 || v < 0 || v >= 1)
            continue;
        const uint8_t *row = rgba + (size_t)MIN(h - 1, (int)((1 - v) * h)) * w * 4;
        uint32_t *line = out + (size_t)y * c->w;
        bool whole_row = y >= c->clip.y0 && y + 1 <= c->clip.y1;
        for (int x = xa; x < xb; x++) {
            if (whole_row && contiguous && x == inner0) {
                memcpy(line + x, row + (size_t)columns[x - xa] * 4, (size_t)(inner1 - inner0) * 4);
                x = inner1 - 1;
                continue;
            }
            int column = columns[x - xa];
            if (column < 0)
                continue;
            const uint8_t *s = row + (size_t)column * 4;
            if (whole_row && x >= inner0 && x < inner1 && s[3] == 255)
                memcpy(line + x, s, 4);
            else
                canvas_paint(c, x, y, (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2], s[3]);
        }
    }
    free(columns);
    return true;
}
void canvas_image(Canvas *c, const uint8_t *rgba, int w, int h, Matrix m) {
    Matrix inv;
    if (!matrix_inverse(m, &inv))
        return;
    Point q[] = {matrix_point(m, (Point){0, 0}), matrix_point(m, (Point){1, 0}),
                 matrix_point(m, (Point){1, 1}), matrix_point(m, (Point){0, 1})};
    double x0 = c->w, x1 = 0, y0 = c->h, y1 = 0;
    for (int i = 0; i < 4; i++) {
        x0 = MIN(x0, q[i].x);
        x1 = MAX(x1, q[i].x);
        y0 = MIN(y0, q[i].y);
        y1 = MAX(y1, q[i].y);
    }
    int ya = bound(floor(MAX(y0, c->clip.y0)), c->h), yb = bound(ceil(MIN(y1, c->clip.y1)), c->h);
    int xa = bound(floor(MAX(x0, c->clip.x0)), c->w), xb = bound(ceil(MIN(x1, c->clip.x1)), c->w);
    if (m.b == 0 && m.c == 0 && !c->mask && !c->softmask && c->opacity >= 1 && xb > xa &&
        image_axis_aligned(c, rgba, w, h, inv, xa, xb, ya, yb))
        return;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            if (x + .5 < c->clip.x0 || x + .5 >= c->clip.x1 || y + .5 < c->clip.y0 ||
                y + .5 >= c->clip.y1)
                continue;
            Point p = matrix_point(inv, (Point){x + .5, y + .5});
            if (p.x < 0 || p.x >= 1 || p.y < 0 || p.y >= 1)
                continue;
            int ix = MIN(w - 1, (int)(p.x * w)), iy = MIN(h - 1, (int)((1 - p.y) * h));
            const uint8_t *s = rgba + ((size_t)iy * w + ix) * 4;
            canvas_paint(c, x, y, (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2], s[3]);
        }
}
/* The same Catmull-Rom curve as the animation scaler. Its support is widened in
 * proportion when minifying, so that shrunken images average instead of alias. */
static double catmull_rom(double x) {
    x = fabs(x);
    return x < 1 ? 1 - 2.5 * x * x + 1.5 * x * x * x
           : x < 2 ? 2 - 4 * x + 2.5 * x * x - .5 * x * x * x
                   : 0;
}
static void filter_free(ImageFilter *f) {
    free(f->spans);
    free(f->weights);
    memset(f, 0, sizeof(*f));
}
static bool filter_build(ImageFilter *f, int from, int to) {
    double scale = MAX(1, (double)from / to), support = 2 * scale;
    f->taps = (size_t)ceil(2 * support) + 1;
    f->spans = malloc((size_t)to * sizeof(*f->spans));
    f->weights = malloc((size_t)to * f->taps * sizeof(*f->weights));
    if (!f->spans || !f->weights)
        return false;
    for (int i = 0; i < to; i++) {
        double center = (i + .5) * from / to, sum = 0;
        int first = (int)MAX(0, ceil(center - support - .5));
        int last = (int)MIN(from - 1, floor(center + support - .5));
        float *w = f->weights + (size_t)i * f->taps;
        int count = 0;
        for (int j = first; j <= last; j++) {
            double v = catmull_rom((j + .5 - center) / scale);
            w[count++] = (float)v;
            sum += v;
        }
        /* Renormalized, since samples past the image's edges are dropped. */
        for (int k = 0; sum != 0 && k < count; k++)
            w[k] = (float)(w[k] / sum);
        f->spans[i] = (ImageFilterSpan){first, count};
    }
    return true;
}
void image_scaler_free(ImageScaler *s) {
    filter_free(&s->x);
    filter_free(&s->y);
    free(s->rows);
    free(s->line);
    free(s->pixels);
    memset(s, 0, sizeof(*s));
}
/* Grows a buffer without keeping its contents. Fresh pages are costly to touch,
 * so a buffer never shrinks and serves later images of any size. */
static bool reserve(void **buffer, size_t *capacity, size_t bytes) {
    if (bytes <= *capacity)
        return true;
    free(*buffer);
    *capacity = 0;
    *buffer = malloc(bytes);
    if (!*buffer)
        return false;
    *capacity = bytes;
    return true;
}
static bool scale_size_valid(int w, int h) {
    return w >= 1 && h >= 1 && w <= SCALE_MAX_SIDE && h <= SCALE_MAX_SIDE &&
           (size_t)w * h <= SCALE_MAX_PIXELS;
}
bool image_scale_bicubic(ImageScaler *s, const uint8_t *rgba, int sw, int sh, int w, int h) {
    if (!scale_size_valid(sw, sh) || !scale_size_valid(w, h) || (size_t)w * sh > SCALE_MAX_PIXELS)
        return false;
    if (s->w != w || s->h != h || s->source_w != sw || s->source_h != sh) {
        filter_free(&s->x);
        filter_free(&s->y);
        s->w = s->h = s->source_w = s->source_h = 0;
        void *rows = s->rows, *line = s->line, *pixels = s->pixels;
        bool ok = reserve(&rows, &s->rows_capacity, (size_t)w * sh * 4 * sizeof(*s->rows)) &&
                  reserve(&line, &s->line_capacity, (size_t)w * 4 * sizeof(*s->line)) &&
                  reserve(&pixels, &s->pixels_capacity, (size_t)w * h * 4);
        s->rows = rows;
        s->line = line;
        s->pixels = pixels;
        if (!ok || !filter_build(&s->x, sw, w) || !filter_build(&s->y, sh, h)) {
            image_scaler_free(s);
            return false;
        }
        s->w = w;
        s->h = h;
        s->source_w = sw;
        s->source_h = sh;
    }
    /* Most images have no transparency, so premultiplication can be skipped. */
    bool opaque = all_opaque(rgba, (size_t)sw * sh);
    for (int y = 0; y < sh; y++) {
        const uint8_t *row = rgba + (size_t)y * sw * 4;
        float *o = s->rows + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            ImageFilterSpan span = s->x.spans[x];
            const float *weight = s->x.weights + (size_t)x * s->x.taps;
            float v[4] = {0};
            if (opaque) {
                for (int t = 0; t < span.count; t++) {
                    const uint8_t *p = row + (size_t)(span.first + t) * 4;
                    v[0] += p[0] * weight[t];
                    v[1] += p[1] * weight[t];
                    v[2] += p[2] * weight[t];
                }
                memcpy(o, v, sizeof(v));
                continue;
            }
            for (int t = 0; t < span.count; t++) {
                const uint8_t *p = row + (size_t)(span.first + t) * 4;
                float color = p[3] * (1 / 255.f) * weight[t];
                v[0] += p[0] * color;
                v[1] += p[1] * color;
                v[2] += p[2] * color;
                v[3] += p[3] * weight[t];
            }
            memcpy(o, v, sizeof(v));
        }
    }
    for (int y = 0; y < h; y++) {
        ImageFilterSpan span = s->y.spans[y];
        const float *weight = s->y.weights + (size_t)y * s->y.taps;
        memset(s->line, 0, (size_t)w * 4 * sizeof(*s->line));
        for (int t = 0; t < span.count; t++) {
            const float *row = s->rows + (size_t)(span.first + t) * w * 4;
            for (int i = 0; i < w * 4; i++)
                s->line[i] += row[i] * weight[t];
        }
        uint8_t *o = s->pixels + (size_t)y * w * 4;
        if (opaque) {
            for (int x = 0; x < w; x++, o += 4) {
                const float *v = s->line + (size_t)x * 4;
                for (int k = 0; k < 3; k++) {
                    int value = (int)(v[k] + .5f);
                    o[k] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
                }
                o[3] = 255;
            }
            continue;
        }
        for (int x = 0; x < w; x++, o += 4) {
            const float *v = s->line + (size_t)x * 4;
            double alpha = MAX(0, MIN(255, v[3]));
            if (alpha >= 254.999) { /* Mostly opaque images need no division here. */
                for (int k = 0; k < 3; k++) {
                    int value = (int)(v[k] + .5f);
                    o[k] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
                }
                o[3] = 255;
                continue;
            }
            for (int k = 0; k < 3; k++)
                o[k] = alpha > 0 ? (uint8_t)(MAX(0, MIN(255, v[k] * 255 / alpha)) + .5) : 0;
            o[3] = (uint8_t)(alpha + .5);
        }
    }
    return true;
}
void canvas_image_scaled(Canvas *c, ImageScaler *s, const uint8_t *rgba, int w, int h, Matrix m) {
    /* Resample to the output size, within the scaler's limits, then place it
     * pixel for pixel. */
    double tw = MIN(SCALE_MAX_SIDE, ceil(hypot(m.a, m.b)));
    double th = MIN(SCALE_MAX_SIDE, ceil(hypot(m.c, m.d)));
    if (isfinite(tw) && isfinite(th) && tw * th > SCALE_MAX_PIXELS) {
        double shrink = sqrt(SCALE_MAX_PIXELS / (tw * th));
        tw = floor(tw * shrink);
        th = floor(th * shrink);
    }
    if (tw * h > SCALE_MAX_PIXELS)
        tw = floor((double)SCALE_MAX_PIXELS / h);
    /* Failing only loses the smoothing, not the image. */
    bool scaled = tw >= 1 && th >= 1 && ((int)tw != w || (int)th != h) &&
                  image_scale_bicubic(s, rgba, w, h, (int)tw, (int)th);
    canvas_image(c, scaled ? s->pixels : rgba, scaled ? s->w : w, scaled ? s->h : h, m);
}
void canvas_image_bicubic(Canvas *c, const uint8_t *rgba, int w, int h, Matrix m) {
    ImageScaler s = {0};
    canvas_image_scaled(c, &s, rgba, w, h, m);
    image_scaler_free(&s);
}
