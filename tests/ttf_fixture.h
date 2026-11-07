/* Builds a tiny TrueType font in memory (1000 units per em):
 *   'H' -> glyph 1, on-curve square (100,0)-(600,700), advance 700
 *   'O' -> glyph 2, four off-curve points only (implied on-curve midpoints)
 *   'B' -> glyph 3, composite of glyph 1 shifted right by 50 units
 *   ' ' -> glyph 4, empty, advance 300
 * The 'kern' table moves the pair H,H together by 100 units. */
#ifndef TTF_FIXTURE_H
#define TTF_FIXTURE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[2048];
    size_t size;
} TtfBuffer;
static void ttf_put16(TtfBuffer *b, int v) {
    b->data[b->size++] = (uint8_t)((unsigned)v >> 8);
    b->data[b->size++] = (uint8_t)v;
}
static void ttf_put32(TtfBuffer *b, uint32_t v) {
    ttf_put16(b, (int)(v >> 16));
    ttf_put16(b, (int)(v & 0xffff));
}
static void ttf_simple(TtfBuffer *b, const int (*pts)[2], const int *on, int n) {
    int xmin = 9999, ymin = 9999, xmax = -9999, ymax = -9999;
    for (int i = 0; i < n; i++) {
        xmin = pts[i][0] < xmin ? pts[i][0] : xmin;
        xmax = pts[i][0] > xmax ? pts[i][0] : xmax;
        ymin = pts[i][1] < ymin ? pts[i][1] : ymin;
        ymax = pts[i][1] > ymax ? pts[i][1] : ymax;
    }
    ttf_put16(b, 1);
    ttf_put16(b, xmin);
    ttf_put16(b, ymin);
    ttf_put16(b, xmax);
    ttf_put16(b, ymax);
    ttf_put16(b, n - 1); /* endPtsOfContours */
    ttf_put16(b, 0);     /* no instructions */
    for (int i = 0; i < n; i++)
        b->data[b->size++] = (uint8_t)(on[i] ? 1 : 0); /* 16-bit deltas */
    for (int axis = 0; axis < 2; axis++)
        for (int i = 0, last = 0; i < n; i++) {
            ttf_put16(b, pts[i][axis] - last);
            last = pts[i][axis];
        }
    if (b->size & 1)
        b->data[b->size++] = 0;
}
static size_t ttf_fixture(uint8_t *out, size_t cap) {
    static const char *tags[] = {"cmap", "glyf", "head", "hhea", "hmtx", "kern", "loca", "maxp"};
    TtfBuffer t[8];
    memset(t, 0, sizeof(t));
    /* cmap: one format 4 subtable (3,1) with delta-mapped single-code segments. */
    TtfBuffer *b = &t[0];
    static const int codes[][2] = {{0x20, 4}, {0x42, 3}, {0x48, 1}, {0x4f, 2}, {0xffff, 0}};
    ttf_put16(b, 0);
    ttf_put16(b, 1);
    ttf_put16(b, 3);
    ttf_put16(b, 1);
    ttf_put32(b, 12);
    ttf_put16(b, 4);
    ttf_put16(b, 16 + 5 * 8);
    ttf_put16(b, 0);
    ttf_put16(b, 10); /* segCountX2 */
    ttf_put16(b, 8);
    ttf_put16(b, 2);
    ttf_put16(b, 2);
    for (int i = 0; i < 5; i++)
        ttf_put16(b, codes[i][0]);
    ttf_put16(b, 0);
    for (int i = 0; i < 5; i++)
        ttf_put16(b, codes[i][0]);
    for (int i = 0; i < 5; i++)
        ttf_put16(b, i == 4 ? 1 : (codes[i][1] - codes[i][0]) & 0xffff);
    for (int i = 0; i < 5; i++)
        ttf_put16(b, 0);
    /* glyf + loca (short offsets). */
    b = &t[1];
    int loca[6];
    loca[0] = loca[1] = 0;
    static const int square[4][2] = {{100, 0}, {100, 700}, {600, 700}, {600, 0}};
    static const int all_on[4] = {1, 1, 1, 1}, all_off[4] = {0, 0, 0, 0};
    static const int rounded[4][2] = {{100, 100}, {100, 600}, {600, 600}, {600, 100}};
    ttf_simple(b, square, all_on, 4);
    loca[2] = (int)b->size;
    ttf_simple(b, rounded, all_off, 4);
    loca[3] = (int)b->size;
    ttf_put16(b, -1);
    ttf_put16(b, 150);
    ttf_put16(b, 0);
    ttf_put16(b, 650);
    ttf_put16(b, 700);
    ttf_put16(b, 0x0003); /* ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES */
    ttf_put16(b, 1);
    ttf_put16(b, 50);
    ttf_put16(b, 0);
    loca[4] = loca[5] = (int)b->size;
    for (int i = 0; i < 6; i++)
        ttf_put16(&t[6], loca[i] / 2);
    /* head */
    b = &t[2];
    for (int i = 0; i < 54; i++)
        b->data[i] = 0;
    b->size = 54;
    b->data[0] = 0;
    b->data[1] = 1;
    b->data[18] = 1000 >> 8;
    b->data[19] = 1000 & 255;
    /* hhea */
    b = &t[3];
    ttf_put32(b, 0x00010000);
    ttf_put16(b, 800);
    ttf_put16(b, -200);
    ttf_put16(b, 0);
    while (b->size < 34)
        ttf_put16(b, 0);
    ttf_put16(b, 5);
    /* hmtx */
    static const int adv[5] = {500, 700, 700, 700, 300};
    for (int i = 0; i < 5; i++) {
        ttf_put16(&t[4], adv[i]);
        ttf_put16(&t[4], 0);
    }
    /* kern: version 0, one format 0 horizontal subtable with one pair. */
    b = &t[5];
    ttf_put16(b, 0);
    ttf_put16(b, 1);
    ttf_put16(b, 0);
    ttf_put16(b, 20);
    ttf_put16(b, 0x0001);
    ttf_put16(b, 1);
    ttf_put16(b, 6);
    ttf_put16(b, 0);
    ttf_put16(b, 0);
    ttf_put16(b, 1);
    ttf_put16(b, 1);
    ttf_put16(b, -100);
    /* maxp 0.5 */
    ttf_put32(&t[7], 0x00005000);
    ttf_put16(&t[7], 5);
    /* Offset table and directory. */
    TtfBuffer *font = (TtfBuffer *)calloc(1, sizeof(TtfBuffer));
    if (!font)
        return 0;
    font->size = 0;
    ttf_put32(font, 0x00010000);
    ttf_put16(font, 8);
    ttf_put16(font, 128);
    ttf_put16(font, 3);
    ttf_put16(font, 0);
    uint32_t offset = 12 + 16 * 8;
    for (int i = 0; i < 8; i++) {
        memcpy(font->data + font->size, tags[i], 4);
        font->size += 4;
        ttf_put32(font, 0);
        ttf_put32(font, offset);
        ttf_put32(font, (uint32_t)t[i].size);
        offset += (uint32_t)((t[i].size + 3) & ~(size_t)3);
    }
    for (int i = 0; i < 8; i++) {
        memcpy(font->data + font->size, t[i].data, t[i].size);
        font->size += t[i].size;
        while (font->size & 3)
            font->data[font->size++] = 0;
    }
    size_t size = font->size <= cap ? font->size : 0;
    if (size)
        memcpy(out, font->data, size);
    free(font);
    return size;
}
#endif
