#ifndef INK_H
#define INK_H
#include <math.h>
#include <stdbool.h>
typedef struct {
    double x, y;
} Point;
typedef struct {
    Point last, end;
    bool active;
} Ink;
typedef struct {
    Point start, c1, c2, end;
} Curve;
static inline Point midpoint(Point a, Point b) {
    return (Point){(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
}
static inline Curve quadratic(Point a, Point control, Point b) {
    return (Curve){a,
                   {a.x + (control.x - a.x) * 2 / 3, a.y + (control.y - a.y) * 2 / 3},
                   {b.x + (control.x - b.x) * 2 / 3, b.y + (control.y - b.y) * 2 / 3},
                   b};
}
static inline void ink_begin(Ink *ink, Point p) { *ink = (Ink){p, p, true}; }
static inline Curve ink_add(Ink *ink, Point p) {
    Point m = midpoint(ink->last, p);
    Curve c = quadratic(ink->end, ink->last, m);
    ink->last = p;
    ink->end = m;
    return c;
}
static inline Curve ink_finish(Ink *ink) {
    Curve c = quadratic(ink->end, ink->last, ink->last);
    ink->active = false;
    return c;
}
static inline bool is_drag(Point a, Point b, double threshold) {
    return hypot(a.x - b.x, a.y - b.y) >= threshold;
}
#endif
