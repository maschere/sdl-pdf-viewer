/* Verify the native link resolver against development-time extracted expectations. */
#include "pdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: check_links input.pdf expectations.tsv\n");
        return 2;
    }
    char error[256];
    Pdf *pdf = pdf_open(argv[1], error, sizeof(error));
    if (!pdf) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    FILE *f = fopen(argv[2], "r");
    if (!f) {
        perror(argv[2]);
        pdf_close(pdf);
        return 1;
    }
    char row[4096];
    int count = 0;
    while (fgets(row, sizeof(row), f)) {
        int page, target, n = 0;
        double x, y;
        if (sscanf(row, "%d\t%lf\t%lf\t%d\t%n", &page, &x, &y, &target, &n) != 4) {
            fprintf(stderr, "Invalid expectation row\n");
            return 1;
        }
        char *url = row + n;
        url[strcspn(url, "\r\n")] = 0;
        PdfLink link;
        if (!pdf_link_at(pdf, page, (Point){x, y}, &link) || link.page != target ||
            ((target < 0) && (!link.url || strcmp(link.url, url)))) {
            fprintf(stderr, "Link %d on page %d did not match expected destination\n", count + 1,
                    page + 1);
            fclose(f);
            pdf_close(pdf);
            return 1;
        }
        count++;
    }
    fclose(f);
    pdf_close(pdf);
    printf("Verified %d PDF link destinations.\n", count);
    return 0;
}
