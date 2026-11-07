#include "pdf.h"
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
int main(int argc, char **argv) {
    CHECK(argc >= 2);
    const char *root = argv[1];
    Pdf *d = open_fixture(root, "demo.pdf", true);
    CHECK(pdf_page_count(d) == 2);
    Canvas c = {0};
    Matrix m;
    CHECK(canvas_init(&c, 400, 300));
    CHECK(pdf_render(d, 0, &c, &m));
    rgb(&c, 50, 170, 255, 0, 0, 0);
    rgb(&c, 110, 170, 0, 255, 0, 0);
    rgb(&c, 50, 230, 0, 0, 255, 0);
    rgb(&c, 110, 230, 255, 255, 0, 0);
    rgb(&c, 200, 200, 255, 0, 255, 3);
    rgb(&c, 320, 210, 127, 127, 255, 1);
    rgb(&c, 25, 280, 255, 0, 0, 0);
    rgb(&c, 55, 280, 0, 255, 0, 0);
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
