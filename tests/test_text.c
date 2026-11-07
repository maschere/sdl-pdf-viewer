#include "text.h"
#include "ttf_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static Canvas canvas;
static int red(int x, int y) { return canvas.pixels[((size_t)y * canvas.w + x) * 4]; }
static void draw(const TextFont *f, const char *s, Point origin, Point *caret) {
    canvas_clear(&canvas, 0xffffff);
    Path path = {0};
    CHECK(text_path(f, s, 20, origin, &path, caret));
    CHECK(canvas_fill(&canvas, &path, 0x000000, false));
    path_free(&path);
}
int main(int argc, char **argv) {
    static uint8_t font[2048];
    size_t size = ttf_fixture(font, sizeof(font));
    CHECK(size > 0);
    TextFont *f = text_font_from_memory(font, size);
    CHECK(f);
    /* 20 px at 1000 units/em: cap height 700 -> 14 px, line 1000 -> 20 px. */
    TextMetrics m = text_metrics(f, 20);
    CHECK(m.cap_height == 14 && m.line_height == 20 && m.ascent == 16 && m.descent == 4);
    CHECK(canvas_init(&canvas, 64, 64));
    Point caret;
    /* The square covers x 12..22, y 16..30 exactly with pixel-aligned edges. */
    draw(f, "H", (Point){10, 30}, &caret);
    CHECK(red(12, 16) == 0 && red(21, 29) == 0 && red(16, 22) == 0);
    CHECK(red(11, 20) == 255 && red(22, 20) == 255 && red(16, 15) == 255 && red(16, 30) == 255);
    CHECK(caret.x == 24 && caret.y == 30);
    /* The origin is snapped to the pixel grid. */
    draw(f, "H", (Point){10.3, 29.7}, NULL);
    CHECK(red(12, 16) == 0 && red(11, 20) == 255);
    /* Kerning pulls the second H 2 px closer: advance 14 - 2 = 12 px. */
    draw(f, "HH", (Point){10, 30}, &caret);
    CHECK(red(23, 20) == 255 && red(24, 20) == 0 && red(33, 20) == 0 && red(34, 20) == 255);
    CHECK(fabs(caret.x - 36) < 1e-9);
    /* A line break returns to the left edge, one line height lower. */
    draw(f, "H\nH ", (Point){10, 30}, &caret);
    CHECK(red(16, 40) == 0 && red(16, 33) == 255);
    CHECK(caret.y == 50 && fabs(caret.x - 30) < 1e-9);
    /* A composite glyph applies its component offset (50 units = 1 px). */
    draw(f, "B", (Point){10, 30}, NULL);
    CHECK(red(12, 20) == 255 && red(13, 20) == 0 && red(22, 20) == 0 && red(23, 20) == 255);
    /* Implied on-curve points between off-curve points form a rounded shape. */
    draw(f, "O", (Point){10, 30}, NULL);
    CHECK(red(17, 23) == 0 && red(12, 18) > 200);
    /* Unmapped characters fall back to the empty .notdef glyph; invalid
     * UTF-8 and control characters are harmless. */
    draw(f, "\xff\x01\xe2\x82", (Point){10, 30}, &caret);
    CHECK(red(16, 22) == 255);
    /* Every truncation of the font is rejected or renders without faults. */
    for (size_t n = 0; n < size; n++) {
        TextFont *t = text_font_from_memory(font, n);
        if (t) {
            Path path = {0};
            text_path(t, "HOB H", 20, (Point){10, 30}, &path, NULL);
            path_free(&path);
            text_font_free(t);
        }
    }
    CHECK(!text_font_from_memory((const uint8_t *)"not a font at all", 17));
    CHECK(!text_font_load("missing-font-file.ttf"));
    /* Optionally render with a system font when one is available. */
    TextFont *system = text_font_open_default();
    bool have_system = system != NULL;
    if (system) {
        Path path = {0};
        CHECK(text_path(system, "Hamburgefonstiv\n\xc3\x84\xc3\x96\xc3\x9c \xc3\x9f \xc3\xa9 0123", 18, (Point){4, 20}, &path,
                        &caret));
        CHECK(path.count > 100 && caret.y > 20);
        path_free(&path);
        text_font_free(system);
    }
    (void)argc;
    (void)argv;
    canvas_free(&canvas);
    text_font_free(f);
    puts(have_system ? "TrueType text layout, hinting, kerning and system font passed."
                : "TrueType text layout, hinting and kerning passed (no system font).");
    return 0;
}
