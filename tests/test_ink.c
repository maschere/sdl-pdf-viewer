#include "../src/ink.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-9; }
int main(void) {
    Ink ink;
    ink_begin(&ink, (Point){0, 0});
    Curve a = ink_add(&ink, (Point){10, 0});
    CHECK(near(a.start.x, 0) && near(a.end.x, 5));
    Curve b = ink_add(&ink, (Point){10, 10});
    CHECK(near(a.end.x, b.start.x) && near(a.end.y, b.start.y));
    /* First derivatives match at the midpoint, avoiding corners. */
    CHECK(near(a.end.x - a.c2.x, b.c1.x - b.start.x));
    CHECK(near(a.end.y - a.c2.y, b.c1.y - b.start.y));
    Curve end = ink_finish(&ink);
    CHECK(!ink.active && near(end.end.x, 10) && near(end.end.y, 10));
    CHECK(!is_drag((Point){0, 0}, (Point){2, 2}, 4));
    CHECK(is_drag((Point){0, 0}, (Point){3, 4}, 4));
    ink_begin(&ink, (Point){7, 9});
    end = ink_finish(&ink);
    CHECK(near(end.start.x, end.end.x) && near(end.start.y, end.end.y));
    puts("Ink continuity, endpoint, tap and drag tests passed.");
}
