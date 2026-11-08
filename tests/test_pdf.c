#include "pdf.h"
#include "ttf_fixture.h"
#include "type1.h"
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
static Pdf *open_fixture(const char *root, const char *name, bool success) {
    char file[2048], error[256];
    snprintf(file, sizeof(file), "%s/%s", root, name);
    Pdf *d = pdf_open(file, error, sizeof(error));
    if ((d != NULL) != success) {
        fprintf(stderr, "%s: %s\n", name, d ? "unexpected success" : error);
        exit(1);
    }
    return d;
}
static void rgb(Canvas *c, int x, int y, int r, int g, int b, int tolerance) {
    const unsigned char *p = c->pixels + ((size_t)y * c->w + x) * 4;
    if (abs(p[0] - r) > tolerance || abs(p[1] - g) > tolerance || abs(p[2] - b) > tolerance) {
        fprintf(stderr, "Pixel %d,%d: got %d,%d,%d; expected %d,%d,%d\n", x, y, p[0], p[1], p[2], r,
                g, b);
        exit(1);
    }
}
/* Writes a page showing 'H' then code 0x92 (WinAnsi U+2019, also a square in
 * the fixture font) at 50pt. Only H has a PDF width, 1000 units, which differs
 * from the 700 in the font's own hmtx so the test proves which one is used. */
static void write_truetype_pdf(const char *path, const char *encoding) {
    uint8_t font[2048];
    size_t font_size = ttf_fixture(font, sizeof(font));
    CHECK(font_size);
    static const char content[] = "BT /F1 50 Tf 10 20 Td (H\\222) Tj ET";
    char font_dict[256];
    snprintf(font_dict, sizeof(font_dict),
             "/Type /Font /Subtype /TrueType /BaseFont /Fixture /Encoding /%s "
             "/FontDescriptor 6 0 R /FirstChar 72 /LastChar 72 /Widths [1000]",
             encoding);
    struct {
        const char *dict;
        const void *data;
        size_t size;
    } objects[] = {
        {"/Type /Catalog /Pages 2 0 R", NULL, 0},
        {"/Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100]", NULL, 0},
        {"/Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >>", NULL,
         0},
        {"", content, sizeof(content) - 1},
        {font_dict, NULL, 0},
        {"/Type /FontDescriptor /FontName /Fixture /Flags 32 /FontFile2 7 0 R", NULL, 0},
        {"", font, font_size},
    };
    enum { COUNT = sizeof(objects) / sizeof(*objects) };
    long offsets[COUNT];
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fputs("%PDF-1.4\n", f);
    for (int i = 0; i < COUNT; i++) {
        offsets[i] = ftell(f);
        fprintf(f, "%d 0 obj\n<< %s", i + 1, objects[i].dict);
        if (objects[i].data) {
            fprintf(f, " /Length %zu >>\nstream\n", objects[i].size);
            fwrite(objects[i].data, 1, objects[i].size, f);
            fputs("\nendstream", f);
        } else
            fputs(" >>", f);
        fputs("\nendobj\n", f);
    }
    long xref = ftell(f);
    fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", COUNT + 1);
    for (int i = 0; i < COUNT; i++)
        fprintf(f, "%010ld 00000 n \n", offsets[i]);
    fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", COUNT + 1, xref);
    CHECK(!fclose(f));
}
/* Opens and outlines every glyph of a possibly damaged Type 1 font program; it
 * may fail, but must do so cleanly (AddressSanitizer builds check the rest). */
static void exercise_type1(const unsigned char *data, size_t size) {
    static const char *const names[] = {".notdef", "A", "acute", "Aacute", "flexy"};
    char error[160];
    Type1 *font = type1_open(data, size, error, sizeof(error));
    if (!font)
        return;
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        Path path = {0};
        int glyph = type1_glyph(font, names[i], strlen(names[i]));
        type1_outline(font, glyph, (Matrix){1, 0, 0, 1, 0, 0}, &path, error, sizeof(error));
        path_free(&path);
    }
    type1_close(font);
}
static void check_damaged_type1(const char *root) {
    char file[2048];
    snprintf(file, sizeof(file), "%s/type1-font.pfa", root);
    FILE *f = fopen(file, "rb");
    CHECK(f);
    static unsigned char font[16384];
    size_t size = fread(font, 1, sizeof(font), f);
    fclose(f);
    CHECK(size > 0 && size < sizeof(font));
    for (size_t n = 0; n < size; n++)
        exercise_type1(font, n);
    for (size_t i = 0; i < size; i++) {
        font[i] ^= 0x55;
        exercise_type1(font, size);
        font[i] ^= 0x55;
    }
}
int main(int argc, char **argv) {
    CHECK(argc >= 2);
    const char *root = argv[1];
    Pdf *d = open_fixture(root, "demo.pdf", true);
    CHECK(pdf_page_count(d) == 2);
    Canvas c = {0};
    Matrix m;
    CHECK(canvas_init(&c, 400, 300));
    CHECK(pdf_render(d, 0, &c, &m));
    /* Bicubic images: true at the source pixels' centers, blended between them. */
    rgb(&c, 50, 170, 255, 0, 0, 3);
    rgb(&c, 110, 170, 0, 255, 0, 3);
    rgb(&c, 50, 230, 0, 0, 255, 3);
    rgb(&c, 110, 230, 255, 255, 0, 3);
    rgb(&c, 80, 170, 128, 128, 0, 8);
    rgb(&c, 200, 200, 255, 0, 255, 3);
    rgb(&c, 320, 210, 127, 127, 255, 1);
    rgb(&c, 30, 280, 255, 0, 0, 3);
    rgb(&c, 50, 280, 0, 255, 0, 3);
    PdfLink link;
    CHECK(pdf_link_at(d, 0, (Point){300, 210}, &link) && link.page == 1 && !link.url);
    CHECK(pdf_link_at(d, 0, (Point){300, 150}, &link) &&
          !strcmp(link.url, "https://www.libsdl.org/"));
    CHECK(pdf_link_at(d, 0, (Point){5, 5}, &link) && link.page == 1);
    CHECK(pdf_link_at(d, 0, (Point){15, 5}, &link) && link.page == 1);
    CHECK(!pdf_link_at(d, 0, (Point){200, 250}, &link));
    CHECK(pdf_render(d, 1, &c, &m));
    Matrix inv;
    CHECK(matrix_inverse(m, &inv));
    Point p = {87, 163}, q = matrix_point(inv, matrix_point(m, p));
    CHECK(fabs(p.x - q.x) < 1e-8 && fabs(p.y - q.y) < 1e-8);
    CHECK(pdf_link_at(d, 1, (Point){100, 100}, &link) && link.page == 0);
    /* Repeated rendering must not retain per-frame parser/image allocations. */
    for (int i = 0; i < 100; i++)
        CHECK(pdf_render(d, i % 2, &c, &m));
    pdf_close(d);
    canvas_free(&c);
    CHECK(canvas_init(&c, 100, 100));
    d = open_fixture(root, "clipping.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 50, 255, 0, 0, 0);
    rgb(&c, 10, 10, 255, 255, 255, 0);
    rgb(&c, 5, 95, 0, 0, 255, 0);
    pdf_close(d);
    d = open_fixture(root, "vectors.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 60, 255, 0, 0, 0);
    rgb(&c, 25, 50, 0, 0, 255, 0);
    pdf_close(d);
    d = open_fixture(root, "path-clipping.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 10, 90, 255, 0, 0, 0);
    rgb(&c, 90, 10, 255, 255, 255, 0);
    pdf_close(d);
    /* A reversed MediaBox and link Rect are normalized; a zero-area link and a
     * link with a missing destination do not hide the link beneath them. */
    d = open_fixture(root, "reversed-rects.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 50, 0, 0, 255, 0);
    rgb(&c, 5, 5, 255, 255, 255, 0);
    CHECK(pdf_link_at(d, 0, (Point){70, 70}, &link) && !strcmp(link.url, "https://example.org/"));
    CHECK(pdf_link_at(d, 0, (Point){30, 30}, &link) && !strcmp(link.url, "https://example.org/"));
    CHECK(pdf_link_at(d, 0, (Point){60, 60}, &link) && link.page == 0 && !link.url);
    CHECK(!pdf_link_at(d, 0, (Point){95, 95}, &link));
    pdf_close(d);
    d = open_fixture(root, "compressed.pdf", true);
    CHECK(pdf_page_count(d) == 1);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 50, 0, 0, 255, 0);
    pdf_close(d);
    const char *fonts[] = {"cff-simple.pdf", "cff-cid.pdf"};
    for (int i = 0; i < 2; i++) {
        d = open_fixture(root, fonts[i], true);
        CHECK(pdf_render(d, 0, &c, &m));
        rgb(&c, 30, 80, 0, 0, 0, 0);
        rgb(&c, 75, 80, 0, 0, 0, 0);
        rgb(&c, 5, 5, 255, 255, 255, 0);
        /* Small CFF text also receives coverage antialiasing in headless mode. */
        Canvas small = {0};
        CHECK(canvas_init(&small, 73, 73));
        CHECK(pdf_render(d, 0, &small, &m));
        int partial = 0;
        for (size_t at = 0; at < (size_t)small.w * small.h; at++) {
            const uint8_t *p = small.pixels + at * 4;
            if (p[0] > 0 && p[0] < 255 && p[0] == p[1] && p[1] == p[2])
                partial++;
        }
        CHECK(partial > 10);
        canvas_free(&small);
        pdf_close(d);
    }
    /* Each 20-pixel square has one edge drawn by a flex operator; a wrong flex
     * end point shifts the far corner across these inside/outside samples. */
    d = open_fixture(root, "cff-flex.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    const int horizontal_flex[][2] = {{27, 73}, {57, 73}, {87, 73}, {27, 23}};
    for (int i = 0; i < 4; i++) {
        rgb(&c, horizontal_flex[i][0], horizontal_flex[i][1], 0, 0, 0, 0);
        rgb(&c, horizontal_flex[i][0], horizontal_flex[i][1] - 6, 255, 255, 255, 0);
    }
    rgb(&c, 57, 30, 0, 0, 0, 0); /* The vertical flex1 square. */
    rgb(&c, 63, 30, 255, 255, 255, 0);
    pdf_close(d);
    /* Width-2 strokes under 4x vertical and horizontal scales are 8 pixels
     * across, while their lengths along the other axis stay unscaled. */
    d = open_fixture(root, "stroke-nonuniform.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 60, 17, 0, 0, 0, 0);
    rgb(&c, 60, 14, 255, 255, 255, 0);
    rgb(&c, 17, 60, 0, 0, 0, 0);
    rgb(&c, 14, 60, 255, 255, 255, 0);
    rgb(&c, 88, 20, 0, 0, 0, 0);
    rgb(&c, 92, 20, 255, 255, 255, 0);
    pdf_close(d);
    /* Type 1: A through the built-in encoding; B and C through Differences to a
     * seac composite and a flex-edged box. Binary with lenIV 4, hex with lenIV 0. */
    const char *type1_files[] = {"type1.pdf", "type1-hex.pdf"};
    for (int i = 0; i < 2; i++) {
        d = open_fixture(root, type1_files[i], true);
        CHECK(pdf_render(d, 0, &c, &m));
        rgb(&c, 15, 85, 0, 0, 0, 0);       /* A: the FontMatrix sets its 5..25 extent. */
        rgb(&c, 30, 85, 255, 255, 255, 0);
        rgb(&c, 65, 85, 0, 0, 0, 0);       /* seac base. */
        rgb(&c, 75, 67, 0, 0, 0, 0);       /* seac accent, offset by the side bearing. */
        rgb(&c, 70, 60, 255, 255, 255, 0);
        rgb(&c, 15, 35, 0, 0, 0, 0);       /* The flex box's interior... */
        rgb(&c, 22, 28, 0, 0, 0, 0);       /* ...undistorted top right corner... */
        rgb(&c, 22, 22, 255, 255, 255, 0);
        rgb(&c, 15, 44, 255, 255, 255, 0); /* ...and the flex's raised bottom edge. */
        pdf_close(d);
    }
    check_damaged_type1(root);
    /* Embedded TrueType, as in slides imported from Office-made PDFs: squares
     * at x 15-40 and, after the 1000-unit PDF width, x 65-90. */
    const char *truetype = "test_pdf_truetype.pdf";
    write_truetype_pdf(truetype, "WinAnsiEncoding");
    d = open_fixture(".", truetype, true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 27, 60, 0, 0, 0, 0);
    rgb(&c, 77, 60, 0, 0, 0, 0);
    rgb(&c, 55, 60, 255, 255, 255, 0);
    rgb(&c, 27, 30, 255, 255, 255, 0);
    pdf_close(d);
    write_truetype_pdf(truetype, "MacRomanEncoding");
    open_fixture(".", truetype, false);
    remove(truetype);
    d = open_fixture(root, "graphics.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 10, 90, 255, 127, 127, 1);
    rgb(&c, 70, 90, 127, 127, 255, 1);
    rgb(&c, 25, 25, 125, 130, 0, 5);
    rgb(&c, 35, 65, 0, 255, 0, 0);
    pdf_close(d);
    d = open_fixture(root, "radial.pdf", true);
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 50, 5, 5, 255, 2);
    rgb(&c, 90, 90, 255, 255, 255, 0);
    pdf_close(d);
    const char *unsupported[] = {"font-missing.pdf", "operator-unsupported.pdf",
                                 "malformed-operand.pdf", "image-unsupported.pdf"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) {
        d = open_fixture(root, unsupported[i], true);
        CHECK(!pdf_render(d, 0, &c, &m));
        CHECK(strlen(pdf_error(d)) > 0);
        pdf_close(d);
    }
    CHECK(!open_fixture(root, "broken.pdf", false));
    CHECK(!open_fixture(root, "too-short.pdf", false));
    CHECK(!open_fixture(root, "encrypted.pdf", false));
    canvas_free(&c);
    puts("PDF parsing, images, masks, clipping, fill rules, rotation, links, repeated rendering "
         "and rejection tests passed.");
    return 0;
}
