#ifndef PDF_H
#define PDF_H
#include "raster.h"
#include "animation.h"
typedef struct Pdf Pdf;
typedef struct {
    int page;
    const char *url;
} PdfLink;
typedef struct {
    const Animation *clip;
    const char *filename;
    Box box; /* Original PDF coordinates, excluding the surrounding fbox. */
    double fps;
    bool autoplay, positioned;
    AnimationInterpolation interpolation;
} PdfAnimation;
Pdf *pdf_open(const char *filename, char *error, size_t error_size);
void pdf_close(Pdf *pdf);
int pdf_page_count(const Pdf *pdf);
/* Destination views intentionally fit the complete target page. */
bool pdf_render(Pdf *pdf, int page, Canvas *canvas, Matrix *pdf_to_canvas);
/* Static page beneath the animation textures; resolves legacy placeholders. */
bool pdf_render_background(Pdf *pdf, int page, Canvas *canvas, Matrix *pdf_to_canvas);
int pdf_animation_count(const Pdf *pdf, int page);
const PdfAnimation *pdf_animation(const Pdf *pdf, int page, int index);
/* Coordinates in the original PDF page coordinate system. */
bool pdf_link_at(Pdf *pdf, int page, Point point, PdfLink *link);
const char *pdf_error(const Pdf *pdf);
#endif
