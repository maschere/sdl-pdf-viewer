#include "pdf.h"
#include "cff.h"
#include "cmyk_table.h"
#include "text.h"
#include "type1.h"
#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* libjpeg requires stdio.h and stddef.h to be included first. */
#include <jpeglib.h>
#define FILE_LIMIT (64u * 1024u * 1024u)
#define MEMORY_LIMIT (256u * 1024u * 1024u)
/* Decoded images kept for the whole session, so slides never decode twice. */
#define IMAGE_CACHE_LIMIT (sizeof(size_t) > 4 ? (size_t)2048u * 1024u * 1024u : (size_t)256u * 1024u * 1024u)
#define OBJECT_LIMIT 100000
#define ARRAY_LIMIT 200000
#define DEPTH_LIMIT 64
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef enum { NIL, NUMBER, NAME, STRING, ARRAY, DICT, REF, WORD } Kind;
typedef struct Value Value;
typedef struct Entry {
    Value *key, *value;
    struct Entry *next;
} Entry;
struct Value {
    Kind kind;
    double number;
    char *text;
    size_t len;
    Value **items;
    size_t count;
    Entry *entries;
    int ref, gen;
    const unsigned char *stream;
    size_t stream_len;
    unsigned char *decoded;
    size_t decoded_len;
    unsigned char *rgba; /* Decoded image, owned by the document once cached. */
};
/* One bounded arena owns permanent objects; frame allocations roll back after rendering. */
typedef struct Allocation {
    struct Allocation *next;
    size_t size;
} Allocation;
typedef struct {
    size_t offset;
    int gen, state;
    Value *value;
    bool used;
    int stream_id, stream_index;
} Object;
typedef struct {
    PdfAnimation info;
    Box anchor;
    bool legacy, placed;
} AttachedAnimation;
typedef struct {
    Value *object, *resources;
    Box box;
    int rotate;
    AttachedAnimation *animations;
    int animation_count;
} Page;
typedef struct PdfFont {
    Cff *cff;           /* Exactly one of cff, truetype and type1 is set. */
    TextFont *truetype; /* Simple font: character codes are WinAnsi. */
    Type1 *type1;       /* Simple font: type1_glyphs maps character codes to glyphs. */
    int *type1_glyphs;
    Value *source;
    bool identity;
    double *widths;
    struct PdfFont *next;
} PdfFont;
struct Pdf {
    unsigned char *file;
    size_t size;
    Object *objects;
    int object_count;
    Value *catalog;
    Page *pages;
    int page_count;
    Allocation *allocs;
    size_t memory;
    jmp_buf jump;
    char error[256];
    Path work;
    PdfFont *fonts;
    bool fonts_frozen;
    Path path_stack[32];
    int path_depth;
    size_t animation_memory;
    ImageScaler scaler;   /* Working buffers kept between images. */
    Box placeholders[64]; /* Candidate animation boxes of the page being rendered. */
    int placeholder_count;
    size_t image_cache_bytes;
    bool image_cache_closed;
};
typedef struct {
    Pdf *d;
    const unsigned char *s;
    size_t n, p;
} Parser;
static void fail(Pdf *d, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(d->error, sizeof(d->error), fmt, args);
    va_end(args);
    longjmp(d->jump, 1);
}
static void *alloc(Pdf *d, size_t n) {
    if (n > MEMORY_LIMIT || d->memory > MEMORY_LIMIT - n)
        fail(d, "PDF memory limit exceeded");
    Allocation *a = calloc(1, sizeof(*a) + n);
    if (!a)
        fail(d, "Out of memory");
    a->next = d->allocs;
    a->size = n;
    d->allocs = a;
    d->memory += n;
    return a + 1;
}
static void rollback(Pdf *d, Allocation *mark) {
    while (d->allocs != mark) {
        Allocation *a = d->allocs;
        d->allocs = a->next;
        d->memory -= a->size;
        free(a);
    }
}
static bool white(unsigned c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
static bool delim(unsigned c) { return white(c) || strchr("()<>[]{}/%", (int)c) != NULL; }
static void skip(Parser *p) {
    while (p->p < p->n) {
        unsigned c = p->s[p->p];
        if (white(c)) {
            p->p++;
            continue;
        }
        if (c == '%') {
            while (p->p < p->n && p->s[p->p] != '\r' && p->s[p->p] != '\n')
                p->p++;
            continue;
        }
        break;
    }
}
static bool keyword(Parser *p, const char *s) {
    skip(p);
    size_t n = strlen(s);
    if (n > p->n - p->p || memcmp(p->s + p->p, s, n) || (p->p + n < p->n && !delim(p->s[p->p + n])))
        return false;
    p->p += n;
    return true;
}
static int hex(unsigned c) {
    if (c >= '0' && c <= '9')
        return (int)c - '0';
    if (c >= 'a' && c <= 'f')
        return (int)c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return (int)c - 'A' + 10;
    return -1;
}
static Value *value(Parser *p, int depth);
static Value *resolve(Pdf *d, Value *v);
static void decode_stream(Pdf *d, Value *v);
static Value *raw_get(Value *v, const char *key) {
    if (!v || v->kind != DICT)
        return NULL;
    for (Entry *e = v->entries; e; e = e->next)
        if (e->key->len == strlen(key) && !memcmp(e->key->text, key, e->key->len))
            return e->value;
    return NULL;
}
static Value *get(Pdf *d, Value *v, const char *key) {
    return resolve(d, raw_get(resolve(d, v), key));
}
static bool named(Value *v, const char *s) {
    return v && v->kind == NAME && v->len == strlen(s) && !memcmp(v->text, s, v->len);
}
static double num(Pdf *d, Value *v) {
    v = resolve(d, v);
    if (!v || v->kind != NUMBER || !isfinite(v->number) || fabs(v->number) > 1e8)
        fail(d, "Expected a finite PDF number");
    return v->number;
}
static int integer(Pdf *d, Value *v, int lo, int hi) {
    double n = num(d, v);
    if (n < lo || n > hi || n != floor(n))
        fail(d, "Invalid PDF integer");
    return (int)n;
}
static Value *item(Pdf *d, Value *v, size_t i) {
    v = resolve(d, v);
    if (!v || v->kind != ARRAY || i >= v->count)
        fail(d, "Invalid PDF array");
    return resolve(d, v->items[i]);
}
static Value *new_value(Parser *p, Kind k) {
    Value *v = alloc(p->d, sizeof(*v));
    v->kind = k;
    return v;
}
/* Parse PDF syntax directly; binary stream boundaries come from /Length, never sentinel scans. */
static Value *value(Parser *p, int depth) {
    if (depth > DEPTH_LIMIT)
        fail(p->d, "PDF nesting limit exceeded");
    skip(p);
    if (p->p >= p->n)
        fail(p->d, "Unexpected end of PDF");
    unsigned c = p->s[p->p++];
    Value *v;
    if (c == '[') {
        v = new_value(p, ARRAY);
        Entry *head = NULL;
        for (;;) {
            skip(p);
            if (p->p >= p->n)
                fail(p->d, "Unterminated array");
            if (p->s[p->p] == ']') {
                p->p++;
                break;
            }
            if (v->count >= ARRAY_LIMIT)
                fail(p->d, "Array limit exceeded");
            Entry *e = alloc(p->d, sizeof(*e));
            e->value = value(p, depth + 1);
            e->next = head;
            head = e;
            v->count++;
        }
        v->items = alloc(p->d, v->count * sizeof(*v->items));
        size_t at = v->count;
        for (Entry *e = head; e; e = e->next)
            v->items[--at] = e->value;
        return v;
    }
    if (c == '<' && p->p < p->n && p->s[p->p] == '<') {
        p->p++;
        v = new_value(p, DICT);
        size_t count = 0;
        for (;;) {
            skip(p);
            if (p->p + 1 < p->n && p->s[p->p] == '>' && p->s[p->p + 1] == '>') {
                p->p += 2;
                return v;
            }
            if (++count > ARRAY_LIMIT)
                fail(p->d, "Dictionary limit exceeded");
            Value *key = value(p, depth + 1);
            if (key->kind != NAME)
                fail(p->d, "Invalid dictionary key");
            if (raw_get(v, key->text))
                fail(p->d, "Duplicate dictionary key");
            Entry *e = alloc(p->d, sizeof(*e));
            e->key = key;
            e->value = value(p, depth + 1);
            e->next = v->entries;
            v->entries = e;
        }
    }
    if (c == '/' || c == '(' || c == '<') {
        Kind k = c == '/' ? NAME : STRING;
        v = new_value(p, k);
        /* Bound storage by the token, rather than the remainder of the file. */
        size_t start = p->p, end = start;
        if (c == '/')
            while (end < p->n && !delim(p->s[end]))
                end++;
        else if (c == '<') {
            while (end < p->n && p->s[end] != '>')
                end++;
            if (end == p->n)
                fail(p->d, "Unterminated hex string");
        } else {
            int nesting = 1;
            while (end < p->n && nesting) {
                unsigned ch = p->s[end++];
                if (ch == '\\') {
                    if (end < p->n)
                        end++;
                } else if (ch == '(')
                    nesting++;
                else if (ch == ')')
                    nesting--;
            }
            if (nesting)
                fail(p->d, "Unterminated literal string");
        }
        v->text = alloc(p->d, end - start + 2);
        if (c == '/') {
            while (p->p < end) {
                unsigned ch = p->s[p->p++];
                if (ch == '#') {
                    if (p->p + 1 >= end || hex(p->s[p->p]) < 0 || hex(p->s[p->p + 1]) < 0)
                        fail(p->d, "Invalid name escape");
                    ch = (unsigned)(hex(p->s[p->p]) * 16 + hex(p->s[p->p + 1]));
                    p->p += 2;
                }
                if (!ch)
                    fail(p->d, "NUL in PDF name unsupported");
                v->text[v->len++] = (char)ch;
            }
        } else if (c == '<') {
            int high = -1;
            while (p->p < end) {
                unsigned ch = p->s[p->p++];
                if (white(ch))
                    continue;
                int h = hex(ch);
                if (h < 0)
                    fail(p->d, "Invalid hex string");
                if (high < 0)
                    high = h;
                else {
                    v->text[v->len++] = (char)(high * 16 + h);
                    high = -1;
                }
            }
            if (high >= 0)
                v->text[v->len++] = (char)(high * 16);
            p->p++;
        } else {
            int nesting = 1;
            while (nesting) {
                unsigned ch = p->s[p->p++];
                if (ch == '\\') {
                    ch = p->s[p->p++];
                    if (ch == '\r' || ch == '\n') {
                        if (ch == '\r' && p->p < p->n && p->s[p->p] == '\n')
                            p->p++;
                        continue;
                    }
                    if (ch >= '0' && ch <= '7') {
                        unsigned x = ch - '0';
                        for (int i = 0;
                             i < 2 && p->p < p->n && p->s[p->p] >= '0' && p->s[p->p] <= '7'; i++)
                            x = x * 8 + p->s[p->p++] - '0';
                        ch = x & 255;
                    } else if (ch == 'n')
                        ch = '\n';
                    else if (ch == 'r')
                        ch = '\r';
                    else if (ch == 't')
                        ch = '\t';
                    else if (ch == 'b')
                        ch = '\b';
                    else if (ch == 'f')
                        ch = '\f';
                } else if (ch == '(')
                    nesting++;
                else if (ch == ')') {
                    if (!--nesting)
                        break;
                } else if (ch == '\r') {
                    ch = '\n';
                    if (p->p < p->n && p->s[p->p] == '\n')
                        p->p++;
                }
                v->text[v->len++] = (char)ch;
            }
        }
        return v;
    }
    size_t start = p->p - 1;
    while (p->p < p->n && !delim(p->s[p->p]))
        p->p++;
    size_t n = p->p - start;
    if (!n || n > 255)
        fail(p->d, "Invalid PDF token");
    char text[256];
    memcpy(text, p->s + start, n);
    text[n] = 0;
    if (c == '+' || c == '-' || c == '.' || isdigit(c)) {
        for (size_t i = 0; i < n; i++)
            if (!isdigit((unsigned char)text[i]) && text[i] != '+' && text[i] != '-' &&
                text[i] != '.')
                fail(p->d, "Invalid PDF number");
        char *tail;
        double a = strtod(text, &tail);
        if (*tail || !isfinite(a))
            fail(p->d, "Invalid PDF number");
        v = new_value(p, NUMBER);
        v->number = a;
        if (a >= 0 && a < OBJECT_LIMIT && a == floor(a)) {
            size_t save = p->p;
            skip(p);
            size_t bstart = p->p;
            while (p->p < p->n && isdigit(p->s[p->p]))
                p->p++;
            size_t bn = p->p - bstart;
            if (bn && bn < 8) {
                char b[8];
                memcpy(b, p->s + bstart, bn);
                b[bn] = 0;
                if (keyword(p, "R")) {
                    v->kind = REF;
                    v->ref = (int)a;
                    v->gen = atoi(b);
                    return v;
                }
            }
            p->p = save;
        }
        return v;
    }
    v = new_value(p, !strcmp(text, "null") ? NIL : WORD);
    v->text = alloc(p->d, n + 1);
    memcpy(v->text, text, n + 1);
    v->len = n;
    return v;
}
static Value *resolve(Pdf *d, Value *v) {
    if (!v || v->kind != REF)
        return v;
    if (v->ref < 1 || v->ref >= d->object_count)
        fail(d, "Invalid indirect reference");
    Object *o = &d->objects[v->ref];
    if (!o->used || o->gen != v->gen)
        fail(d, "Missing PDF object %d", v->ref);
    if (o->state == 2)
        return o->value;
    if (o->state == 1)
        fail(d, "Cyclic object definition");
    o->state = 1;
    if (o->stream_id) {
        Value ref = {0};
        ref.kind = REF;
        ref.ref = o->stream_id;
        Value *stream = resolve(d, &ref);
        decode_stream(d, stream);
        if (!named(get(d, stream, "Type"), "ObjStm"))
            fail(d, "Expected object stream");
        int count = integer(d, get(d, stream, "N"), 1, OBJECT_LIMIT);
        size_t first = (size_t)integer(d, get(d, stream, "First"), 0, (int)stream->decoded_len);
        if (o->stream_index >= count)
            fail(d, "Invalid object stream index");
        Parser header = {d, stream->decoded, first, 0};
        size_t offset = 0;
        int id = 0;
        for (int i = 0; i <= o->stream_index; i++) {
            id = integer(d, value(&header, 0), 1, OBJECT_LIMIT - 1);
            offset = (size_t)integer(d, value(&header, 0), 0, (int)stream->decoded_len);
        }
        if (id != v->ref || offset >= stream->decoded_len - first)
            fail(d, "Object stream header mismatch");
        Parser body = {d, stream->decoded, stream->decoded_len, first + offset};
        o->value = value(&body, 0);
        o->state = 2;
        return o->value;
    }
    Parser p = {d, d->file, d->size, o->offset};
    Value *id = value(&p, 0), *generation = value(&p, 0);
    if (integer(d, id, 0, OBJECT_LIMIT) != v->ref || integer(d, generation, 0, 65535) != o->gen ||
        !keyword(&p, "obj"))
        fail(d, "Invalid object header");
    o->value = value(&p, 0);
    if (o->value->kind == REF)
        fail(d, "Indirect reference aliases unsupported");
    if (o->value->kind == DICT && keyword(&p, "stream")) {
        if (p.p < p.n && p.s[p.p] == '\r') {
            p.p++;
            if (p.p < p.n && p.s[p.p] == '\n')
                p.p++;
        } else if (p.p < p.n && p.s[p.p] == '\n')
            p.p++;
        else
            fail(d, "Missing stream newline");
        size_t length = (size_t)integer(d, get(d, o->value, "Length"), 0, FILE_LIMIT);
        if (length > p.n - p.p)
            fail(d, "Stream outside file");
        o->value->stream = p.s + p.p;
        o->value->stream_len = length;
        p.p += length;
        if (!keyword(&p, "endstream"))
            fail(d, "Incorrect stream length");
    }
    if (!keyword(&p, "endobj"))
        fail(d, "Missing endobj");
    o->state = 2;
    return o->value;
}
/* PDF rectangles may name any two opposite corners; normalize them. */
static Box rect_value(Pdf *d, Value *v) {
    if (!v || v->kind != ARRAY || v->count != 4)
        fail(d, "Invalid PDF rectangle");
    double x0 = num(d, item(d, v, 0)), y0 = num(d, item(d, v, 1)), x1 = num(d, item(d, v, 2)),
           y1 = num(d, item(d, v, 3));
    return (Box){MIN(x0, x1), MIN(y0, y1), MAX(x0, x1), MAX(y0, y1)};
}
static Box box_value(Pdf *d, Value *v) {
    Box b = rect_value(d, v);
    if (b.x1 <= b.x0 || b.y1 <= b.y0)
        fail(d, "Empty page box");
    return b;
}
static void pages(Pdf *d, Value *node, Value *resources, Value *media, Value *crop, int rotation,
                  int depth) {
    if (depth > DEPTH_LIMIT)
        fail(d, "Page tree depth/cycle limit exceeded");
    node = resolve(d, node);
    Value *v;
    if ((v = get(d, node, "Resources")))
        resources = v;
    if ((v = get(d, node, "MediaBox")))
        media = v;
    if ((v = get(d, node, "CropBox")))
        crop = v;
    if ((v = get(d, node, "Rotate")))
        rotation = integer(d, v, -360000, 360000);
    if (named(get(d, node, "Type"), "Pages")) {
        Value *kids = get(d, node, "Kids");
        if (!kids || kids->kind != ARRAY)
            fail(d, "Invalid page tree");
        for (size_t i = 0; i < kids->count; i++)
            pages(d, item(d, kids, i), resources, media, crop, rotation, depth + 1);
        return;
    }
    if (!named(get(d, node, "Type"), "Page"))
        fail(d, "Expected Page dictionary");
    if (d->page_count >= 10000)
        fail(d, "Page count limit exceeded");
    if (rotation % 90)
        fail(d, "Page rotation must be a multiple of 90");
    Box b = box_value(d, media);
    if (crop) {
        Box c = box_value(d, crop);
        b = (Box){MAX(b.x0, c.x0), MAX(b.y0, c.y0), MIN(b.x1, c.x1), MIN(b.y1, c.y1)};
    }
    if (b.x1 <= b.x0 || b.y1 <= b.y0)
        fail(d, "Empty crop box");
    if (get(d, node, "OC"))
        fail(d, "Optional page content unsupported");
    if (get(d, node, "Group"))
        fail(d, "Page transparency groups unsupported");
    if ((v = get(d, node, "UserUnit")) && num(d, v) != 1)
        fail(d, "UserUnit other than 1 unsupported");
    Value *annots = get(d, node, "Annots");
    if (annots) {
        if (annots->kind != ARRAY)
            fail(d, "Invalid annotations");
        for (size_t i = 0; i < annots->count; i++) {
            Value *type = get(d, item(d, annots, i), "Subtype");
            if (!named(type, "Link") && !named(type, "FileAttachment"))
                fail(d, "Only Link and FileAttachment annotations supported");
        }
    }
    d->pages[d->page_count++] = (Page){node, resources, b, ((rotation % 360) + 360) % 360};
}
static void decode_stream(Pdf *d, Value *v) {
    if (!v || !v->stream || v->decoded)
        return;
    Value *f = get(d, v, "Filter");
    if (f && f->kind == ARRAY) {
        if (f->count != 1)
            fail(d, "Multiple stream filters unsupported");
        f = item(d, f, 0);
    }
    if (!f || named(f, "DCTDecode")) {
        v->decoded = (unsigned char *)v->stream;
        v->decoded_len = v->stream_len;
        return;
    }
    if (!named(f, "FlateDecode"))
        fail(d, "Unsupported stream filter (only FlateDecode and DCTDecode)");
    z_stream zs = {0};
    zs.next_in = (Bytef *)v->stream;
    zs.avail_in = (uInt)v->stream_len;
    if (inflateInit(&zs) != Z_OK)
        fail(d, "Cannot initialize zlib");
    size_t capacity = 4096;
    unsigned char *tmp = malloc(capacity);
    if (!tmp) {
        inflateEnd(&zs);
        fail(d, "Out of memory");
    }
    int ret = Z_OK;
    while (ret == Z_OK) {
        if (zs.total_out == capacity) {
            if (capacity >= FILE_LIMIT) {
                free(tmp);
                inflateEnd(&zs);
                fail(d, "Decompressed stream too large");
            }
            capacity *= 2;
            unsigned char *next = realloc(tmp, capacity);
            if (!next) {
                free(tmp);
                inflateEnd(&zs);
                fail(d, "Out of memory");
            }
            tmp = next;
        }
        zs.next_out = tmp + zs.total_out;
        zs.avail_out = (uInt)(capacity - zs.total_out);
        ret = inflate(&zs, Z_NO_FLUSH);
    }
    size_t length = zs.total_out;
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        free(tmp);
        fail(d, "Invalid Flate stream");
    }
    /* Check allocation budget before alloc(), so tmp cannot leak on longjmp. */
    if (length > MEMORY_LIMIT - d->memory) {
        free(tmp);
        fail(d, "PDF memory limit exceeded");
    }
    v->decoded = alloc(d, length);
    memcpy(v->decoded, tmp, length);
    v->decoded_len = length;
    free(tmp);
}
/* Attachment text can be PDFDocEncoding or UTF-16BE (as emitted by XeLaTeX).
 * Protocol metadata is ASCII; filenames are informational, never opened. */
static const char *attachment_text(Pdf *d, Value *v) {
    if (!v || v->kind != STRING)
        return "";
    char *out = alloc(d, v->len + 1);
    size_t n = 0;
    if (v->len >= 2 && (unsigned char)v->text[0] == 254 && (unsigned char)v->text[1] == 255) {
        for (size_t i = 2; i + 1 < v->len; i += 2)
            out[n++] =
                v->text[i] == 0 && (unsigned char)v->text[i + 1] < 128 ? v->text[i + 1] : '?';
    } else {
        memcpy(out, v->text, v->len);
        n = v->len;
    }
    out[n] = 0;
    return out;
}
static bool metadata(Pdf *d, const char *text, PdfAnimation *info) {
    if (strncmp(text, "SDLAnim ", 8))
        return false;
    text += 8;
    while (*text) {
        while (*text == ' ' || *text == ';')
            text++;
        if (!*text)
            break;
        const char *end = text;
        while (*end && *end != ' ' && *end != ';')
            end++;
        size_t size = (size_t)(end - text);
        char token[96];
        if (size >= sizeof(token))
            fail(d, "Animation metadata token too long");
        memcpy(token, text, size);
        token[size] = 0;
        if (!strncmp(token, "fps=", 4)) {
            char *tail;
            double fps = strtod(token + 4, &tail);
            if (tail == token + 4 || *tail || !isfinite(fps) || fps < .1 || fps > 240)
                fail(d, "Animation FPS must be between 0.1 and 240");
            info->fps = fps;
        } else if (!strcmp(token, "autoplay=true") || !strcmp(token, "autoplay=1"))
            info->autoplay = true;
        else if (!strcmp(token, "autoplay=false") || !strcmp(token, "autoplay=0"))
            info->autoplay = false;
        else if (!strcmp(token, "interpolation=nearest"))
            info->interpolation = ANIMATION_NEAREST;
        else if (!strcmp(token, "interpolation=bicubic"))
            info->interpolation = ANIMATION_BICUBIC;
        else
            fail(d, "Unknown SDLAnim metadata option: %s", token);
        text = end;
    }
    return true;
}
static void load_animations(Pdf *d, Page *page) {
    Value *annots = get(d, page->object, "Annots");
    if (!annots)
        return;
    size_t count = 0;
    for (size_t i = 0; i < annots->count; i++)
        if (named(get(d, item(d, annots, i), "Subtype"), "FileAttachment"))
            count++;
    if (!count)
        return;
    if (count > 64)
        fail(d, "Too many attachments on one page");
    page->animations = alloc(d, count * sizeof(*page->animations));
    for (size_t i = 0; i < annots->count; i++) {
        Value *annot = item(d, annots, i);
        if (!named(get(d, annot, "Subtype"), "FileAttachment"))
            continue;
        Value *fs = get(d, annot, "FS"), *ef = get(d, fs, "EF");
        Value *stream = get(d, ef, "UF");
        if (!stream)
            stream = get(d, ef, "F");
        if (!stream || !stream->decoded || stream->decoded_len < 8)
            continue;
        const unsigned char *data = stream->decoded;
        bool gif = !memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6);
        bool png = !memcmp(data, "\211PNG\r\n\032\n", 8);
        PdfAnimation info = {.fps = 24, .interpolation = ANIMATION_NEAREST};
        bool explicit_box = metadata(d, attachment_text(d, get(d, annot, "Subj")), &info);
        if (!gif && !png) {
            if (explicit_box)
                fail(d, "SDLAnim attachment is not GIF or APNG");
            continue;
        }
        /* A normal PNG attachment is not an animation. Inspect bounded chunks. */
        if (png && !explicit_box) {
            bool animated = false;
            for (size_t at = 8; stream->decoded_len - at >= 12;) {
                uint32_t length = (uint32_t)data[at] << 24 | (uint32_t)data[at + 1] << 16 |
                                  (uint32_t)data[at + 2] << 8 | data[at + 3];
                if (length > stream->decoded_len - at - 12)
                    break;
                if (!memcmp(data + at + 4, "acTL", 4)) {
                    animated = true;
                    break;
                }
                at += (size_t)length + 12;
            }
            if (!animated)
                continue;
        }
        Box anchor = rect_value(d, get(d, annot, "Rect"));
        if (explicit_box && (anchor.x1 <= anchor.x0 || anchor.y1 <= anchor.y0))
            fail(d, "SDLAnim requires a nonempty annotation rectangle");
        Value *name = get(d, fs, "UF");
        if (!name)
            name = get(d, fs, "F");
        info.filename = attachment_text(d, name);
        info.box = anchor;
        info.positioned = explicit_box || (anchor.x1 > anchor.x0 && anchor.y1 > anchor.y0);
        const size_t budget = 128u * 1024u * 1024u;
        char error[256];
        Animation *clip = animation_decode(data, stream->decoded_len, budget - d->animation_memory,
                                           error, sizeof(error));
        if (!clip)
            fail(d, "Animation %s: %s", info.filename, error);
        info.clip = clip;
        d->animation_memory += clip->bytes;
        page->animations[page->animation_count++] =
            (AttachedAnimation){info, anchor, !info.positioned};
    }
}
/* Fills the code-to-glyph map: the font's built-in encoding or StandardEncoding,
 * then any PDF Differences by glyph name. */
static void type1_load_encoding(Pdf *d, PdfFont *font, Value *source) {
    Value *encoding = get(d, source, "Encoding"), *base = encoding, *differences = NULL;
    if (encoding && encoding->kind == DICT) {
        base = get(d, encoding, "BaseEncoding");
        differences = get(d, encoding, "Differences");
    }
    if (base && !named(base, "StandardEncoding"))
        fail(d, "Only Differences and StandardEncoding supported for Type 1 fonts");
    font->type1_glyphs = alloc(d, 256 * sizeof(int));
    for (unsigned code = 0; code < 256; code++)
        font->type1_glyphs[code] = base ? type1_standard_glyph(font->type1, code)
                                        : type1_builtin_glyph(font->type1, code);
    if (!differences)
        return;
    if (differences->kind != ARRAY)
        fail(d, "Invalid font encoding Differences");
    int code = 0;
    for (size_t i = 0; i < differences->count; i++) {
        Value *v = item(d, differences, i);
        if (v->kind == NUMBER)
            code = integer(d, v, 0, 255);
        else if (v->kind == NAME && code < 256)
            font->type1_glyphs[code++] = type1_glyph(font->type1, v->text, v->len);
        else
            fail(d, "Invalid font encoding Differences");
    }
}
static PdfFont *font_load(Pdf *d, Value *source) {
    for (PdfFont *f = d->fonts; f; f = f->next)
        if (f->source == source)
            return f;
    if (d->fonts_frozen)
        fail(d, "Font resources were not prepared at document load");
    bool identity = named(get(d, source, "Subtype"), "Type0");
    bool truetype = named(get(d, source, "Subtype"), "TrueType");
    Value *metrics = source;
    if (identity) {
        if (!named(get(d, source, "Encoding"), "Identity-H"))
            fail(d, "Only Identity-H composite fonts supported");
        metrics = item(d, get(d, source, "DescendantFonts"), 0);
    } else if (!truetype && !named(get(d, source, "Subtype"), "Type1"))
        fail(d, "Expected an embedded CFF or TrueType font");
    Value *descriptor = get(d, metrics, "FontDescriptor");
    Value *file = get(d, descriptor, truetype ? "FontFile2" : "FontFile3");
    bool type1 = !identity && !truetype && !file && get(d, descriptor, "FontFile");
    if (type1)
        file = get(d, descriptor, "FontFile");
    if (!file || !file->decoded)
        fail(d, truetype ? "Font requires embedded TrueType outlines"
                         : "Font requires embedded CFF or Type 1 outlines");
    if (truetype) {
        if (!named(get(d, source, "Encoding"), "WinAnsiEncoding"))
            fail(d, "Only WinAnsiEncoding simple TrueType fonts supported");
    } else if (!type1 && !named(get(d, file, "Subtype"), "Type1C") &&
               !named(get(d, file, "Subtype"), "CIDFontType0C"))
        fail(d, "Unsupported embedded font format");
    PdfFont *font = alloc(d, sizeof(*font));
    font->source = source;
    font->identity = identity;
    font->next = d->fonts;
    d->fonts = font;
    if (truetype) {
        font->truetype = text_font_from_memory(file->decoded, file->decoded_len);
        if (!font->truetype)
            fail(d, "Embedded TrueType font could not load");
    } else if (type1) {
        char error[160];
        font->type1 = type1_open(file->decoded, file->decoded_len, error, sizeof(error));
        if (!font->type1)
            fail(d, "Embedded Type 1 font could not load: %s", error);
        type1_load_encoding(d, font, source);
    } else {
        font->cff = cff_open(file->decoded, file->decoded_len, d->error, sizeof(d->error));
        if (!font->cff)
            fail(d, "Embedded CFF font could not load");
    }
    font->widths = alloc(d, 65536 * sizeof(double));
    double default_width = 1000;
    Value *v;
    if (identity && (v = get(d, metrics, "DW")))
        default_width = num(d, v);
    if (!identity)
        default_width = 0;
    for (int i = 0; i < 65536; i++)
        font->widths[i] = default_width;
    if (identity) {
        Value *widths = get(d, metrics, "W");
        if (widths) {
            if (widths->kind != ARRAY)
                fail(d, "Invalid CID widths");
            size_t i = 0;
            while (i < widths->count) {
                int first = integer(d, item(d, widths, i++), 0, 65535);
                Value *next = item(d, widths, i++);
                if (next->kind == ARRAY) {
                    if (next->count > 65536u - first)
                        fail(d, "CID widths overflow");
                    for (size_t j = 0; j < next->count; j++)
                        font->widths[first + j] = num(d, item(d, next, j));
                } else {
                    int last = integer(d, next, first, 65535);
                    double w = num(d, item(d, widths, i++));
                    for (int j = first; j <= last; j++)
                        font->widths[j] = w;
                }
            }
        }
    } else {
        int first = integer(d, get(d, metrics, "FirstChar"), 0, 255);
        Value *widths = get(d, metrics, "Widths");
        if (!widths || widths->kind != ARRAY || widths->count > 256u - first)
            fail(d, "Invalid simple font widths");
        for (size_t i = 0; i < widths->count; i++)
            font->widths[first + i] = num(d, item(d, widths, i));
        if (!truetype && !type1 && get(d, source, "Encoding"))
            fail(d, "PDF encoding overrides for simple CFF fonts unsupported");
    }
    return font;
}
Pdf *pdf_open(const char *filename, char *error, size_t error_size) {
    Pdf *d = calloc(1, sizeof(*d));
    if (!d) {
        snprintf(error, error_size, "Out of memory");
        return NULL;
    }
    if (setjmp(d->jump)) {
        snprintf(error, error_size, "%s", d->error);
        pdf_close(d);
        return NULL;
    }
    FILE *fp = fopen(filename, "rb");
    if (!fp)
        fail(d, "Cannot open PDF file");
    if (fseek(fp, 0, SEEK_END)) {
        fclose(fp);
        fail(d, "Cannot seek PDF");
    }
    long size = ftell(fp);
    if (size < 16 || size > FILE_LIMIT) {
        fclose(fp);
        fail(d, "PDF must be between 16 bytes and 64 MiB");
    }
    rewind(fp);
    d->size = (size_t)size;
    d->file = malloc(d->size + 1);
    if (!d->file) {
        fclose(fp);
        fail(d, "Out of memory");
    }
    size_t got = fread(d->file, 1, d->size, fp);
    fclose(fp);
    d->file[d->size] = 0;
    if (got != d->size || memcmp(d->file, "%PDF-", 5))
        fail(d, "Invalid PDF header");
    size_t sx = 0;
    bool found = false;
    for (size_t i = d->size - 9; i > 0; i--)
        if (!memcmp(d->file + i, "startxref", 9)) {
            sx = i + 9;
            found = true;
            break;
        }
    if (!found)
        fail(d, "Missing startxref");
    Parser p = {d, d->file, d->size, sx};
    size_t offset = (size_t)integer(d, value(&p, 0), 0, (int)d->size - 1);
    p.p = offset;
    d->object_count = OBJECT_LIMIT;
    d->objects = calloc(OBJECT_LIMIT, sizeof(*d->objects));
    if (!d->objects)
        fail(d, "Out of memory");
    Value *trailer = NULL;
    if (keyword(&p, "xref")) {
        for (;;) {
            if (keyword(&p, "trailer"))
                break;
            int start = integer(d, value(&p, 0), 0, OBJECT_LIMIT - 1),
                count = integer(d, value(&p, 0), 0, OBJECT_LIMIT - start);
            for (int i = 0; i < count; i++) {
                size_t pos = (size_t)integer(d, value(&p, 0), 0, (int)d->size);
                int gen = integer(d, value(&p, 0), 0, 65535);
                bool used = keyword(&p, "n");
                if (!used && !keyword(&p, "f"))
                    fail(d, "Invalid xref entry");
                Object *o = &d->objects[start + i];
                if (o->used)
                    fail(d, "Overlapping xref sections");
                *o = (Object){.offset = pos, .gen = gen, .used = used};
            }
        }
        trailer = value(&p, 0);
    } else {
        p.p = offset;
        int id = integer(d, value(&p, 0), 1, OBJECT_LIMIT - 1),
            gen = integer(d, value(&p, 0), 0, 65535);
        d->objects[id] = (Object){.offset = offset, .gen = gen, .used = true};
        Value ref = {0};
        ref.kind = REF;
        ref.ref = id;
        ref.gen = gen;
        trailer = resolve(d, &ref);
        if (!named(get(d, trailer, "Type"), "XRef"))
            fail(d, "Expected xref stream");
        decode_stream(d, trailer);
        if (get(d, trailer, "DecodeParms"))
            fail(d, "Predictors in xref streams unsupported");
        Value *widths = get(d, trailer, "W"), *indices = get(d, trailer, "Index");
        int widths3[3];
        size_t stride = 0;
        for (int k = 0; k < 3; k++) {
            widths3[k] = integer(d, item(d, widths, k), 0, 4);
            stride += widths3[k];
        }
        if (!stride)
            fail(d, "Empty xref fields");
        int total = integer(d, get(d, trailer, "Size"), 1, OBJECT_LIMIT);
        size_t at = 0;
        size_t ranges = indices ? indices->count : 2;
        if (indices && (indices->kind != ARRAY || ranges % 2))
            fail(d, "Invalid xref Index");
        for (size_t range = 0; range < ranges; range += 2) {
            int first = indices ? integer(d, item(d, indices, range), 0, OBJECT_LIMIT - 1) : 0;
            int count =
                indices ? integer(d, item(d, indices, range + 1), 0, OBJECT_LIMIT - first) : total;
            for (int i = 0; i < count; i++) {
                if (stride > trailer->decoded_len - at)
                    fail(d, "Truncated xref stream");
                uint32_t fields[3] = {widths3[0] ? 0 : 1, 0, 0};
                for (int k = 0; k < 3; k++)
                    for (int j = 0; j < widths3[k]; j++)
                        fields[k] = (fields[k] << 8) | trailer->decoded[at++];
                if (first + i == id)
                    continue;
                Object *o = &d->objects[first + i];
                if (fields[0] == 0)
                    continue;
                if (fields[0] == 1) {
                    if (fields[1] >= d->size || fields[2] > 65535)
                        fail(d, "Invalid xref stream offset");
                    *o = (Object){.offset = fields[1], .gen = (int)fields[2], .used = true};
                } else if (fields[0] == 2) {
                    if (fields[1] < 1 || fields[1] >= OBJECT_LIMIT || fields[2] >= OBJECT_LIMIT)
                        fail(d, "Invalid compressed object reference");
                    *o = (Object){
                        .used = true, .stream_id = (int)fields[1], .stream_index = (int)fields[2]};
                } else
                    fail(d, "Unsupported xref entry type");
            }
        }
    }
    if (get(d, trailer, "Encrypt"))
        fail(d, "Encrypted PDFs unsupported");
    if (get(d, trailer, "Prev") || get(d, trailer, "XRefStm"))
        fail(d, "Incremental/hybrid PDF files unsupported; rewrite as a classic PDF");
    d->catalog = get(d, trailer, "Root");
    if (!named(get(d, d->catalog, "Type"), "Catalog"))
        fail(d, "Invalid document catalog");
    /* Resolve all objects now; rendering can then use temporary allocations safely. */
    for (int i = 1; i < d->object_count; i++)
        if (d->objects[i].used) {
            Value ref = {0};
            ref.kind = REF;
            ref.ref = i;
            ref.gen = d->objects[i].gen;
            resolve(d, &ref);
        }
    for (int i = 1; i < d->object_count; i++)
        if (d->objects[i].used)
            decode_stream(d, d->objects[i].value);
    for (int i = 1; i < d->object_count; i++)
        if (d->objects[i].used) {
            Value *v = d->objects[i].value;
            if (named(get(d, v, "Type"), "Font") &&
                (named(get(d, v, "Subtype"), "Type0") || named(get(d, v, "Subtype"), "Type1") ||
                 named(get(d, v, "Subtype"), "TrueType")))
                font_load(d, v);
        }
    /* Also prepare direct font dictionaries in page/form resources. Cached font
       metadata must outlive the temporary per-render arena. */
    for (int i = 1; i < d->object_count; i++)
        if (d->objects[i].used) {
            Value *object = d->objects[i].value;
            Value *resources[2] = {object, get(d, object, "Resources")};
            for (int k = 0; k < 2; k++) {
                Value *fonts = get(d, resources[k], "Font");
                if (fonts && fonts->kind == DICT)
                    for (Entry *e = fonts->entries; e; e = e->next)
                        font_load(d, resolve(d, e->value));
            }
        }
    d->fonts_frozen = true;
    d->pages = alloc(d, 10000 * sizeof(*d->pages));
    pages(d, get(d, d->catalog, "Pages"), NULL, NULL, NULL, 0, 0);
    if (!d->page_count)
        fail(d, "No PDF pages");
    for (int i = 0; i < d->page_count; i++) load_animations(d, &d->pages[i]);
    return d;
}
void pdf_close(Pdf *d) {
    if (!d)
        return;
    path_free(&d->work);
    image_scaler_free(&d->scaler);
    for (PdfFont *f = d->fonts; f; f = f->next) {
        cff_close(f->cff);
        text_font_free(f->truetype);
        type1_close(f->type1);
    }
    for (int i = 0; i < d->page_count; i++)
        for (int j = 0; j < d->pages[i].animation_count; j++)
            animation_free((Animation *)d->pages[i].animations[j].info.clip);
    for (int i = 1; d->objects && i < d->object_count; i++)
        if (d->objects[i].value)
            free(d->objects[i].value->rgba);
    rollback(d, NULL);
    free(d->objects);
    free(d->file);
    free(d);
}
Pdf *pdf_worker_create(Pdf *d) {
    Pdf *worker = malloc(sizeof(*worker));
    if (!worker)
        return NULL;
    /* Nothing below this point writes to state the renderers share. */
    d->image_cache_closed = true;
    for (PdfFont *f = d->fonts; f; f = f->next)
        if (f->cff)
            cff_freeze(f->cff);
    *worker = *d;
    worker->allocs = NULL;
    worker->memory = 0;
    worker->work = (Path){0};
    worker->scaler = (ImageScaler){0};
    memset(worker->path_stack, 0, sizeof(worker->path_stack));
    worker->path_depth = worker->placeholder_count = 0;
    worker->error[0] = 0;
    return worker;
}
void pdf_worker_free(Pdf *worker) {
    if (!worker)
        return;
    path_free(&worker->work);
    image_scaler_free(&worker->scaler);
    rollback(worker, NULL);
    free(worker);
}
int pdf_page_count(const Pdf *d) { return d->page_count; }
const char *pdf_error(const Pdf *d) { return d->error; }
int pdf_animation_count(const Pdf *d, int page) {
    return page >= 0 && page < d->page_count ? d->pages[page].animation_count : 0;
}
const PdfAnimation *pdf_animation(const Pdf *d, int page, int index) {
    return index >= 0 && index < pdf_animation_count(d, page) ? &d->pages[page].animations[index].info : NULL;
}
/* Image codecs are independent of PDF interpretation. No platform API is used. */
typedef struct {
    struct jpeg_decompress_struct info;
    struct jpeg_error_mgr error;
    jmp_buf jump;
    bool created;
} Jpeg;
static void jpeg_failure(j_common_ptr c) {
    Jpeg *j = (Jpeg *)c->client_data;
    longjmp(j->jump, 1);
}
static unsigned char *predict(Pdf *d, Value *v, int w, int h, int components, int bits) {
    Value *parms = get(d, v, "DecodeParms");
    if (parms && parms->kind == ARRAY)
        parms = item(d, parms, 0);
    int predictor = 1;
    Value *n;
    if (parms && (n = get(d, parms, "Predictor")))
        predictor = integer(d, n, 1, 15);
    size_t row = ((size_t)w * components * bits + 7) / 8, total = row * h;
    if (total > FILE_LIMIT)
        fail(d, "Decoded image exceeds 64 MiB");
    if (predictor == 1) {
        if (v->decoded_len != total)
            fail(d, "Incorrect image byte count");
        return v->decoded;
    }
    int columns = 1, colors = 1, bpc = 8;
    if ((n = get(d, parms, "Columns")))
        columns = integer(d, n, 1, 16384);
    if ((n = get(d, parms, "Colors")))
        colors = integer(d, n, 1, 4);
    if ((n = get(d, parms, "BitsPerComponent")))
        bpc = integer(d, n, 1, 16);
    if (columns != w || colors != components || bpc != bits)
        fail(d, "Image predictor parameters do not match image");
    unsigned char *out = alloc(d, total);
    int bpp = MAX(1, (components * bits + 7) / 8);
    if (predictor == 2) {
        if (bits != 8 || v->decoded_len != total)
            fail(d, "TIFF predictor requires 8-bit samples");
        memcpy(out, v->decoded, total);
        for (int y = 0; y < h; y++)
            for (size_t x = (size_t)bpp; x < row; x++)
                out[y * row + x] = (unsigned char)(out[y * row + x] + out[y * row + x - bpp]);
        return out;
    }
    if (predictor < 10 || v->decoded_len != (row + 1) * h)
        fail(d, "Invalid PNG predictor data");
    for (int y = 0; y < h; y++) {
        int filter = v->decoded[y * (row + 1)];
        if (filter > 4)
            fail(d, "Invalid PNG predictor filter");
        for (size_t x = 0; x < row; x++) {
            int a = x >= (size_t)bpp ? out[y * row + x - bpp] : 0,
                b = y ? out[(y - 1) * row + x] : 0,
                c = y && x >= (size_t)bpp ? out[(y - 1) * row + x - bpp] : 0, add = 0;
            if (filter == 1)
                add = a;
            else if (filter == 2)
                add = b;
            else if (filter == 3)
                add = (a + b) / 2;
            else if (filter == 4) {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            out[y * row + x] = (unsigned char)(v->decoded[y * (row + 1) + 1 + x] + add);
        }
    }
    return out;
}
static unsigned char *image(Pdf *d, Value *v, int *width, int *height, int depth) {
    if (depth > 4)
        fail(d, "Image soft mask nesting limit");
    if (get(d, v, "OC"))
        fail(d, "Optional image content unsupported");
    if (!named(get(d, v, "Subtype"), "Image") || !v->stream)
        fail(d, "Only Image XObjects are supported");
    int w = integer(d, get(d, v, "Width"), 1, 16384), h = integer(d, get(d, v, "Height"), 1, 16384);
    if ((size_t)w * h > 16000000)
        fail(d, "Image exceeds 16 million pixels");
    *width = w;
    *height = h;
    if (v->rgba)
        return v->rgba;
    Value *mask = get(d, v, "ImageMask");
    if (mask && mask->kind == WORD && !strcmp(mask->text, "true"))
        fail(d, "Stencil images unsupported");
    Value *cs = get(d, v, "ColorSpace");
    int components = 0, indexed = 0, palette_components = 0, high = 0;
    bool calibrated = false;
    double gamma[3] = {1, 1, 1}, calmatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const unsigned char *palette = NULL;
    size_t palette_size = 0;
    Value *base = cs;
    if (cs && cs->kind == ARRAY && cs->count == 4 && named(item(d, cs, 0), "Indexed")) {
        indexed = 1;
        base = item(d, cs, 1);
    }
    if (named(base, "DeviceRGB"))
        components = 3;
    else if (named(base, "DeviceGray"))
        components = 1;
    else if (base && base->kind == ARRAY && base->count == 2 && named(item(d, base, 0), "CalRGB")) {
        components = 3;
        calibrated = true;
        Value *params = item(d, base, 1), *a = get(d, params, "Gamma"), *m = get(d, params, "Matrix");
        if (a)
            for (int k = 0; k < 3; k++)
                gamma[k] = num(d, item(d, a, k));
        if (m)
            for (int k = 0; k < 9; k++)
                calmatrix[k] = num(d, item(d, m, k));
        Value *wp = get(d, params, "WhitePoint");
        if (!wp || fabs(num(d, item(d, wp, 0)) - .95046) > .002 ||
            fabs(num(d, item(d, wp, 2)) - 1.08906) > .002)
            fail(d, "CalRGB currently requires D65 white point");
    } else
        fail(d, "Images require DeviceRGB, DeviceGray, CalRGB or an Indexed palette with such a base");
    if (indexed) {
        palette_components = components;
        high = integer(d, item(d, cs, 2), 0, 255);
        Value *lookup = item(d, cs, 3);
        if (lookup->kind == STRING) {
            palette = (unsigned char *)lookup->text;
            palette_size = lookup->len;
        } else if (lookup->stream) {
            palette = lookup->decoded;
            palette_size = lookup->decoded_len;
        } else
            fail(d, "Invalid image palette");
        if (palette_size < (size_t)(high + 1) * palette_components)
            fail(d, "Truncated image palette");
        components = 1;
    }
    Value *bpc = get(d, v, "BitsPerComponent");
    int bits = bpc ? integer(d, bpc, 1, 8) : 8;
    if ((bits != 1 && bits != 2 && bits != 4 && bits != 8) || (components == 3 && bits != 8))
        fail(d, "Unsupported image sample depth");
    unsigned char *rgba = alloc(d, (size_t)w * h * 4), *samples = NULL;
    Value *filter = get(d, v, "Filter");
    if (filter && filter->kind == ARRAY)
        filter = item(d, filter, 0);
    if (named(filter, "DCTDecode")) {
        if (indexed || bits != 8 || get(d, v, "DecodeParms"))
            fail(d, "JPEG parameters/Indexed JPEG unsupported");
        samples = alloc(d, (size_t)w * h * components);
        Jpeg *j = alloc(d, sizeof(*j));
        j->info.err = jpeg_std_error(&j->error);
        j->error.error_exit = jpeg_failure;
        j->info.client_data = j;
        if (setjmp(j->jump)) {
            if (j->created)
                jpeg_destroy_decompress(&j->info);
            fail(d, "Invalid or unsupported JPEG image");
        }
        jpeg_create_decompress(&j->info);
        j->created = true;
        j->info.client_data = j;
        jpeg_mem_src(&j->info, v->decoded, (unsigned long)v->decoded_len);
        jpeg_read_header(&j->info, TRUE);
        if (j->info.jpeg_color_space == JCS_CMYK || j->info.jpeg_color_space == JCS_YCCK) {
            jpeg_destroy_decompress(&j->info);
            j->created = false;
            fail(d, "CMYK JPEG unsupported");
        }
        j->info.out_color_space = components == 3 ? JCS_RGB : JCS_GRAYSCALE;
        if (j->info.image_width != (unsigned)w || j->info.image_height != (unsigned)h) {
            jpeg_destroy_decompress(&j->info);
            j->created = false;
            fail(d, "JPEG dimensions disagree with PDF image");
        }
        jpeg_start_decompress(&j->info);
        while (j->info.output_scanline < j->info.output_height) {
            JSAMPROW row = samples + (size_t)j->info.output_scanline * w * components;
            jpeg_read_scanlines(&j->info, &row, 1);
        }
        jpeg_finish_decompress(&j->info);
        jpeg_destroy_decompress(&j->info);
        j->created = false;
    } else
        samples = predict(d, v, w, h, components, bits);
    Value *decode = get(d, v, "Decode");
    double low[3] = {0, 0, 0}, upper[3] = {1, 1, 1};
    if (indexed)
        upper[0] = (1 << bits) - 1;
    if (decode) {
        if (decode->kind != ARRAY || decode->count != (size_t)components * 2)
            fail(d, "Invalid image Decode array");
        for (int k = 0; k < components; k++) {
            low[k] = num(d, item(d, decode, 2 * k));
            upper[k] = num(d, item(d, decode, 2 * k + 1));
        }
    }
    /* SMask takes precedence over Mask. Color-key ranges refer to the raw
     * samples, before Decode, palette lookup or color-space conversion. */
    Value *smask = get(d, v, "SMask");
    if (named(smask, "None"))
        smask = NULL;
    Value *key = smask ? NULL : get(d, v, "Mask");
    int key_low[3] = {0}, key_high[3] = {0};
    if (key) {
        if (key->kind != ARRAY)
            fail(d, "Explicit image masks unsupported");
        if (key->count != (size_t)components * 2)
            fail(d, "Invalid image color-key mask array");
        for (int k = 0; k < components; k++) {
            key_low[k] = integer(d, item(d, key, 2 * k), 0, (1 << bits) - 1);
            key_high[k] = integer(d, item(d, key, 2 * k + 1), 0, (1 << bits) - 1);
            if (key_low[k] > key_high[k])
                fail(d, "Invalid image color-key mask range");
        }
    }
    int mw = 0, mh = 0;
    unsigned char *alpha = NULL;
    Value *matte = NULL;
    double matte_color[3] = {0};
    if (smask) {
        if (!named(get(d, smask, "ColorSpace"), "DeviceGray"))
            fail(d, "Soft mask requires DeviceGray");
        alpha = image(d, smask, &mw, &mh, depth + 1);
        matte = get(d, smask, "Matte");
        if (matte) {
            if (indexed)
                fail(d, "Matte requires a non-Indexed image color space");
            if (matte->kind != ARRAY || matte->count != (size_t)components || mw != w || mh != h)
                fail(d, "Invalid image Matte array or dimensions");
            for (int k = 0; k < components; k++) {
                matte_color[k] = num(d, item(d, matte, k));
                if (matte_color[k] < 0 || matte_color[k] > 1)
                    fail(d, "Invalid image Matte component");
            }
        }
    }
    size_t row = ((size_t)w * components * bits + 7) / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char values[3] = {0, 0, 0};
            bool transparent = key != NULL;
            unsigned char opacity = alpha ? alpha[((size_t)(y * mh / h) * mw + x * mw / w) * 4]
                                          : 255;
            for (int k = 0; k < components; k++) {
                size_t bit = ((size_t)x * components + k) * bits;
                int sample =
                    (samples[y * row + bit / 8] >> (8 - bits - (bit % 8))) & ((1 << bits) - 1);
                if (key && (sample < key_low[k] || sample > key_high[k]))
                    transparent = false;
                double a = low[k] + (upper[k] - low[k]) * sample / ((1 << bits) - 1);
                /* Undo preblending in the source color space, before rounding
                 * to bytes or converting CalRGB. Fully transparent pixels
                 * have no recoverable color and must not divide by zero. */
                if (matte)
                    a = opacity ? matte_color[k] + (a - matte_color[k]) * 255 / opacity : 0;
                a = MAX(0, MIN(indexed ? high : 1, a));
                values[k] = (unsigned char)(indexed ? (int)(a + .5) : (int)(a * 255 + .5));
            }
            unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            for (int k = 0; k < 3; k++)
                p[k] = indexed ? palette[values[0] * palette_components +
                                         (palette_components == 1 ? 0 : k)]
                               : values[components == 1 ? 0 : k];
            if (calibrated) {
                double c[3], xyz[3] = {0};
                for (int k = 0; k < 3; k++)
                    c[k] = pow(p[k] / 255.0, gamma[k]);
                for (int k = 0; k < 3; k++)
                    xyz[k] =
                        calmatrix[k] * c[0] + calmatrix[3 + k] * c[1] + calmatrix[6 + k] * c[2];
                double linear[3] = {3.2404542 * xyz[0] - 1.5371385 * xyz[1] - .4985314 * xyz[2],
                                    -.969266 * xyz[0] + 1.8760108 * xyz[1] + .041556 * xyz[2],
                                    .0556434 * xyz[0] - .2040259 * xyz[1] + 1.0572252 * xyz[2]};
                for (int k = 0; k < 3; k++) {
                    double a = linear[k] <= .0031308 ? 12.92 * linear[k]
                                                     : 1.055 * pow(linear[k], 1 / 2.4) - .055;
                    p[k] = (unsigned char)(MAX(0, MIN(1, a)) * 255 + .5);
                }
            }
            p[3] = transparent ? 0 : opacity;
        }
    size_t bytes = (size_t)w * h * 4;
    if (!depth && !d->image_cache_closed && bytes <= IMAGE_CACHE_LIMIT - d->image_cache_bytes) {
        v->rgba = malloc(bytes);
        if (v->rgba) {
            memcpy(v->rgba, rgba, bytes);
            d->image_cache_bytes += bytes;
            return v->rgba;
        }
    }
    return rgba;
}
typedef struct {
    Matrix ctm;
    uint32_t fill, stroke;
    double width, miter;
    int cap, join;
    Box clip;
    double fill_alpha, stroke_alpha;
    const unsigned char *mask, *softmask;
} Graphics;
static uint32_t color(Pdf *d, Value **args, int n) {
    double c[4] = {0};
    for (int i = 0; i < n; i++)
        c[i] = MAX(0, MIN(1, num(d, args[i])));
    double r = c[0], g = c[0], b = c[0];
    if (n == 3) {
        g = c[1];
        b = c[2];
    } else if (n == 4) {
        /* Multilinear interpolation in a characterized default CMYK color cube.
         * DeviceCMYK does not specify an output profile; this is an explicit default. */
        int base[4];
        double fraction[4], rgb[3] = {0};
        for (int k = 0; k < 4; k++) {
            double x = c[k] * 8;
            base[k] = (int)x;
            if (base[k] == 8)
                base[k] = 7;
            fraction[k] = x - base[k];
        }
        for (int corner = 0; corner < 16; corner++) {
            size_t index = 0;
            double weight = 1;
            for (int k = 0; k < 4; k++) {
                int high = (corner >> k) & 1;
                index = index * 9 + base[k] + high;
                weight *= high ? fraction[k] : 1 - fraction[k];
            }
            for (int k = 0; k < 3; k++)
                rgb[k] += weight * cmyk_samples[index * 3 + k] / 255.0;
        }
        r = rgb[0];
        g = rgb[1];
        b = rgb[2];
    }
    return (uint32_t)(r * 255 + .5) << 16 | (uint32_t)(g * 255 + .5) << 8 |
           (uint32_t)(b * 255 + .5);
}
static Point argpoint(Pdf *d, Graphics *g, Value **a, int i) {
    return matrix_point(g->ctm, (Point){num(d, a[i]), num(d, a[i + 1])});
}
static void require(Pdf *d, bool condition, const char *s) {
    if (!condition)
        fail(d, "%s", s);
}
typedef struct {
    PdfFont *font;
    double size, charspace, wordspace, hscale, leading, rise;
    int mode;
} Text;
/* WinAnsiEncoding as defined by PDF: Windows-1252, with unused codes shown as
 * bullets and the non-breaking space and soft hyphen drawn as their plain forms. */
static uint32_t winansi_unicode(unsigned code) {
    static const uint16_t c1[32] = {
        0x20ac, 0x2022, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x2022, 0x017d, 0x2022, 0x2022, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
        0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x2022, 0x017e, 0x0178};
    if (code >= 0x80 && code <= 0x9f)
        return c1[code - 0x80];
    return code == 0x7f ? 0x2022 : code == 0xa0 ? 0x20 : code == 0xad ? 0x2d : code;
}
static void text_show(Pdf *d, Canvas *canvas, Graphics *g, Text *text, Matrix *tm, Value *str) {
    require(d, str && str->kind == STRING && text->font, "Text show requires a font and string");
    unsigned stride = text->font->identity ? 2 : 1;
    require(d, str->len % stride == 0, "Incomplete composite font code");
    for (size_t i = 0; i < str->len; i += stride) {
        unsigned code = (unsigned char)str->text[i];
        if (stride == 2)
            code = (code << 8) | (unsigned char)str->text[i + 1];
        Matrix scale = {text->size * text->hscale, 0, 0, text->size, 0, text->rise};
        Matrix transform = matrix_mul(g->ctm, matrix_mul(*tm, scale));
        Path *path = &d->work;
        size_t old = path->count;
        bool started = path->started, new_subpath = path->new_subpath;
        Point current = path->current, start = path->start;
        /* PDF glyph outlines must not modify the current content path. */
        Path glyph = {0};
        char error[160];
        if (text->font->truetype) {
            if (!text_glyph_path(text->font->truetype, winansi_unicode(code), transform, &glyph)) {
                path_free(&glyph);
                fail(d, "Cannot outline TrueType glyph");
            }
        } else if (text->font->type1) {
            if (!type1_outline(text->font->type1, text->font->type1_glyphs[code], transform, &glyph,
                               error, sizeof(error))) {
                path_free(&glyph);
                fail(d, "Type 1 glyph: %s", error);
            }
        } else if (!cff_outline(text->font->cff, code, text->font->identity, transform, &glyph,
                                error, sizeof(error))) {
            path_free(&glyph);
            fail(d, "CFF glyph: %s", error);
        }
        bool ok = true;
        if (text->mode == 0 || text->mode == 2)
            ok = canvas_fill(canvas, &glyph, g->fill, false);
        if (text->mode == 1 || text->mode == 2)
            ok =
                ok && canvas_stroke(canvas, &glyph, g->stroke, g->width * hypot(g->ctm.a, g->ctm.b),
                                    g->cap, g->join, g->miter);
        path_free(&glyph);
        require(d, ok, "Cannot render text outline");
        path->count = old;
        path->started = started;
        path->new_subpath = new_subpath;
        path->current = current;
        path->start = start;
        double advance = (text->font->widths[code] * .001 * text->size + text->charspace +
                          (stride == 1 && code == 32 ? text->wordspace : 0)) *
                         text->hscale;
        *tm = matrix_mul(*tm, (Matrix){1, 0, 0, 1, advance, 0});
    }
}
static void interpret(Pdf *d, Page *page, Canvas *canvas, const unsigned char *data, size_t size,
                      Matrix view, const Graphics *initial, int depth);
static Box transformed_box(Matrix matrix, Box b) {
    Point p[4] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    Box out = {1e30, 1e30, -1e30, -1e30};
    for (int i = 0; i < 4; i++) {
        Point q = matrix_point(matrix, p[i]);
        out.x0 = MIN(out.x0, q.x);
        out.y0 = MIN(out.y0, q.y);
        out.x1 = MAX(out.x1, q.x);
        out.y1 = MAX(out.y1, q.y);
    }
    return out;
}
static const unsigned char *path_clip(Pdf *d, Canvas *canvas, const Path *path,
                                      const unsigned char *old, bool evenodd) {
    size_t pixels = (size_t)canvas->w * canvas->h;
    unsigned char *coverage = alloc(d, pixels);
    Allocation *mark = d->allocs;
    Canvas tmp = *canvas;
    tmp.pixels = alloc(d, pixels * 4);
    tmp.mask = tmp.softmask = NULL;
    tmp.opacity = 1;
    canvas_clear(&tmp, 0);
    require(d, canvas_fill(&tmp, path, 0xffffff, evenodd), "Clip rasterization failed");
    for (size_t i = 0; i < pixels; i++)
        coverage[i] =
            old ? (unsigned char)((tmp.pixels[4 * i] * old[i] + 127) / 255) : tmp.pixels[4 * i];
    rollback(d, mark);
    return coverage;
}
static void form_draw(Pdf *d, Page *parent, Value *form, Canvas *canvas, Graphics state,
                      int depth) {
    require(d, depth < 24, "Form recursion limit");
    require(d, named(get(d, form, "Subtype"), "Form") && form->decoded, "Expected Form XObject");
    Value *matrix = get(d, form, "Matrix");
    if (matrix) {
        Matrix m = {num(d, item(d, matrix, 0)), num(d, item(d, matrix, 1)),
                    num(d, item(d, matrix, 2)), num(d, item(d, matrix, 3)),
                    num(d, item(d, matrix, 4)), num(d, item(d, matrix, 5))};
        state.ctm = matrix_mul(state.ctm, m);
    }
    Box box = transformed_box(state.ctm, box_value(d, get(d, form, "BBox")));
    state.clip = (Box){MAX(state.clip.x0, box.x0), MAX(state.clip.y0, box.y0),
                       MIN(state.clip.x1, box.x1), MIN(state.clip.y1, box.y1)};
    Page page = *parent;
    Value *resources = get(d, form, "Resources");
    if (resources)
        page.resources = resources;
    require(d, d->path_depth < 32, "Path stack overflow");
    d->path_stack[d->path_depth++] = d->work;
    d->work = (Path){0};
    interpret(d, &page, canvas, form->decoded, form->decoded_len, state.ctm, &state, depth + 1);
    path_free(&d->work);
    d->work = d->path_stack[--d->path_depth];
}
static const unsigned char *soft_mask(Pdf *d, Page *page, Value *mask, Canvas *canvas,
                                      Graphics state, int depth) {
    require(d, named(get(d, mask, "S"), "Luminosity"), "Only luminosity form soft masks supported");
    require(d, !get(d, mask, "TR"), "Soft mask transfer functions unsupported");
    size_t pixels = (size_t)canvas->w * canvas->h;
    unsigned char *coverage = alloc(d, pixels);
    Allocation *mark = d->allocs;
    Canvas tmp = *canvas;
    tmp.pixels = alloc(d, pixels * 4);
    tmp.mask = tmp.softmask = NULL;
    tmp.opacity = 1;
    canvas_clear(&tmp, 0);
    state.fill_alpha = state.stroke_alpha = 1;
    state.mask = state.softmask = NULL;
    form_draw(d, page, get(d, mask, "G"), &tmp, state, depth);
    for (size_t i = 0; i < pixels; i++)
        coverage[i] = (unsigned char)((tmp.pixels[4 * i] * 77 + tmp.pixels[4 * i + 1] * 150 +
                                       tmp.pixels[4 * i + 2] * 29 + 128) >>
                                      8);
    rollback(d, mark);
    return coverage;
}
static void function_value(Pdf *d, Value *function, double t, double out[3], int depth) {
    require(d, depth < 16, "Shading function recursion limit");
    int type = integer(d, get(d, function, "FunctionType"), 0, 4);
    Value *domain = get(d, function, "Domain");
    double lo = num(d, item(d, domain, 0)), hi = num(d, item(d, domain, 1));
    t = MAX(lo, MIN(hi, t));
    if (type == 2) {
        Value *c0 = get(d, function, "C0"), *c1 = get(d, function, "C1");
        double n = num(d, get(d, function, "N")), factor = pow(t, n);
        for (int k = 0; k < 3; k++) {
            double a = c0 ? num(d, item(d, c0, k)) : 0, b = c1 ? num(d, item(d, c1, k)) : 1;
            out[k] = a + factor * (b - a);
        }
    } else if (type == 3) {
        Value *functions = get(d, function, "Functions"), *bounds = get(d, function, "Bounds"),
              *encode = get(d, function, "Encode");
        require(d, functions && functions->kind == ARRAY && functions->count > 0,
                "Invalid stitching function");
        size_t i = 0;
        double lower = lo, upper = hi;
        for (; i + 1 < functions->count; i++) {
            upper = num(d, item(d, bounds, i));
            if (t < upper)
                break;
            lower = upper;
        }
        if (i + 1 == functions->count)
            upper = hi;
        double a = num(d, item(d, encode, 2 * i)), b = num(d, item(d, encode, 2 * i + 1));
        require(d, upper > lower, "Empty stitching interval");
        function_value(d, item(d, functions, i), a + (t - lower) / (upper - lower) * (b - a), out,
                       depth + 1);
    } else
        fail(d, "Unsupported shading function");
}
static bool truth(Value *v) { return v && v->kind == WORD && !strcmp(v->text, "true"); }
static void shading(Pdf *d, Value *sh, Canvas *canvas, Graphics *g) {
    int type = integer(d, get(d, sh, "ShadingType"), 2, 3);
    require(d, named(get(d, sh, "ColorSpace"), "DeviceRGB"), "Shadings require DeviceRGB");
    Value *coords = get(d, sh, "Coords"), *domain = get(d, sh, "Domain"),
          *extend = get(d, sh, "Extend");
    double xy[6] = {0};
    for (int i = 0; i < (type == 2 ? 4 : 6); i++)
        xy[i] = num(d, item(d, coords, i));
    if (type == 3)
        require(d, fabs(xy[0] - xy[3]) < 1e-7 && fabs(xy[1] - xy[4]) < 1e-7 && xy[5] > xy[2],
                "Radial shading currently requires concentric growing circles");
    double lo = domain ? num(d, item(d, domain, 0)) : 0,
           hi = domain ? num(d, item(d, domain, 1)) : 1;
    bool before = extend && truth(item(d, extend, 0)), after = extend && truth(item(d, extend, 1));
    uint32_t lut[1024];
    for (int i = 0; i < 1024; i++) {
        double c[3];
        function_value(d, get(d, sh, "Function"), lo + (hi - lo) * i / 1023.0, c, 0);
        require(d, isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]),
                "Invalid shading function output");
        lut[i] = (uint32_t)(MAX(0, MIN(1, c[0])) * 255 + .5) << 16 |
                 (uint32_t)(MAX(0, MIN(1, c[1])) * 255 + .5) << 8 |
                 (uint32_t)(MAX(0, MIN(1, c[2])) * 255 + .5);
    }
    Matrix inv;
    require(d, matrix_inverse(g->ctm, &inv), "Singular shading transform");
    int xa = (int)MAX(0, floor(g->clip.x0)), ya = (int)MAX(0, floor(g->clip.y0)),
        xb = (int)MIN(canvas->w, ceil(g->clip.x1)), yb = (int)MIN(canvas->h, ceil(g->clip.y1));
    double dx = xy[2] - xy[0], dy = xy[3] - xy[1], len2 = dx * dx + dy * dy;
    require(d, type != 2 || len2 > 1e-20, "Empty axial shading axis");
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            Point p = matrix_point(inv, (Point){x + .5, y + .5});
            double t = type == 2 ? ((p.x - xy[0]) * dx + (p.y - xy[1]) * dy) / len2
                                 : (hypot(p.x - xy[0], p.y - xy[1]) - xy[2]) / (xy[5] - xy[2]);
            if ((t < 0 && !before) || (t > 1 && !after))
                continue;
            int i = (int)(MAX(0, MIN(1, t)) * 1023 + .5);
            canvas_paint(canvas, x, y, lut[i], 255);
        }
}
static void remember_placeholder(Pdf *d, Page *page, const Path *path, Matrix view) {
    if (!page->animation_count || d->placeholder_count >= 64 || path->count != 4)
        return;
    Matrix inverse;
    if (!matrix_inverse(view, &inverse))
        return;
    Box b = {1e30, 1e30, -1e30, -1e30};
    for (size_t i = 0; i < path->count; i++) {
        Point a = matrix_point(inverse, path->edges[i].a),
              z = matrix_point(inverse, path->edges[i].b);
        if (fabs(a.x - z.x) > 1e-5 && fabs(a.y - z.y) > 1e-5)
            return;
        b.x0 = MIN(b.x0, a.x);
        b.x1 = MAX(b.x1, a.x);
        b.y0 = MIN(b.y0, a.y);
        b.y1 = MAX(b.y1, a.y);
    }
    double w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (w < 20 || h < 20 ||
        w * h > .7 * (page->box.x1 - page->box.x0) * (page->box.y1 - page->box.y0))
        return;
    d->placeholders[d->placeholder_count++] = b;
}
static void interpret(Pdf *d, Page *page, Canvas *canvas, const unsigned char *data, size_t size,
                      Matrix view, const Graphics *initial, int depth) {
    Parser p = {d, data, size, 0};
    Graphics g = {.ctm = view,
                  .width = 1,
                  .miter = 10,
                  .clip = canvas->clip,
                  .fill_alpha = 1,
                  .stroke_alpha = 1},
             stack[32];
    if (initial)
        g = *initial;
    int sp = 0;
    Text text = {0}, text_stack[32];
    text.hscale = 1;
    Matrix tm = {1, 0, 0, 1, 0, 0}, line_matrix = tm;
    bool in_text = false;
    Value *args[64];
    int count = 0;
    Path *path = &d->work;
    bool clip_pending = false, rect_path = false, clip_evenodd = false;
    Box clip_box = {0};
    while (1) {
        skip(&p);
        if (p.p >= p.n)
            break;
        Value *op = value(&p, 0);
        if (op->kind != WORD) {
            if (count == 64)
                fail(d, "Operand stack limit exceeded");
            args[count++] = op;
            continue;
        }
        const char *s = op->text;
#define OP(x, n)                                                                                   \
    (!strcmp(s, x) && (require(d, count == (n), "Wrong PDF operator operand count"), true))
        canvas->clip = g.clip;
        canvas->mask = g.mask;
        canvas->softmask = g.softmask;
        canvas->opacity = g.fill_alpha;
        if (OP("q", 0)) {
            require(d, sp < 32, "Graphics stack overflow");
            text_stack[sp] = text;
            stack[sp++] = g;
        } else if (OP("Q", 0)) {
            require(d, sp > 0, "Unbalanced Q");
            g = stack[--sp];
            text = text_stack[sp];
        } else if (OP("cm", 6)) {
            Matrix m = {num(d, args[0]), num(d, args[1]), num(d, args[2]),
                        num(d, args[3]), num(d, args[4]), num(d, args[5])};
            g.ctm = matrix_mul(g.ctm, m);
            require(d,
                    isfinite(g.ctm.a) && isfinite(g.ctm.b) && isfinite(g.ctm.c) &&
                        isfinite(g.ctm.d) && isfinite(g.ctm.e) && isfinite(g.ctm.f),
                    "PDF transform overflow");
        } else if (OP("w", 1)) {
            g.width = num(d, args[0]);
            require(d, g.width >= 0, "Negative line width");
        } else if (OP("J", 1))
            g.cap = integer(d, args[0], 0, 2);
        else if (OP("j", 1))
            g.join = integer(d, args[0], 0, 2);
        else if (OP("M", 1)) {
            g.miter = num(d, args[0]);
            require(d, g.miter >= 1, "Invalid miter limit");
        } else if (OP("d", 2)) {
            require(d, args[0]->kind == ARRAY && args[0]->count == 0, "Dashed strokes unsupported");
            (void)num(d, args[1]);
        } else if (OP("ri", 1)) {
            require(d, args[0]->kind == NAME, "Invalid rendering intent");
        } else if (OP("i", 1)) {
            (void)num(d, args[0]);
        } else if (OP("g", 1) || OP("rg", 3) || OP("k", 4))
            g.fill = color(d, args, count);
        else if (OP("G", 1) || OP("RG", 3) || OP("K", 4))
            g.stroke = color(d, args, count);
        else if (OP("m", 2)) {
            require(d, path_move(path, argpoint(d, &g, args, 0)), "Invalid move");
            rect_path = false;
        } else if (OP("l", 2)) {
            require(d, path_line(path, argpoint(d, &g, args, 0)), "Invalid path");
            rect_path = false;
        } else if (OP("c", 6)) {
            require(d,
                    path_curve(path, argpoint(d, &g, args, 0), argpoint(d, &g, args, 2),
                               argpoint(d, &g, args, 4)),
                    "Invalid curve");
            rect_path = false;
        } else if (OP("v", 4)) {
            require(
                d,
                path_curve(path, path->current, argpoint(d, &g, args, 0), argpoint(d, &g, args, 2)),
                "Invalid curve");
            rect_path = false;
        } else if (OP("y", 4)) {
            Point end = argpoint(d, &g, args, 2);
            require(d, path_curve(path, argpoint(d, &g, args, 0), end, end), "Invalid curve");
            rect_path = false;
        } else if (OP("h", 0)) {
            require(d, path_close(path), "Invalid closepath");
        } else if (OP("re", 4)) {
            double x = num(d, args[0]), y = num(d, args[1]), w = num(d, args[2]),
                   h = num(d, args[3]);
            Point a = matrix_point(g.ctm, (Point){x, y}),
                  b = matrix_point(g.ctm, (Point){x + w, y}),
                  c = matrix_point(g.ctm, (Point){x + w, y + h}),
                  e = matrix_point(g.ctm, (Point){x, y + h});
            rect_path = path->count == 0 && ((fabs(g.ctm.b) < 1e-9 && fabs(g.ctm.c) < 1e-9) ||
                                             (fabs(g.ctm.a) < 1e-9 && fabs(g.ctm.d) < 1e-9));
            clip_box = (Box){MIN(a.x, c.x), MIN(a.y, c.y), MAX(a.x, c.x), MAX(a.y, c.y)};
            require(d,
                    path_move(path, a) && path_line(path, b) && path_line(path, c) &&
                        path_line(path, e) && path_close(path),
                    "Invalid rectangle");
        } else if (OP("W", 0) || OP("W*", 0)) {
            clip_pending = true;
            clip_evenodd = strchr(s, '*') != NULL;
        } else if (OP("S", 0) || OP("s", 0) || OP("f", 0) || OP("F", 0) || OP("f*", 0) ||
                   OP("B", 0) || OP("B*", 0) || OP("b", 0) || OP("b*", 0) || OP("n", 0)) {
            bool stroke = s[0] == 'S' || s[0] == 's' || s[0] == 'B' || s[0] == 'b';
            bool fill = s[0] == 'f' || s[0] == 'F' || s[0] == 'B' || s[0] == 'b';
            if (fill && rect_path && g.fill == 0 && g.fill_alpha == 1 && depth == 0)
                remember_placeholder(d, page, path, view);
            if (s[0] == 's' || s[0] == 'b')
                require(d, path_close(path), "Invalid closepath");
            if (fill)
                require(d, canvas_fill(canvas, path, g.fill, strchr(s, '*') != NULL),
                        "Rasterizer allocation failed");
            if (stroke) {
                canvas->opacity = g.stroke_alpha;
                double sx = hypot(g.ctm.a, g.ctm.b), sy = hypot(g.ctm.c, g.ctm.d),
                       dot = g.ctm.a * g.ctm.c + g.ctm.b * g.ctm.d,
                       det = g.ctm.a * g.ctm.d - g.ctm.b * g.ctm.c;
                bool circular =
                    fabs(sx - sy) < 1e-6 * MAX(1, sx) && fabs(dot) < 1e-6 * MAX(1, sx * sy);
                bool ok;
                if (circular || !(g.width > 0) || fabs(det) < 1e-12)
                    ok = canvas_stroke(canvas, path, g.stroke, g.width * sx, g.cap, g.join, g.miter);
                else {
                    /* Unit-determinant pen, keeping curve flattening at output scale. */
                    double s = sqrt(fabs(det));
                    Matrix pen = {g.ctm.a / s, g.ctm.b / s, g.ctm.c / s, g.ctm.d / s, 0, 0};
                    ok = canvas_stroke_pen(canvas, path, g.stroke, g.width * s, g.cap, g.join,
                                           g.miter, pen);
                }
                require(d, ok, "Stroke allocation failed");
            }
            if (clip_pending) {
                if (rect_path)
                    g.clip = (Box){MAX(g.clip.x0, clip_box.x0), MAX(g.clip.y0, clip_box.y0),
                                   MIN(g.clip.x1, clip_box.x1), MIN(g.clip.y1, clip_box.y1)};
                else
                    g.mask = path_clip(d, canvas, path, g.mask, clip_evenodd);
                clip_pending = false;
            }
            path->count = 0;
            path->started = false;
            rect_path = false;
        } else if (OP("Do", 1)) {
            require(d, args[0]->kind == NAME, "Expected XObject name");
            Value *objects = get(d, page->resources, "XObject"),
                  *v = get(d, objects, args[0]->text);
            if (named(get(d, v, "Subtype"), "Form"))
                form_draw(d, page, v, canvas, g, depth);
            else {
                int w, h;
                unsigned char *pixels = image(d, v, &w, &h, 0);
                canvas_image_scaled(canvas, &d->scaler, pixels, w, h, g.ctm);
            }
        } else if (OP("gs", 1)) {
            require(d, args[0]->kind == NAME, "Invalid graphics state name");
            Value *state = get(d, get(d, page->resources, "ExtGState"), args[0]->text);
            require(d, state && state->kind == DICT, "Missing ExtGState");
            Value *v;
            if ((v = get(d, state, "ca")))
                g.fill_alpha = MAX(0, MIN(1, num(d, v)));
            if ((v = get(d, state, "CA")))
                g.stroke_alpha = MAX(0, MIN(1, num(d, v)));
            if ((v = get(d, state, "SMask")))
                g.softmask = named(v, "None") ? NULL : soft_mask(d, page, v, canvas, g, depth);
            if ((v = get(d, state, "BM")))
                require(d, named(v, "Normal"), "Unsupported blend mode");
        } else if (OP("sh", 1)) {
            require(d, args[0]->kind == NAME, "Expected shading name");
            shading(d,
                    get(d, page->resources, "Shading")
                        ? get(d, get(d, page->resources, "Shading"), args[0]->text)
                        : NULL,
                    canvas, &g);
        } else if (OP("BMC", 1)) {
            require(d, args[0]->kind == NAME, "Invalid marked content");
            require(d, !named(args[0], "OC"), "Optional content layers unsupported");
        } else if (OP("BDC", 2)) {
            require(d, args[0]->kind == NAME, "Invalid marked content");
            require(d, !named(args[0], "OC"), "Optional content layers unsupported");
        } else if (OP("EMC", 0) || OP("MP", 1) || OP("DP", 2)) {
        } else if (OP("BT", 0)) {
            require(d, !in_text, "Nested BT");
            in_text = true;
            tm = line_matrix = (Matrix){1, 0, 0, 1, 0, 0};
        } else if (OP("ET", 0)) {
            require(d, in_text, "ET outside text");
            in_text = false;
        } else if (OP("Tf", 2)) {
            require(d, args[0]->kind == NAME, "Invalid font name");
            text.font = font_load(d, get(d, get(d, page->resources, "Font"), args[0]->text));
            text.size = num(d, args[1]);
        } else if (OP("Tm", 6)) {
            tm = line_matrix = (Matrix){num(d, args[0]), num(d, args[1]), num(d, args[2]),
                                        num(d, args[3]), num(d, args[4]), num(d, args[5])};
        } else if (OP("Td", 2) || OP("TD", 2)) {
            double tx = num(d, args[0]), ty = num(d, args[1]);
            if (s[1] == 'D')
                text.leading = -ty;
            line_matrix = matrix_mul(line_matrix, (Matrix){1, 0, 0, 1, tx, ty});
            tm = line_matrix;
        } else if (OP("T*", 0)) {
            line_matrix = matrix_mul(line_matrix, (Matrix){1, 0, 0, 1, 0, -text.leading});
            tm = line_matrix;
        } else if (OP("Tc", 1))
            text.charspace = num(d, args[0]);
        else if (OP("Tw", 1))
            text.wordspace = num(d, args[0]);
        else if (OP("Tz", 1))
            text.hscale = num(d, args[0]) / 100;
        else if (OP("TL", 1))
            text.leading = num(d, args[0]);
        else if (OP("Ts", 1))
            text.rise = num(d, args[0]);
        else if (OP("Tr", 1)) {
            text.mode = integer(d, args[0], 0, 3);
        } else if (OP("Tj", 1)) {
            require(d, in_text, "Text outside BT/ET");
            text_show(d, canvas, &g, &text, &tm, args[0]);
        } else if (OP("TJ", 1)) {
            require(d, in_text && args[0]->kind == ARRAY, "Invalid TJ");
            for (size_t i = 0; i < args[0]->count; i++) {
                Value *v = item(d, args[0], i);
                if (v->kind == STRING)
                    text_show(d, canvas, &g, &text, &tm, v);
                else
                    tm = matrix_mul(
                        tm, (Matrix){1, 0, 0, 1, -num(d, v) * .001 * text.size * text.hscale, 0});
            }
        }

        else
            fail(d, "Unsupported PDF content operator: %.48s", s);
        count = 0;
#undef OP
    }
    require(d, count == 0 && sp == 0 && !clip_pending && !in_text,
            "Unbalanced PDF content/graphics state");
    path->count = 0;
    path->started = false;
}
/* PDF user space -> cropped/rotated top-down raster space -> letterboxed viewport. */
static Matrix page_view(Page *p, int w, int h) {
    double pw = p->box.x1 - p->box.x0, ph = p->box.y1 - p->box.y0;
    Matrix origin = {1, 0, 0, 1, -p->box.x0, -p->box.y0}, rotate = {1, 0, 0, -1, 0, ph};
    if (p->rotate == 90)
        rotate = (Matrix){0, 1, 1, 0, 0, 0};
    if (p->rotate == 180)
        rotate = (Matrix){-1, 0, 0, 1, pw, 0};
    if (p->rotate == 270)
        rotate = (Matrix){0, -1, -1, 0, ph, pw};
    double rw = p->rotate % 180 ? ph : pw, rh = p->rotate % 180 ? pw : ph,
           scale = MIN(w / rw, h / rh);
    Matrix fit = {scale, 0, 0, scale, (w - scale * rw) / 2, (h - scale * rh) / 2};
    return matrix_mul(fit, matrix_mul(rotate, origin));
}
static void render_page(Pdf *d, int page, Canvas *canvas, Matrix *view) {
    require(d, page >= 0 && page < d->page_count, "Invalid page number");
    Page *p = &d->pages[page];
    d->placeholder_count = 0;
    *view = page_view(p, canvas->w, canvas->h);
    canvas->mask = canvas->softmask = NULL;
    canvas->opacity = 1;
    canvas->clip = (Box){0, 0, canvas->w, canvas->h};
    canvas_clear(canvas, 0x101010);
    Point a = matrix_point(*view, (Point){p->box.x0, p->box.y0}),
          b = matrix_point(*view, (Point){p->box.x1, p->box.y1});
    canvas->clip = (Box){MIN(a.x, b.x), MIN(a.y, b.y), MAX(a.x, b.x), MAX(a.y, b.y)};
    Path paper = {0};
    path_move(&paper, (Point){canvas->clip.x0, canvas->clip.y0});
    bool ok = path_line(&paper, (Point){canvas->clip.x1, canvas->clip.y0}) &&
              path_line(&paper, (Point){canvas->clip.x1, canvas->clip.y1}) &&
              path_line(&paper, (Point){canvas->clip.x0, canvas->clip.y1}) && path_close(&paper) &&
              canvas_fill(canvas, &paper, 0xffffff, false);
    path_free(&paper);
    require(d, ok, "Rasterizer allocation failed");
    Value *contents = get(d, p->object, "Contents");
    size_t size = 0;
    if (contents && contents->kind == ARRAY) {
        for (size_t i = 0; i < contents->count; i++) {
            Value *v = item(d, contents, i);
            require(d, v->stream != NULL, "Expected page content stream");
            require(d, v->decoded_len < FILE_LIMIT - size, "Page content too large");
            size += v->decoded_len + 1;
        }
        unsigned char *data = alloc(d, size);
        size_t at = 0;
        for (size_t i = 0; i < contents->count; i++) {
            Value *v = item(d, contents, i);
            memcpy(data + at, v->decoded, v->decoded_len);
            at += v->decoded_len;
            data[at++] = '\n';
        }
        interpret(d, p, canvas, data, size, *view, NULL, 0);
    } else if (contents) {
        require(d, contents->stream != NULL, "Expected page content stream");
        interpret(d, p, canvas, contents->decoded, contents->decoded_len, *view, NULL, 0);
    }
    canvas->clip = (Box){MIN(a.x, b.x), MIN(a.y, b.y), MAX(a.x, b.x), MAX(a.y, b.y)};
}
static void place_animations(Pdf *d, Page *page, Canvas *canvas, Matrix view, bool first_frames) {
    bool used[64] = {0};
    for (int i = 0; i < page->animation_count; i++) {
        AttachedAnimation *attached = &page->animations[i];
        PdfAnimation *info = &attached->info;
        if (attached->legacy && !attached->placed) {
            double x = (attached->anchor.x0 + attached->anchor.x1) * .5;
            double y = (attached->anchor.y0 + attached->anchor.y1) * .5, best = 1e30;
            int candidate = -1;
            for (int j = 0; j < d->placeholder_count; j++) {
                if (used[j])
                    continue;
                Box b = d->placeholders[j];
                double dx = MAX(b.x0 - x, MAX(0, x - b.x1)), dy = MAX(b.y0 - y, MAX(0, y - b.y1));
                double score =
                    dx * dx + dy * dy +
                    .001 * (pow(x - (b.x0 + b.x1) * .5, 2) + pow(y - (b.y0 + b.y1) * .5, 2));
                if (hypot(dx, dy) <= MAX(b.x1 - b.x0, b.y1 - b.y0) && score < best) {
                    best = score;
                    candidate = j;
                }
            }
            require(d, candidate >= 0,
                    "Cannot locate animation placeholder; use the provided \\pdfanimation command");
            used[candidate] = true;
            info->box = d->placeholders[candidate];
            info->positioned = true;
            attached->placed = true;
        }
        Box b = info->box;
        Point corners[4] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
        if (attached->legacy) {
            /* Replace the black \\rule, preserving its fbox and padding. */
            const double pad = .5;
            corners[0] = (Point){b.x0 - pad, b.y0 - pad};
            corners[1] = (Point){b.x1 + pad, b.y0 - pad};
            corners[2] = (Point){b.x1 + pad, b.y1 + pad};
            corners[3] = (Point){b.x0 - pad, b.y1 + pad};
            Path clear = {0};
            bool ok = path_move(&clear, matrix_point(view, corners[0]));
            for (int k = 1; k < 4 && ok; k++)
                ok = path_line(&clear, matrix_point(view, corners[k]));
            ok = ok && path_close(&clear) && canvas_fill(canvas, &clear, 0xffffff, false);
            path_free(&clear);
            require(d, ok, "Cannot clear animation placeholder");
        }
        if (first_frames) {
            Matrix transform =
                matrix_mul(view, (Matrix){b.x1 - b.x0, 0, 0, b.y1 - b.y0, b.x0, b.y0});
            const Animation *clip = info->clip;
            if (info->interpolation == ANIMATION_BICUBIC)
                canvas_image_bicubic(canvas, clip->frames[0], clip->w, clip->h, transform);
            else
                canvas_image(canvas, clip->frames[0], clip->w, clip->h, transform);
        }
    }
}
static bool render_document_page(Pdf *d, int page, Canvas *canvas, Matrix *view,
                                 bool first_frames) {
    Allocation *mark = d->allocs;
    if (setjmp(d->jump)) {
        rollback(d, mark);
        path_free(&d->work);
        while (d->path_depth)
            path_free(&d->path_stack[--d->path_depth]);
        return false;
    }
    render_page(d, page, canvas, view);
    canvas->mask = canvas->softmask = NULL;
    canvas->opacity = 1;
    place_animations(d, &d->pages[page], canvas, *view, first_frames);
    rollback(d, mark);
    path_free(&d->work);
    return true;
}
bool pdf_render(Pdf *d, int page, Canvas *canvas, Matrix *view) {
    return render_document_page(d, page, canvas, view, true);
}
bool pdf_render_background(Pdf *d, int page, Canvas *canvas, Matrix *view) {
    return render_document_page(d, page, canvas, view, false);
}
static bool same_name(Value *a, Value *b) {
    return a && b && (a->kind == NAME || a->kind == STRING) &&
           (b->kind == NAME || b->kind == STRING) && a->len == b->len &&
           !memcmp(a->text, b->text, a->len);
}
static Value *name_tree(Pdf *d, Value *node, Value *key, int depth, int *budget) {
    if (depth > DEPTH_LIMIT || --*budget < 0)
        fail(d, "Destination name-tree cycle/size limit");
    Value *names = get(d, node, "Names");
    if (names) {
        require(d, names->kind == ARRAY && names->count % 2 == 0, "Invalid destination name tree");
        for (size_t i = 0; i < names->count; i += 2)
            if (same_name(item(d, names, i), key))
                return item(d, names, i + 1);
    }
    Value *kids = get(d, node, "Kids");
    if (kids) {
        require(d, kids->kind == ARRAY, "Invalid name tree children");
        for (size_t i = 0; i < kids->count; i++) {
            Value *v = name_tree(d, item(d, kids, i), key, depth + 1, budget);
            if (v)
                return v;
        }
    }
    return NULL;
}
static int destination(Pdf *d, Value *v, int depth) {
    if (depth > DEPTH_LIMIT)
        fail(d, "Destination cycle limit");
    v = resolve(d, v);
    if (!v)
        return -1;
    if (v->kind == NAME || v->kind == STRING) {
        Value *found = NULL, *legacy = get(d, d->catalog, "Dests");
        if (legacy && v->len == strlen(v->text))
            found = get(d, legacy, v->text);
        if (!found) {
            int budget = 10000;
            Value *tree = get(d, get(d, d->catalog, "Names"), "Dests");
            if (tree)
                found = name_tree(d, tree, v, 0, &budget);
        }
        return found ? destination(d, found, depth + 1) : -1;
    }
    if (v->kind == DICT)
        return destination(d, get(d, v, "D"), depth + 1);
    if (v->kind != ARRAY || !v->count)
        return -1;
    Value *p = item(d, v, 0);
    if (p->kind == NUMBER) {
        int n = integer(d, p, 0, d->page_count - 1);
        return n;
    }
    for (int i = 0; i < d->page_count; i++)
        if (d->pages[i].object == p)
            return i;
    return -1;
}
static bool url_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s++) != *prefix++)
            return false;
    }
    return true;
}
bool pdf_link_at(Pdf *d, int page, Point point, PdfLink *link) {
    if (setjmp(d->jump))
        return false;
    link->page = -1;
    link->url = NULL;
    require(d, page >= 0 && page < d->page_count, "Invalid page");
    Value *annots = get(d, d->pages[page].object, "Annots");
    if (!annots)
        return false;
    for (size_t i = 0; i < annots->count; i++) {
        Value *a = item(d, annots, i);
        if (!named(get(d, a, "Subtype"), "Link"))
            continue;
        Value *flags = get(d, a, "F");
        if (flags && (integer(d, flags, 0, INT_MAX) & (1 | 2 | 32)))
            continue;
        Box b = rect_value(d, get(d, a, "Rect"));
        if (point.x < b.x0 || point.x > b.x1 || point.y < b.y0 || point.y > b.y1)
            continue;
        Value *dest = get(d, a, "Dest"), *action = get(d, a, "A");
        if (dest)
            link->page = destination(d, dest, 0);
        else if (action && named(get(d, action, "S"), "GoTo"))
            link->page = destination(d, get(d, action, "D"), 0);
        else if (action && named(get(d, action, "S"), "URI")) {
            Value *uri = get(d, action, "URI");
            if (uri && uri->kind == STRING && uri->len == strlen(uri->text)) {
                /* Only browser URLs; no shell, JavaScript, local files or launch actions. */
                if (url_prefix(uri->text, "http://") || url_prefix(uri->text, "https://")) {
                    bool valid = true;
                    for (size_t k = 0; k < uri->len; k++)
                        if ((unsigned char)uri->text[k] <= 32)
                            valid = false;
                    if (valid)
                        link->url = uri->text;
                }
            }
        }
        /* A link without a usable target does not hide links beneath it. */
        if (link->page >= 0 || link->url != NULL)
            return true;
    }
    return false;
}
