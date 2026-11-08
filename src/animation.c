#include "animation.h"
#include <gif_lib.h>
#include <png.h>
#include <zlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MAX_FRAMES 4096
#define MAX_PIXELS 16000000u
#define MAX_FILE (64u * 1024u * 1024u)

static bool message(char *error, size_t n, const char *s) {
    if (n)
        snprintf(error, n, "%s", s);
    return false;
}
void animation_free(Animation *a) {
    if (!a)
        return;
    for (int i = 0; i < a->count; i++)
        free(a->frames[i]);
    free(a->frames);
    free(a);
}
static bool dimensions(unsigned w, unsigned h) {
    return w && h && w <= 16384 && h <= 16384 && (size_t)w * h <= MAX_PIXELS;
}
static bool append_frame(Animation *a, const uint8_t *rgba, size_t budget) {
    size_t bytes = (size_t)a->w * a->h * 4;
    if (a->count >= MAX_FRAMES || a->bytes > budget || bytes > budget - a->bytes)
        return false;
    uint8_t **frames = realloc(a->frames, (size_t)(a->count + 1) * sizeof(*frames));
    if (!frames)
        return false;
    a->frames = frames;
    uint8_t *frame = malloc(bytes);
    if (!frame)
        return false;
    memcpy(frame, rgba, bytes);
    a->frames[a->count++] = frame;
    a->bytes += bytes;
    return true;
}
typedef struct {
    const uint8_t *data;
    size_t size, at;
} GifInput;
static int gif_read(GifFileType *gif, GifByteType *out, int n) {
    GifInput *in = gif->UserData;
    size_t count = MIN((size_t)MAX(0, n), in->size - in->at);
    memcpy(out, in->data + in->at, count);
    in->at += count;
    return (int)count;
}
static void gif_background(uint8_t *out, size_t pixels, const GifFileType *gif, bool transparent) {
    GifColorType color = {0};
    if (gif->SColorMap && gif->SBackGroundColor >= 0 &&
        gif->SBackGroundColor < gif->SColorMap->ColorCount)
        color = gif->SColorMap->Colors[gif->SBackGroundColor];
    for (size_t i = 0; i < pixels; i++) {
        out[4 * i] = transparent ? 0 : color.Red;
        out[4 * i + 1] = transparent ? 0 : color.Green;
        out[4 * i + 2] = transparent ? 0 : color.Blue;
        out[4 * i + 3] = transparent ? 0 : 255;
    }
}
static bool decode_gif(Animation *a, const uint8_t *data, size_t size, size_t budget, char *error,
                       size_t n) {
    GifInput input = {data, size, 0};
    int code = 0;
    GifFileType *gif = DGifOpen(&input, gif_read, &code);
    uint8_t *canvas = NULL, *saved = NULL, *row = NULL;
    const char *failure = "Invalid or truncated GIF";
    bool ok = false;
    if (!gif)
        goto done;
    if (!dimensions((unsigned)gif->SWidth, (unsigned)gif->SHeight)) {
        failure = "GIF dimensions exceed limits";
        goto done;
    }
    a->w = gif->SWidth;
    a->h = gif->SHeight;
    size_t bytes = (size_t)a->w * a->h * 4;
    if (bytes > budget) {
        failure = "Animation memory limit exceeded";
        goto done;
    }
    canvas = calloc(1, bytes);
    saved = malloc(bytes);
    row = malloc((size_t)a->w);
    if (!canvas || !saved || !row) {
        failure = "Out of animation memory";
        goto done;
    }
    GraphicsControlBlock control = {0, false, 0, NO_TRANSPARENT_COLOR};
    for (;;) {
        GifRecordType type;
        if (DGifGetRecordType(gif, &type) == GIF_ERROR)
            goto done;
        if (type == TERMINATE_RECORD_TYPE) {
            ok = a->count > 0;
            break;
        }
        if (type == EXTENSION_RECORD_TYPE) {
            GifByteType *block;
            int function;
            if (DGifGetExtension(gif, &function, &block) == GIF_ERROR)
                goto done;
            if (function == GRAPHICS_EXT_FUNC_CODE && block) {
                if (block[0] != 4 || DGifExtensionToGCB(4, block + 1, &control) == GIF_ERROR ||
                    control.DisposalMode > DISPOSE_PREVIOUS)
                    goto done;
            }
            while (block)
                if (DGifGetExtensionNext(gif, &block) == GIF_ERROR)
                    goto done;
        } else if (type == IMAGE_DESC_RECORD_TYPE) {
            if (a->count >= MAX_FRAMES || bytes > budget - a->bytes) {
                failure = "Animation frame/memory limit exceeded";
                goto done;
            }
            if (DGifGetImageDesc(gif) == GIF_ERROR)
                goto done;
            GifImageDesc *frame = &gif->Image;
            if (frame->Width < 1 || frame->Height < 1 || frame->Left < 0 || frame->Top < 0 ||
                frame->Width > a->w - frame->Left || frame->Height > a->h - frame->Top)
                goto done;
            ColorMapObject *palette = frame->ColorMap ? frame->ColorMap : gif->SColorMap;
            if (!palette)
                goto done;
            if (!a->count)
                gif_background(canvas, bytes / 4, gif, control.TransparentColor >= 0);
            if (control.DisposalMode == DISPOSE_PREVIOUS)
                memcpy(saved, canvas, bytes);
            static const int starts[] = {0, 4, 2, 1}, steps[] = {8, 8, 4, 2};
            for (int pass = 0; pass < (frame->Interlace ? 4 : 1); pass++)
                for (int y = frame->Interlace ? starts[pass] : 0; y < frame->Height;
                     y += frame->Interlace ? steps[pass] : 1) {
                    if (DGifGetLine(gif, row, frame->Width) == GIF_ERROR)
                        goto done;
                    uint8_t *dest = canvas + ((size_t)(frame->Top + y) * a->w + frame->Left) * 4;
                    for (int x = 0; x < frame->Width; x++) {
                        unsigned index = row[x];
                        if ((int)index == control.TransparentColor)
                            continue;
                        if (index >= (unsigned)palette->ColorCount)
                            goto done;
                        GifColorType color = palette->Colors[index];
                        dest[4 * x] = color.Red;
                        dest[4 * x + 1] = color.Green;
                        dest[4 * x + 2] = color.Blue;
                        dest[4 * x + 3] = 255;
                    }
                }
            if (!append_frame(a, canvas, budget)) {
                failure = "Animation memory limit exceeded";
                goto done;
            }
            if (control.DisposalMode == DISPOSE_PREVIOUS)
                memcpy(canvas, saved, bytes);
            else if (control.DisposalMode == DISPOSE_BACKGROUND)
                for (int y = 0; y < frame->Height; y++)
                    gif_background(canvas + ((size_t)(frame->Top + y) * a->w + frame->Left) * 4,
                                   (size_t)frame->Width, gif, control.TransparentColor >= 0);
            control = (GraphicsControlBlock){0, false, 0, NO_TRANSPARENT_COLOR};
        } else
            goto done;
    }
done:
    if (gif)
        DGifCloseFile(gif, &code);
    free(canvas);
    free(saved);
    free(row);
    return ok || message(error, n, failure);
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
typedef struct {
    uint8_t *data;
    size_t size;
} Buffer;
static bool add_chunk(Buffer *b, const char *type, const uint8_t *data, size_t size) {
    if (size > MAX_FILE || b->size > MAX_FILE - size || b->size + size > MAX_FILE - 12)
        return false;
    uint8_t *p = realloc(b->data, b->size + size + 12);
    if (!p)
        return false;
    b->data = p;
    p += b->size;
    put32(p, (uint32_t)size);
    memcpy(p + 4, type, 4);
    if (size)
        memcpy(p + 8, data, size);
    put32(p + 8 + size, (uint32_t)crc32(0, p + 4, (uInt)(size + 4)));
    b->size += size + 12;
    return true;
}
typedef struct {
    unsigned w, h, x, y, dispose, blend;
} PngFrame;
static bool png_frame(Animation *a, Buffer *png, PngFrame f, uint8_t *canvas, uint8_t *saved,
                      size_t budget, char *error, size_t n) {
    if (!add_chunk(png, "IEND", NULL, 0))
        return message(error, n, "PNG frame too large");
    png_image image = {0};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, png->data, png->size)) {
        message(error, n, image.message);
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    if (image.width != f.w || image.height != f.h) {
        png_image_free(&image);
        return message(error, n, "Incorrect PNG frame dimensions");
    }
    uint8_t *rgba = malloc((size_t)f.w * f.h * 4);
    if (!rgba) {
        png_image_free(&image);
        return message(error, n, "Out of PNG memory");
    }
    if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        message(error, n, image.message);
        png_image_free(&image);
        free(rgba);
        return false;
    }
    png_image_free(&image);
    size_t bytes = (size_t)a->w * a->h * 4;
    if (f.dispose == 2)
        memcpy(saved, canvas, bytes);
    for (unsigned y = 0; y < f.h; y++)
        for (unsigned x = 0; x < f.w; x++) {
            uint8_t *dest = canvas + ((size_t)(f.y + y) * a->w + f.x + x) * 4;
            const uint8_t *src = rgba + ((size_t)y * f.w + x) * 4;
            if (!f.blend)
                memcpy(dest, src, 4);
            else {
                unsigned sa = src[3], da = dest[3], alpha = sa * 255 + da * (255 - sa);
                for (int k = 0; k < 3; k++)
                    dest[k] = alpha ? (uint8_t)((src[k] * sa * 255 + dest[k] * da * (255 - sa) +
                                                 alpha / 2) /
                                                alpha)
                                    : 0;
                dest[3] = (uint8_t)((alpha + 127) / 255);
            }
        }
    free(rgba);
    if (!append_frame(a, canvas, budget))
        return message(error, n, "Animation frame/memory limit exceeded");
    if (f.dispose == 2)
        memcpy(canvas, saved, bytes);
    else if (f.dispose == 1)
        for (unsigned y = 0; y < f.h; y++)
            memset(canvas + ((size_t)(f.y + y) * a->w + f.x) * 4, 0, (size_t)f.w * 4);
    return true;
}
static bool decode_png(Animation *a, const uint8_t *data, size_t size, size_t budget, char *error,
                       size_t n) {
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    Buffer common = {0}, frame = {0};
    uint8_t header[13] = {0}, *canvas = NULL, *saved = NULL;
    unsigned declared = 0, sequence = 0;
    bool ihdr = false, actl = false, idat = false, have_frame = false, have_data = false,
         ended = false, ok = false;
    PngFrame control = {0};
    const char *failure = "Invalid or truncated APNG";
    for (size_t at = 8; at < size;) {
        if (size - at < 12)
            goto done;
        unsigned length = be32(data + at);
        if (length > size - at - 12)
            goto done;
        const uint8_t *type = data + at + 4, *chunk = data + at + 8;
        if ((uint32_t)crc32(0, type, length + 4) != be32(chunk + length)) {
            failure = "APNG chunk CRC mismatch";
            goto done;
        }
        if (!ihdr && memcmp(type, "IHDR", 4))
            goto done;
        if (!memcmp(type, "IHDR", 4)) {
            if (ihdr || length != 13 || !dimensions(be32(chunk), be32(chunk + 4)))
                goto done;
            memcpy(header, chunk, 13);
            a->w = (int)be32(chunk);
            a->h = (int)be32(chunk + 4);
            ihdr = true;
            size_t bytes = (size_t)a->w * a->h * 4;
            if (bytes > budget) {
                failure = "Animation memory limit exceeded";
                goto done;
            }
            canvas = calloc(1, bytes);
            saved = malloc(bytes);
            if (!canvas || !saved) {
                failure = "Out of APNG memory";
                goto done;
            }
        } else if (!memcmp(type, "acTL", 4)) {
            if (actl || idat || length != 8)
                goto done;
            declared = be32(chunk);
            if (!declared || declared > MAX_FRAMES || (size_t)a->w * a->h * 4 > budget / declared) {
                failure = "Animation frame/memory limit exceeded";
                goto done;
            }
            actl = true;
        } else if (!memcmp(type, "fcTL", 4)) {
            if (!actl || length != 26 || be32(chunk) != sequence++)
                goto done;
            if (have_frame) {
                if (!have_data)
                    goto done;
                if (!png_frame(a, &frame, control, canvas, saved, budget, error, n))
                    goto decoder_error;
            }
            control = (PngFrame){be32(chunk + 4),  be32(chunk + 8), be32(chunk + 12),
                                 be32(chunk + 16), chunk[24],       chunk[25]};
            if (!dimensions(control.w, control.h) || control.x > (unsigned)a->w ||
                control.y > (unsigned)a->h || control.w > (unsigned)a->w - control.x ||
                control.h > (unsigned)a->h - control.y || control.dispose > 2 || control.blend > 1)
                goto done;
            if (!idat && (control.x || control.y || control.w != (unsigned)a->w ||
                          control.h != (unsigned)a->h))
                goto done;
            if (!a->count && control.dispose == 2)
                control.dispose = 1;
            free(frame.data);
            frame = (Buffer){0};
            frame.data = malloc(8);
            if (!frame.data) {
                failure = "Out of APNG memory";
                goto done;
            }
            memcpy(frame.data, signature, 8);
            frame.size = 8;
            uint8_t fh[13];
            memcpy(fh, header, 13);
            put32(fh, control.w);
            put32(fh + 4, control.h);
            if (!add_chunk(&frame, "IHDR", fh, 13))
                goto done;
            if (common.size) {
                uint8_t *p = realloc(frame.data, frame.size + common.size);
                if (!p) {
                    failure = "Out of APNG memory";
                    goto done;
                }
                frame.data = p;
                memcpy(p + frame.size, common.data, common.size);
                frame.size += common.size;
            }
            have_frame = true;
            have_data = false;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!actl) {
                failure = "PNG attachment is not animated";
                goto done;
            }
            if (a->count)
                goto done;
            idat = true;
            /* With no preceding fcTL, IDAT is a separate poster, not frame 0. */
            if (have_frame) {
                if (!add_chunk(&frame, "IDAT", chunk, length))
                    goto done;
                have_data = true;
            }
        } else if (!memcmp(type, "fdAT", 4)) {
            if (!have_frame || !idat || length < 4 || be32(chunk) != sequence++)
                goto done;
            if (!add_chunk(&frame, "IDAT", chunk + 4, length - 4))
                goto done;
            have_data = true;
        } else if (!memcmp(type, "IEND", 4)) {
            if (length || !have_frame || !have_data || at + 12 != size)
                goto done;
            if (!png_frame(a, &frame, control, canvas, saved, budget, error, n))
                goto decoder_error;
            ended = true;
            break;
        } else if (!idat && (!memcmp(type, "PLTE", 4) || !memcmp(type, "tRNS", 4) ||
                             !memcmp(type, "gAMA", 4) || !memcmp(type, "cHRM", 4) ||
                             !memcmp(type, "sRGB", 4))) {
            if (!add_chunk(&common, (const char *)type, chunk, length))
                goto done;
            /* Global palette/color chunks may follow the first fcTL. */
            if (have_frame && !add_chunk(&frame, (const char *)type, chunk, length))
                goto done;
        } else if (!(type[0] & 32)) {
            failure = "Unsupported critical PNG chunk";
            goto done;
        }
        at += (size_t)length + 12;
    }
    ok = ended && declared == (unsigned)a->count;
done:
    if (!ok)
        message(error, n, failure);
decoder_error:
    free(common.data);
    free(frame.data);
    free(canvas);
    free(saved);
    return ok;
}
Animation *animation_decode(const uint8_t *data, size_t size, size_t budget, char *error,
                            size_t n) {
    if (!data || size < 8 || size > MAX_FILE) {
        message(error, n, "Invalid animation size");
        return NULL;
    }
    Animation *a = calloc(1, sizeof(*a));
    if (!a) {
        message(error, n, "Out of animation memory");
        return NULL;
    }
    bool ok;
    if (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6))
        ok = decode_gif(a, data, size, budget, error, n);
    else if (!memcmp(data, "\211PNG\r\n\032\n", 8))
        ok = decode_png(a, data, size, budget, error, n);
    else
        ok = message(error, n, "Attachment is not GIF or APNG");
    if (!ok) {
        animation_free(a);
        return NULL;
    }
    return a;
}

void animation_player_init(AnimationPlayer *p, int count, double fps, bool autoplay, uint64_t now) {
    if (!isfinite(fps) || fps < .1 || fps > 240)
        fps = 24;
    *p = (AnimationPlayer){0, count, autoplay && count > 1, (uint64_t)(1e9 / fps + .5), 0};
    p->next = now + p->interval;
}
bool animation_player_tick(AnimationPlayer *p, uint64_t now) {
    if (!p->playing || p->count < 2 || now < p->next)
        return false;
    uint64_t steps = 1 + (now - p->next) / p->interval;
    p->frame = (int)((p->frame + steps % (unsigned)p->count) % (unsigned)p->count);
    p->next += steps * p->interval;
    return true;
}
void animation_player_toggle(AnimationPlayer *p, uint64_t now) {
    animation_player_tick(p, now);
    p->playing = !p->playing && p->count > 1;
    p->next = now + p->interval;
}
int animation_player_wait(const AnimationPlayer *p, uint64_t now) {
    if (!p->playing)
        return -1;
    if (now >= p->next)
        return 0;
    uint64_t ms = (p->next - now + 999999) / 1000000;
    return (int)MIN(ms, 10000);
}
