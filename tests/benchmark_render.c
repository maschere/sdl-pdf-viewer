/* CPU rendering benchmark; excludes file I/O, SDL and texture upload. */
#include "pdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: benchmark_render file.pdf width height repetitions\n");
        return 2;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]), repeats = atoi(argv[4]);
    char error[256];
    Pdf *pdf = pdf_open(argv[1], error, sizeof(error));
    if (!pdf) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    Canvas c = {0};
    Matrix view;
    if (repeats < 1 || !canvas_init(&c, w, h)) {
        pdf_close(pdf);
        return 2;
    }
    /* Optional report of every page, to find the slow ones. The first pass
     * includes image decoding, which the viewer does before showing anything. */
    const char *passes = getenv("BENCHMARK_PER_PAGE");
    for (int pass = 0; passes && pass < atoi(passes); pass++)
        for (int i = 0; i < pdf_page_count(pdf); i++) {
            clock_t begin = clock();
            bool ok = pdf_render(pdf, i, &c, &view);
            printf("pass %d page %d: %.1f ms%s\n", pass + 1, i + 1,
                   1000. * (clock() - begin) / CLOCKS_PER_SEC, ok ? "" : " FAILED");
        }
    clock_t start = clock();
    for (int i = 0; i < repeats; i++)
        if (!pdf_render(pdf, i % pdf_page_count(pdf), &c, &view)) {
            fprintf(stderr, "%s\n", pdf_error(pdf));
            canvas_free(&c);
            pdf_close(pdf);
            return 1;
        }
    double ms = 1000. * (clock() - start) / CLOCKS_PER_SEC / repeats;
    printf("%s: %dx%d, %d renders, %.3f ms/render\n", argv[1], w, h, repeats, ms);
    canvas_free(&c);
    pdf_close(pdf);
    return 0;
}
