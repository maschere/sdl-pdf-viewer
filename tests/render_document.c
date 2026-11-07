/* Optional acceptance-test helper: render every page without SDL or external PDF code. */
#include "pdf.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: render_document input.pdf existing-output-directory\n");
        return 2;
    }
    char error[256];
    Pdf *pdf = pdf_open(argv[1], error, sizeof(error));
    if (!pdf) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    Canvas canvas = {0};
    Matrix transform;
    if (!canvas_init(&canvas, 1600, 1000)) {
        pdf_close(pdf);
        return 1;
    }
    for (int page = 0; page < pdf_page_count(pdf); page++) {
        if (!pdf_render(pdf, page, &canvas, &transform)) {
            fprintf(stderr, "Page %d: %s\n", page + 1, pdf_error(pdf));
            canvas_free(&canvas);
            pdf_close(pdf);
            return 1;
        }
        char filename[2048];
        snprintf(filename, sizeof(filename), "%s/page-%03d.ppm", argv[2], page + 1);
        FILE *f = fopen(filename, "wb");
        if (!f) {
            perror(filename);
            return 1;
        }
        fprintf(f, "P6\n800 500\n255\n");
        for (int y = 0; y < 500; y++)
            for (int x = 0; x < 800; x++) {
                unsigned char rgb[3];
                for (int k = 0; k < 3; k++) {
                    unsigned sum = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++)
                            sum +=
                                canvas.pixels[((size_t)(2 * y + dy) * 1600 + 2 * x + dx) * 4 + k];
                    rgb[k] = (unsigned char)((sum + 2) / 4);
                }
                if (fwrite(rgb, 1, 3, f) != 3)
                    return 1;
            }
        if (fclose(f))
            return 1;
        printf("Rendered page %d/%d\n", page + 1, pdf_page_count(pdf));
        fflush(stdout);
    }
    canvas_free(&canvas);
    pdf_close(pdf);
    return 0;
}
