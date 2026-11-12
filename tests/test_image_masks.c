/* Synthetic image dictionaries exercise masks without shipping lecture assets. */
#include "pdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

#define IMAGE "/Type /XObject /Subtype /Image "
#define RGB IMAGE "/Width 4 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceRGB "
#define GRAY IMAGE "/Width 4 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray "
#define CAL "[/CalRGB << /WhitePoint [.95046 1 1.08906] /Gamma [1 1 1] " \
            "/Matrix [.41239 .21264 .01933 .35758 .71517 .11919 .18048 .07219 .95053] >>]"

static void check_image(const char *dict, const unsigned char *data, size_t size,
                        const char *mask_dict, const unsigned char *mask, size_t mask_size,
                        const int expected[4][3]) {
    const char *filename = "test_image_masks.pdf";
    static const char content[] = "0 0 1 rg 0 0 4 1 re f q 4 0 0 1 0 0 cm /I Do Q";
    struct {
        const char *dict;
        const void *data;
        size_t size;
    } objects[] = {
        {"/Type /Catalog /Pages 2 0 R", NULL, 0},
        {"/Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 4 1]", NULL, 0},
        {"/Type /Page /Parent 2 0 R /Contents 4 0 R "
         "/Resources << /XObject << /I 5 0 R >> >>", NULL, 0},
        {"", content, sizeof(content) - 1},
        {dict, data, size},
        {mask_dict ? mask_dict : "", mask, mask_size},
    };
    long offsets[6];
    FILE *f = fopen(filename, "wb");
    CHECK(f);
    fputs("%PDF-1.4\n", f);
    for (int i = 0; i < 6; i++) {
        offsets[i] = ftell(f);
        fprintf(f, "%d 0 obj\n<< %s", i + 1, objects[i].dict);
        if (objects[i].data) {
            fprintf(f, " /Length %zu >>\nstream\n", objects[i].size);
            CHECK(fwrite(objects[i].data, 1, objects[i].size, f) == objects[i].size);
            fputs("\nendstream", f);
        } else
            fputs(" >>", f);
        fputs("\nendobj\n", f);
    }
    long xref = ftell(f);
    fputs("xref\n0 7\n0000000000 65535 f \n", f);
    for (int i = 0; i < 6; i++)
        fprintf(f, "%010ld 00000 n \n", offsets[i]);
    fprintf(f, "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
    CHECK(!fclose(f));
    char error[256];
    Pdf *pdf = pdf_open(filename, error, sizeof(error));
    CHECK(pdf);
    Canvas c = {0};
    Matrix view;
    CHECK(canvas_init(&c, 4, 1));
    for (int pass = 0; pass < 3; pass++) {
        Pdf *worker = pass == 2 && expected ? pdf_worker_create(pdf) : NULL;
        CHECK(pass != 2 || !expected || worker);
        Pdf *renderer = worker ? worker : pdf;
        bool ok = pdf_render(renderer, 0, &c, &view);
        if (ok != (expected != NULL)) {
            fprintf(stderr, "%s: %s\n", dict, pdf_error(renderer));
            exit(1);
        }
        if (expected)
            for (int x = 0; x < 4; x++)
                for (int k = 0; k < 3; k++) {
                    if (abs(c.pixels[x * 4 + k] - expected[x][k]) > 1) {
                        fprintf(stderr, "%s: pixel %d component %d: got %d, expected %d\n",
                                dict, x, k, c.pixels[x * 4 + k], expected[x][k]);
                        exit(1);
                    }
                }
        else
            CHECK(*pdf_error(pdf));
        pdf_worker_free(worker);
    }
    canvas_free(&c);
    pdf_close(pdf);
    CHECK(!remove(filename));
}

int main(void) {
    /* All component ranges are inclusive and must match simultaneously. */
    const unsigned char colors[] = {10,20,30, 15,25,35, 9,25,35, 15,26,35};
    const int keyed[4][3] = {{0,0,255}, {0,0,255}, {9,25,35}, {15,26,35}};
    check_image(RGB "/Mask [10 15 20 25 30 35]", colors, sizeof(colors), NULL, NULL, 0, keyed);
    /* Masking precedes Decode; /SMask /None still permits a color key. */
    const int inverted[4][3] = {{0,0,255}, {0,0,255}, {246,230,220}, {240,229,220}};
    check_image(RGB "/Mask [10 15 20 25 30 35] /SMask /None /Decode [1 0 1 0 1 0]",
                colors, sizeof(colors), NULL, NULL, 0, inverted);
    /* Packed gray samples at every supported depth use raw integer keys. */
    for (int bits = 1; bits <= 8; bits *= 2) {
        int max = (1 << bits) - 1, samples[] = {0, 1, max - 1, max};
        unsigned char packed[4] = {0};
        int expected[4][3];
        for (int x = 0; x < 4; x++) {
            int bit = x * bits;
            packed[bit / 8] |= (unsigned char)(samples[x] << (8 - bits - bit % 8));
            for (int k = 0; k < 3; k++)
                expected[x][k] = samples[x] == max ? (k == 2 ? 255 : 0)
                                                  : (samples[x] * 255 + max / 2) / max;
        }
        char dict[256];
        snprintf(dict, sizeof(dict), IMAGE "/Width 4 /Height 1 /ColorSpace /DeviceGray "
                 "/BitsPerComponent %d /Mask [%d %d]", bits, max, max);
        check_image(dict, packed, (4 * bits + 7) / 8, NULL, NULL, 0, expected);
    }
    /* The lecture's combination: an Indexed palette with a D65 CalRGB base.
     * Gamma 1 makes mid-gray visibly different from an unconverted RGB byte. */
    const unsigned char indices[] = {0x1b};
    const int palette[4][3] = {{188,188,188}, {255,0,0}, {0,255,0}, {0,0,255}};
    check_image(IMAGE "/Width 4 /Height 1 /BitsPerComponent 2 /ColorSpace "
                "[/Indexed " CAL " 3 <808080ff000000ff00ffffff>] /Mask [3 3]",
                indices, sizeof(indices), NULL, NULL, 0, palette);
    /* Undo a white matte at zero, partial and full alpha. SMask overrides Mask,
     * and its Decode inversion is applied before using it to undo the matte. */
    const unsigned char preblended[] = {255,255,255, 255,191,191, 255,127,127, 255,0,0};
    const unsigned char inverse_alpha[] = {255,191,127,0};
    const int red[4][3] = {{0,0,255}, {64,0,191}, {128,0,127}, {255,0,0}};
    check_image(RGB "/SMask 6 0 R /Mask [0 255 0 255 0 255]", preblended,
                sizeof(preblended), GRAY "/Decode [1 0] /Matte [1 1 1]",
                inverse_alpha, sizeof(inverse_alpha), red);
    const unsigned char gray[] = {255,191,127,0}, alpha[] = {0,64,128,255};
    const int black[4][3] = {{0,0,255}, {0,0,191}, {0,0,127}, {0,0,0}};
    check_image(GRAY "/SMask 6 0 R", gray, sizeof(gray), GRAY "/Matte [1]",
                alpha, sizeof(alpha), black);
    /* CalRGB must be unblended before gamma/matrix conversion; a smaller soft
     * mask without Matte may still supply alpha to a larger image. */
    const unsigned char blend[] = {128,127,255}, half[] = {128};
    const int green[4][3] = {{0,128,127}, {0,128,127}, {0,128,127}, {0,128,127}};
    check_image(IMAGE "/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace " CAL
                " /Decode [1 0 1 0 1 0] /SMask 6 0 R", blend, sizeof(blend),
                IMAGE "/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray "
                "/Matte [1 0 0]", half, sizeof(half), green);
    const unsigned char solid_red[] = {255,0,0, 255,0,0, 255,0,0, 255,0,0};
    const int half_red[4][3] = {{128,0,127}, {128,0,127}, {128,0,127}, {128,0,127}};
    check_image(RGB "/SMask 6 0 R", solid_red, sizeof(solid_red),
                IMAGE "/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray",
                half, sizeof(half), half_red);
    /* Invalid arrays, ranges and matte definitions fail without corrupting
     * the document, including when the same failed page is rendered again. */
    const char *bad_keys[] = {"[0 255]", "[-1 0 0 0 0 0]", "[0 256 0 0 0 0]",
                              "[2 1 0 0 0 0]", "[0 .5 0 0 0 0]"};
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(*bad_keys); i++) {
        char dict[256];
        snprintf(dict, sizeof(dict), RGB "/Mask %s", bad_keys[i]);
        check_image(dict, colors, sizeof(colors), NULL, NULL, 0, NULL);
    }
    check_image(RGB "/SMask 6 0 R", preblended, sizeof(preblended),
                GRAY "/Matte [1]", alpha, sizeof(alpha), NULL);
    check_image(RGB "/SMask 6 0 R", preblended, sizeof(preblended),
                GRAY "/Matte [1 1 2]", alpha, sizeof(alpha), NULL);
    check_image(RGB "/SMask 6 0 R", preblended, sizeof(preblended),
                IMAGE "/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray "
                "/Matte [1 1 1]", half, sizeof(half), NULL);
    puts("Color-key masks, Indexed CalRGB, Matte, Decode, mask precedence, cached/worker "
         "rendering and invalid image rejection passed.");
    return 0;
}
