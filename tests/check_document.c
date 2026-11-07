#include "pdf.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: check_document file.pdf\n");
        return 2;
    }
    char error[256];
    Pdf *pdf = pdf_open(argv[1], error, sizeof(error));
    if (!pdf) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    Canvas c = {0};
    Matrix view;
    if (!canvas_init(&c, 800, 500)) {
        pdf_close(pdf);
        return 1;
    }
    for (int i = 0; i < pdf_page_count(pdf); i++)
        if (!pdf_render(pdf, i, &c, &view)) {
            fprintf(stderr, "Page %d: %s\n", i + 1, pdf_error(pdf));
            canvas_free(&c);
            pdf_close(pdf);
            return 1;
        }
    printf("Rendered all %d pages at 800x500.\n", pdf_page_count(pdf));
    canvas_free(&c);
    pdf_close(pdf);
    return 0;
}
