#include "raster.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
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
void canvas_clear(Canvas *c, uint32_t rgb) {
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) {
        c->pixels[4 * i] = (uint8_t)(rgb >> 16);
        c->pixels[4 * i + 1] = (uint8_t)(rgb >> 8);
        c->pixels[4 * i + 2] = (uint8_t)rgb;
        c->pixels[4 * i + 3] = 255;
    }
}
/* Geometry coverage has already been clipped by the scan converter. */
static void composite(Canvas *c, int x, int y, uint32_t rgb, unsigned alpha) {
    size_t at = (size_t)y * c->w + x;
    unsigned a = (unsigned)(alpha * MAX(0, MIN(1, c->opacity)) + .5);
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
    ScanEdge *edges = malloc(capacity * sizeof(*edges));
    ScanEdge **active = malloc(capacity * sizeof(*active));
    Hit *hits = malloc(capacity * sizeof(*hits));
    size_t row_bytes = (size_t)(x1 - x0 + 1) * sizeof(double);
    double *coverage = malloc(row_bytes), *delta = malloc(row_bytes);
    if (!edges || !active || !hits || !coverage || !delta) {
        free(edges); free(active); free(hits); free(coverage); free(delta);
        return false;
    }
    size_t count = 0, next = 0, live = 0;
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
        memset(coverage, 0, row_bytes);
        memset(delta, 0, row_bytes);
        double ya = MAX(y, top), yb = MIN(y + 1., bottom);
        double weight = (yb - ya) / AA_SAMPLES;
        for (int sample = 0; sample < AA_SAMPLES; sample++) {
            double fy = ya + (sample + .5) * weight;
            size_t n = 0;
            for (size_t i = 0; i < live; i++)
                if (active[i]->y1 > fy)
                    active[n++] = active[i];
            live = n;
            while (next < count && edges[next].y0 <= fy) {
                if (edges[next].y1 > fy)
                    active[live++] = &edges[next];
                next++;
            }
            for (size_t i = 0; i < live; i++) {
                ScanEdge *e = active[i];
                hits[i] = (Hit){e->x + (fy - e->y0) * e->slope, e->dir};
            }
            qsort(hits, live, sizeof(*hits), hit_compare);
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
        double total = 0;
        for (int x = x0; x < x1; x++) {
            total += delta[x - x0];
            unsigned alpha = (unsigned)(MAX(0, MIN(1, coverage[x - x0] + total)) * 255 + .5);
            if (alpha)
                composite(c, x, y, rgb, alpha);
        }
    }
    free(edges); free(active); free(hits); free(coverage); free(delta);
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
bool canvas_stroke(Canvas *c, const Path *p, uint32_t rgb, double width, int cap, int join,
                   double miter) {
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
    ok = canvas_fill(c, &outline, rgb, false);
    path_free(&outline);
    return ok;
failed:
    path_free(&outline);
    return false;
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
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            if (x + .5 < c->clip.x0 || x + .5 >= c->clip.x1 || y + .5 < c->clip.y0 ||
                y + .5 >= c->clip.y1)
                continue;
            Point p = matrix_point(inv, (Point){x + .5, y + .5});
            if (p.x < 0 || p.x >= 1 || p.y < 0 || p.y >= 1)
                continue;
            int ix = (int)(p.x * w), iy = MIN(h - 1, (int)((1 - p.y) * h));
            const uint8_t *s = rgba + ((size_t)iy * w + ix) * 4;
            canvas_paint(c, x, y, (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2], s[3]);
        }
}
