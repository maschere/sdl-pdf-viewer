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
static const char *root;
static uint8_t *read_fixture(const char *name, size_t *size) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long length = ftell(f);
    CHECK(length > 0);
    CHECK(!fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)length);
    CHECK(data);
    CHECK(fread(data, 1, (size_t)length, f) == (size_t)length);
    CHECK(!fclose(f));
    *size = (size_t)length;
    return data;
}
static Animation *load(const char *name) {
    size_t size;
    uint8_t *data = read_fixture(name, &size);
    char error[256];
    Animation *a = animation_decode(data, size, 128u * 1024u * 1024u, error, sizeof(error));
    if (!a)
        fprintf(stderr, "%s: %s\n", name, error);
    free(data);
    CHECK(a);
    return a;
}
static const uint8_t *pixel(const Animation *a, int frame, int x, int y) {
    return a->frames[frame] + ((size_t)y * a->w + x) * 4;
}
static uint32_t hash(const Animation *a) {
    uint32_t value = 2166136261u;
    for (int f = 0; f < a->count; f++)
        for (size_t i = 0; i < (size_t)a->w * a->h; i++) {
            const uint8_t *p = a->frames[f] + i * 4;
            for (int k = 0; k < 4; k++)
                value = (value ^ (k < 3 && !p[3] ? 0 : p[k])) * 16777619u;
        }
    return value;
}
static void decode_tests(void) {
    Animation *g = load("animation-disposal.gif"), *a = load("animation-disposal.apng"),
              *poster = load("animation-poster.apng");
    CHECK(g->w == 4 && g->h == 4 && g->count == 4);
    CHECK(pixel(g, 0, 0, 0)[0] == 255 && pixel(g, 0, 3, 0)[3] == 0);
    CHECK(pixel(g, 1, 2, 0)[1] == 255 && pixel(g, 1, 0, 0)[0] == 255);
    CHECK(pixel(g, 2, 2, 0)[3] == 0 && pixel(g, 2, 0, 2)[2] == 255);
    CHECK(pixel(g, 3, 0, 2)[3] == 0 && pixel(g, 3, 3, 3)[1] == 255);
    CHECK(a->w == 4 && a->h == 4 && a->count == 5 && poster->count == 5);
    CHECK(pixel(a, 0, 0, 0)[0] == 255 && pixel(a, 0, 3, 0)[3] == 0);
    CHECK(pixel(a, 1, 2, 0)[1] == 255);
    CHECK(pixel(a, 2, 2, 0)[3] == 0 && pixel(a, 2, 0, 2)[2] == 255);
    CHECK(pixel(a, 3, 0, 2)[3] == 0);
    CHECK(pixel(a, 3, 0, 0)[0] == 127 && pixel(a, 3, 0, 0)[1] == 128 &&
          pixel(a, 3, 0, 0)[3] == 255);
    CHECK(pixel(a, 4, 1, 1)[3] == 0);
    for (int i = 0; i < a->count; i++)
        CHECK(!memcmp(a->frames[i], poster->frames[i], 4 * 4 * 4));
    animation_free(g);
    animation_free(a);
    animation_free(poster);
    size_t size;
    uint8_t *data = read_fixture("animation-disposal.apng", &size);
    char error[256];
    CHECK(
        !animation_decode(data, size, 4 * 4 * 4, error, sizeof(error))); /* bounded frame storage */
    CHECK(!animation_decode(data, size - 8, 1024 * 1024, error, sizeof(error)));
    data[45] ^= 1;
    CHECK(!animation_decode(data, size, 1024 * 1024, error, sizeof(error))); /* CRC */
    free(data);
    data = read_fixture("animation-disposal.gif", &size);
    CHECK(!animation_decode(data, size - 5, 1024 * 1024, error, sizeof(error)));
    CHECK(!animation_decode(data, size, 32, error, sizeof(error)));
    free(data);
}
static void player_tests(void) {
    AnimationPlayer p;
    animation_player_init(&p, 32, 20, false, 0);
    CHECK(!p.playing && p.frame == 0 && animation_player_wait(&p, 0) == -1);
    CHECK(!animation_player_tick(&p, 1000000000));
    animation_player_toggle(&p, 1000000000);
    CHECK(p.playing && animation_player_wait(&p, 1000000000) == 50);
    CHECK(!animation_player_tick(&p, 1049999999));
    CHECK(animation_player_tick(&p, 1050000000) && p.frame == 1);
    CHECK(animation_player_tick(&p, 1250000000) && p.frame == 5);
    animation_player_toggle(&p, 1250000000);
    CHECK(!p.playing);
    CHECK(!animation_player_tick(&p, 9999999999ULL) && p.frame == 5);
    animation_player_toggle(&p, 10000000000ULL);
    CHECK(!animation_player_tick(&p, 10049999999ULL));
    CHECK(animation_player_tick(&p, 10050000000ULL) && p.frame == 6);
    animation_player_init(&p, 24, 29.97, true, 0);
    CHECK(p.playing && p.interval == 33366700);
    CHECK(animation_player_tick(&p, p.next + 25 * p.interval) && p.frame == 2);
}
static void interpolation_tests(void) {
    AnimationScaler s = {0};
    uint8_t source[8] = {255, 0, 0, 255, 0, 0, 255, 255};
    CHECK(animation_scale_bicubic(&s, source, 2, 1, 8, 1));
    CHECK(s.pixels[0] == 255 && s.pixels[2] == 0);
    CHECK(s.pixels[3 * 4] > 0 && s.pixels[3 * 4 + 2] > 0);
    for (int x = 0; x < 8; x++)
        CHECK(s.pixels[x * 4 + 3] == 255);
    source[7] = 0; /* A transparent blue neighbor must not create a blue halo. */
    CHECK(animation_scale_bicubic(&s, source, 2, 1, 8, 1));
    for (int x = 0; x < 8; x++) {
        CHECK(s.pixels[x * 4 + 2] == 0);
        if (s.pixels[x * 4 + 3])
            CHECK(s.pixels[x * 4] == 255);
    }
    CHECK(!animation_scale_bicubic(&s, source, 2, 1, 16384, 16384));
    animation_scaler_free(&s);
}
static void pdf_tests(void) {
    char path[2048], error[256];
    Canvas c = {0};
    Matrix view;
    snprintf(path, sizeof(path), "%s/animation-metadata.pdf", root);
    Pdf *d = pdf_open(path, error, sizeof(error));
    CHECK(d);
    CHECK(pdf_animation_count(d, 0) == 2);
    const PdfAnimation *a = pdf_animation(d, 0, 0), *b = pdf_animation(d, 0, 1);
    CHECK(a->fps == 12 && !a->autoplay && a->interpolation == ANIMATION_NEAREST);
    CHECK(fabs(b->fps - 29.97) < 1e-6 && b->autoplay && b->interpolation == ANIMATION_BICUBIC);
    CHECK(a->positioned && b->positioned && a->box.x0 == 10 && b->box.y1 == 180);
    CHECK(canvas_init(&c, 400, 400));
    CHECK(pdf_render(d, 0, &c, &view)); /* Includes 90-degree rotation. */
    Point point = matrix_point(view, (Point){20, 70});
    const uint8_t *p = c.pixels + ((size_t)(int)point.y * c.w + (int)point.x) * 4;
    CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);
    pdf_close(d);
    canvas_free(&c);
    snprintf(path, sizeof(path), "%s/testslides.pdf", root);
    d = pdf_open(path, error, sizeof(error));
    if (!d)
        fprintf(stderr, "%s\n", error);
    CHECK(d && pdf_page_count(d) == 5);
    CHECK(pdf_animation_count(d, 1) == 1 && pdf_animation_count(d, 2) == 1);
    a = pdf_animation(d, 1, 0);
    b = pdf_animation(d, 2, 0);
    CHECK(a->clip->w == 112 && a->clip->count == 24 && b->clip->w == 64 && b->clip->count == 32);
    /* Independent Pillow reference hashes, all RGBA frames, transparent RGB normalized. */
    CHECK(hash(a->clip) == 0xcaf066b7u);
    CHECK(hash(b->clip) == 0x38c1f77eu);
    CHECK(canvas_init(&c, 1000, 625));
    CHECK(pdf_render_background(d, 1, &c, &view));
    CHECK(a->positioned && fabs(a->box.x0 - 20.603) < .002 && fabs(a->box.y0 - 82.349) < .002);
    CHECK(fabs(a->box.x1 - a->box.x0 - 141.732) < .002 &&
          fabs(a->box.y1 - a->box.y0 - 141.732) < .002);
    CHECK(pdf_render_background(d, 2, &c, &view));
    CHECK(b->positioned && fabs(b->box.x0 - 105.642) < .002 && fabs(b->box.y0 - 82.416) < .002);
    CHECK(fabs(b->box.x1 - b->box.x0 - 141.732) < .002);
    pdf_close(d);
    canvas_free(&c);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    root = argv[1];
    decode_tests();
    player_tests();
    interpolation_tests();
    pdf_tests();
    puts("GIF/APNG disposal and blending, poster frames, memory bounds, CRC, fixed-FPS timing, "
         "bicubic alpha, metadata, rotation and all 56 real slide frames passed.");
    return 0;
}
