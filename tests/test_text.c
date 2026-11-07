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
    /* Corrupt glyphs are skipped: 'O' gets an instruction length past its data
     * and 'B' becomes a composite of itself. The rest of the text renders. */
    {
        uint8_t bad[2048];
        memcpy(bad, font, size);
        size_t glyf = 0, loca = 0;
        for (int i = 0, n = bad[4] << 8 | bad[5]; i < n; i++) {
            const uint8_t *r = bad + 12 + 16 * i;
            size_t at = (size_t)r[8] << 24 | (size_t)r[9] << 16 | (size_t)r[10] << 8 | r[11];
            if (!memcmp(r, "glyf", 4))
                glyf = at;
            if (!memcmp(r, "loca", 4))
                loca = at;
        }
        CHECK(glyf && loca);
        size_t o = glyf + 2 * (size_t)(bad[loca + 4] << 8 | bad[loca + 5]);
        size_t b = glyf + 2 * (size_t)(bad[loca + 6] << 8 | bad[loca + 7]);
        bad[o + 12] = bad[o + 13] = 0xff;
        bad[b + 12] = 0;
        bad[b + 13] = 3;
        TextFont *t = text_font_from_memory(bad, size);
        CHECK(t);
        draw(t, "HOBH", (Point){10, 30}, &caret);
        CHECK(red(16, 22) == 0 && red(16 + 42, 22) == 0); /* First and last H. */
        CHECK(red(16 + 14, 22) == 255 && red(16 + 28, 22) == 255);
        text_font_free(t);
    }
    /* Text whose outline exceeds the path limit reports failure. */
    {
        size_t n = 60000;
        char *many = malloc(n + 1);
        CHECK(many);
        memset(many, 'H', n);
        many[n] = 0;
        Path path = {0};
        CHECK(!text_path(f, many, 20, (Point){10, 30}, &path, NULL));
        path_free(&path);
        free(many);
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
