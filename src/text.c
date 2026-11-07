#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FONT_BYTES (64u << 20)
#define MAX_COMPOSITE_DEPTH 8
#define MAX_GLYPH_PARTS 256 /* Glyph records visited per glyph, across composites. */

struct TextFont {
    uint8_t *data;
    size_t size;
    uint32_t glyf, glyf_len, loca, loca_len, hmtx, hmtx_len, cmap, kern, kern_len;
    int units, loca_long, glyphs, hmetrics;
    int ascender, descender, line_gap, x_height, cap_height;
};

static uint16_t u16(const TextFont *f, size_t at) {
    return at + 2 <= f->size ? (uint16_t)(f->data[at] << 8 | f->data[at + 1]) : 0;
}
static int16_t s16(const TextFont *f, size_t at) { return (int16_t)u16(f, at); }
static uint32_t u32(const TextFont *f, size_t at) {
    return at + 4 <= f->size ? (uint32_t)u16(f, at) << 16 | u16(f, at + 2) : 0;
}
static bool table(const TextFont *f, uint32_t dir, const char *tag, uint32_t *offset,
                  uint32_t *length) {
    int n = u16(f, dir + 4);
    for (int i = 0; i < n; i++) {
        size_t r = dir + 12 + 16 * (size_t)i;
        if (r + 16 > f->size)
            return false;
        if (!memcmp(f->data + r, tag, 4)) {
            uint32_t o = u32(f, r + 8), l = u32(f, r + 12);
            if (o > f->size || l > f->size - o)
                return false;
            *offset = o;
            *length = l;
            return true;
        }
    }
    return false;
}

/* Byte range of a glyph inside 'glyf'; false for empty or invalid glyphs. */
static bool glyph_range(const TextFont *f, int glyph, uint32_t *start, uint32_t *end) {
    if (glyph < 0 || glyph >= f->glyphs)
        return false;
    size_t need = (size_t)(glyph + 2) * (f->loca_long ? 4 : 2);
    if (need > f->loca_len)
        return false;
    if (f->loca_long) {
        *start = u32(f, f->loca + 4 * (size_t)glyph);
        *end = u32(f, f->loca + 4 * (size_t)glyph + 4);
    } else {
        *start = 2u * u16(f, f->loca + 2 * (size_t)glyph);
        *end = 2u * u16(f, f->loca + 2 * (size_t)glyph + 2);
    }
    return *start < *end && *end <= f->glyf_len && *end - *start >= 10;
}
static int cmap_lookup(const TextFont *f, uint32_t sub, uint32_t code) {
    uint16_t format = u16(f, sub);
    if (format == 4) {
        if (code > 0xffff)
            return 0;
        uint32_t seg2 = u16(f, sub + 6), ends = sub + 14, starts = ends + seg2 + 2;
        uint32_t deltas = starts + seg2, ranges = deltas + seg2;
        if (ranges + seg2 > f->size)
            return 0;
        uint32_t lo = 0, hi = seg2 / 2;
        while (lo < hi) { /* first segment whose endCode >= code */
            uint32_t mid = (lo + hi) / 2;
            if (u16(f, ends + 2 * mid) < code)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo >= seg2 / 2 || u16(f, starts + 2 * lo) > code)
            return 0;
        uint16_t start = u16(f, starts + 2 * lo), delta = u16(f, deltas + 2 * lo);
        uint16_t range = u16(f, ranges + 2 * lo);
        if (!range)
            return (uint16_t)(code + delta);
        uint16_t g = u16(f, ranges + 2 * lo + range + 2 * (code - start));
        return g ? (uint16_t)(g + delta) : 0;
    }
    if (format == 12) {
        uint32_t groups = u32(f, sub + 12), lo = 0, hi = groups;
        if (groups > (f->size - sub) / 12)
            return 0;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2, at = sub + 16 + 12 * mid;
            if (u32(f, at + 4) < code)
                lo = mid + 1;
            else if (u32(f, at) > code)
                hi = mid;
            else
                return (int)(u32(f, at + 8) + (code - u32(f, at)));
        }
    }
    return 0;
}
static int glyph_index(const TextFont *f, uint32_t code) {
    int g = cmap_lookup(f, f->cmap, code);
    return g > 0 && g < f->glyphs ? g : 0;
}
static int advance(const TextFont *f, int glyph) {
    int i = glyph < f->hmetrics ? glyph : f->hmetrics - 1;
    return 4 * (size_t)i + 2 <= f->hmtx_len ? u16(f, f->hmtx + 4 * (size_t)i) : 0;
}
/* Legacy 'kern' format 0 horizontal pairs. GPOS kerning is not interpreted. */
static int kerning(const TextFont *f, int left, int right) {
    if (!f->kern || u16(f, f->kern) != 0)
        return 0;
    uint32_t at = f->kern + 4, end = f->kern + f->kern_len;
    int n = u16(f, f->kern + 2), total = 0;
    for (int t = 0; t < n && at + 6 <= end; t++) {
        uint16_t length = u16(f, at + 2), coverage = u16(f, at + 4);
        if (length < 6)
            break;
        if ((coverage & 0xff07) == 0x0001) { /* format 0, horizontal, not min/cross */
            uint32_t pairs = u16(f, at + 6), lo = 0, hi = pairs;
            uint32_t key = (uint32_t)left << 16 | (uint32_t)right;
            if (at + 14 + 6 * pairs <= end)
                while (lo < hi) {
                    uint32_t mid = (lo + hi) / 2, p = at + 14 + 6 * mid;
                    uint32_t k = u32(f, p);
                    if (k < key)
                        lo = mid + 1;
                    else if (k > key)
                        hi = mid;
                    else {
                        total += s16(f, p + 4);
                        break;
                    }
                }
        }
        at += length;
    }
    return total;
}
static int glyph_top(const TextFont *f, uint32_t code) {
    uint32_t start, end;
    int g = glyph_index(f, code);
    return g && glyph_range(f, g, &start, &end) ? s16(f, f->glyf + start + 8) : 0;
}

TextFont *text_font_from_memory(const uint8_t *data, size_t size) {
    if (!data || size < 12 || size > MAX_FONT_BYTES)
        return NULL;
    TextFont *f = calloc(1, sizeof(*f));
    if (!f || !(f->data = malloc(size))) {
        free(f);
        return NULL;
    }
    memcpy(f->data, data, size);
    f->size = size;
    uint32_t dir = 0, off, len, head, hhea, maxp, cmap;
    if (!memcmp(data, "ttcf", 4)) /* First face of a collection. */
        dir = u32(f, 12);
    uint32_t version = u32(f, dir);
    if ((version != 0x00010000 && version != 0x74727565) || /* 'true' */
        !table(f, dir, "head", &head, &len) || len < 54 ||
        !table(f, dir, "hhea", &hhea, &len) || len < 36 ||
        !table(f, dir, "maxp", &maxp, &len) || len < 6 ||
        !table(f, dir, "glyf", &f->glyf, &f->glyf_len) ||
        !table(f, dir, "loca", &f->loca, &f->loca_len) ||
        !table(f, dir, "hmtx", &f->hmtx, &f->hmtx_len) || !table(f, dir, "cmap", &cmap, &len))
        goto fail;
    f->units = u16(f, head + 18);
    f->loca_long = s16(f, head + 50) == 1;
    f->glyphs = u16(f, maxp + 4);
    f->ascender = s16(f, hhea + 4);
    f->descender = s16(f, hhea + 6);
    f->line_gap = s16(f, hhea + 8);
    f->hmetrics = u16(f, hhea + 34);
    if (f->units < 16 || f->units > 16384 || !f->glyphs || !f->hmetrics ||
        (size_t)f->hmetrics * 4 > f->hmtx_len)
        goto fail;
    /* Prefer full Unicode (format 12), then the BMP (format 4). */
    int best = 0;
    for (int i = 0, n = u16(f, cmap + 2); i < n; i++) {
        uint32_t r = cmap + 4 + 8 * (uint32_t)i, sub = cmap + u32(f, r + 4);
        uint16_t platform = u16(f, r), encoding = u16(f, r + 2), format = u16(f, sub);
        bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        int score = !unicode || sub >= f->size ? 0 : format == 12 ? 2 : format == 4 ? 1 : 0;
        if (score > best) {
            best = score;
            f->cmap = sub;
        }
    }
    if (!best)
        goto fail;
    if (table(f, dir, "kern", &off, &len) && len >= 4) {
        f->kern = off;
        f->kern_len = len;
    }
    if (table(f, dir, "OS/2", &off, &len) && len >= 90 && u16(f, off) >= 2) {
        f->x_height = s16(f, off + 86);
        f->cap_height = s16(f, off + 88);
    }
    if (f->x_height <= 0)
        f->x_height = glyph_top(f, 'x');
    if (f->cap_height <= 0)
        f->cap_height = glyph_top(f, 'H');
    return f;
fail:
    text_font_free(f);
    return NULL;
}
TextFont *text_font_load(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    TextFont *font = NULL;
    uint8_t *data = NULL;
    long size = -1;
    if (!fseek(file, 0, SEEK_END))
        size = ftell(file);
    if (size > 0 && (unsigned long)size <= MAX_FONT_BYTES && !fseek(file, 0, SEEK_SET) &&
        (data = malloc((size_t)size)) && fread(data, 1, (size_t)size, file) == (size_t)size)
        font = text_font_from_memory(data, (size_t)size);
    free(data);
    fclose(file);
    return font;
}
TextFont *text_font_open_default(void) {
    const char *env = getenv("SDL_PDF_VIEWER_FONT");
    if (env && *env)
        return text_font_load(env);
    static const char *const windows[] = {"segoeui.ttf", "arial.ttf", "calibri.ttf",
                                          "tahoma.ttf", "verdana.ttf"};
    static const char *const paths[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Geneva.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    };
    const char *windir = getenv("WINDIR");
    if (windir && *windir)
        for (size_t i = 0; i < sizeof(windows) / sizeof(*windows); i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, windows[i]);
            TextFont *f = text_font_load(path);
            if (f)
                return f;
        }
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
        TextFont *f = text_font_load(paths[i]);
        if (f)
            return f;
    }
    return NULL;
}
void text_font_free(TextFont *f) {
    if (f)
        free(f->data);
    free(f);
}

/* Light vertical hinting: a piecewise-linear map in font units whose knots at
 * the baseline, x-height and cap height land on whole pixels. */
typedef struct {
    double scale, fu[3], px[3];
    int knots;
} Hint;
static Hint hint_init(const TextFont *f, double size) {
    Hint h = {size / f->units, {0}, {0}, 1};
    if (f->x_height > 0) {
        h.fu[h.knots] = f->x_height;
        h.px[h.knots++] = fmax(1, round(f->x_height * h.scale));
    }
    double cap = round(f->cap_height * h.scale);
    if (f->cap_height > h.fu[h.knots - 1] && cap > h.px[h.knots - 1]) {
        h.fu[h.knots] = f->cap_height;
        h.px[h.knots++] = cap;
    }
    return h;
}
static double hint_y(const Hint *h, double y) {
    if (y <= 0)
        return y * h->scale;
    for (int i = 1; i < h->knots; i++)
        if (y <= h->fu[i])
            return h->px[i - 1] +
                   (y - h->fu[i - 1]) * (h->px[i] - h->px[i - 1]) / (h->fu[i] - h->fu[i - 1]);
    return h->px[h->knots - 1] + (y - h->fu[h->knots - 1]) * h->scale;
}
TextMetrics text_metrics(const TextFont *f, double size) {
    Hint h = hint_init(f, size);
    double ascent = round(f->ascender * h.scale), descent = round(-f->descender * h.scale);
    double line = round((f->ascender - f->descender + f->line_gap) * h.scale);
    /* Typical proportions when a font provides neither metrics nor H/x glyphs. */
    double cap = f->cap_height > 0 ? round(hint_y(&h, f->cap_height)) : round(size * .7);
    double x = f->x_height > 0 ? round(hint_y(&h, f->x_height)) : round(size * .5);
    return (TextMetrics){ascent, descent, fmax(1, line), cap, x};
}

typedef struct {
    const TextFont *font;
    const Hint *hint;
    Path *path;
    double x, baseline;
    int *parts; /* Remaining outline budget for the current glyph. */
} Emit;
static Point emit_point(const Emit *e, Matrix m, double x, double y) {
    Point p = matrix_point(m, (Point){x, y});
    return (Point){e->x + p.x * e->hint->scale, e->baseline - hint_y(e->hint, p.y)};
}
static bool emit_quad(const Emit *e, Point control, Point end) {
    Curve c = quadratic(e->path->current, control, end);
    return path_curve(e->path, c.c1, c.c2, c.end);
}
/* `at` and `end` are absolute offsets of one glyph's data. */
static bool simple_glyph(const Emit *e, uint32_t at, uint32_t end, int contours, Matrix m) {
    const TextFont *f = e->font;
    if (!contours)
        return true;
    uint32_t p = at + 10 + 2 * (uint32_t)contours;
    if (p + 2 > end)
        return false;
    int points = u16(f, p - 2) + 1;
    p += 2 + u16(f, p); /* Skip hinting instructions. */
    uint8_t *flag = malloc((size_t)points);
    Point *pt = malloc((size_t)points * sizeof(*pt));
    bool ok = flag && pt;
    /* Flags, then delta-encoded x and y coordinates. */
    for (int i = 0; ok && i < points;) {
        if (p >= end)
            ok = false;
        else {
            uint8_t fl = f->data[p++];
            int repeat = 1;
            if (fl & 8)
                repeat += p < end ? f->data[p++] : 0;
            while (repeat-- && i < points)
                flag[i++] = fl;
        }
    }
    for (int axis = 0; ok && axis < 2; axis++) {
        int value = 0, shortbit = axis ? 4 : 2, samebit = axis ? 32 : 16;
        for (int i = 0; ok && i < points; i++) {
            if (flag[i] & shortbit) {
                if ((ok = p + 1 <= end)) {
                    int d = f->data[p++];
                    value += flag[i] & samebit ? d : -d;
                }
            } else if (!(flag[i] & samebit)) {
                if ((ok = p + 2 <= end)) {
                    value += s16(f, p);
                    p += 2;
                }
            }
            if (axis)
                pt[i].y = value;
            else
                pt[i].x = value;
        }
    }
    int first = 0;
    for (int c = 0; ok && c < contours; c++) {
        int last = u16(f, at + 10 + 2 * (uint32_t)c);
        if (last < first || last >= points) {
            ok = false;
            break;
        }
        int n = last - first, from = 0, to = n; /* Process points [from, to]. */
        const Point *q = pt + first;
        const uint8_t *on = flag + first;
        /* Start on an on-curve point, or between two off-curve points. */
        Point start;
        if (on[0] & 1) {
            start = q[0];
            from = 1;
        } else if (on[n] & 1) {
            start = q[n];
            to = n - 1;
        } else
            start = midpoint(q[0], q[n]);
        ok = path_move(e->path, emit_point(e, m, start.x, start.y));
        bool pending = false;
        Point control = {0};
        for (int k = from; ok && k <= to; k++) {
            if (on[k] & 1) {
                Point target = emit_point(e, m, q[k].x, q[k].y);
                ok = pending ? emit_quad(e, emit_point(e, m, control.x, control.y), target)
                             : path_line(e->path, target);
                pending = false;
            } else {
                if (pending) {
                    Point mid = midpoint(control, q[k]);
                    ok = emit_quad(e, emit_point(e, m, control.x, control.y),
                                   emit_point(e, m, mid.x, mid.y));
                }
                control = q[k];
                pending = true;
            }
        }
        if (ok)
            ok = pending ? emit_quad(e, emit_point(e, m, control.x, control.y),
                                     emit_point(e, m, start.x, start.y))
                         : path_close(e->path);
        first = last + 1;
    }
    free(flag);
    free(pt);
    return ok;
}
static bool glyph_outline(const Emit *e, int glyph, Matrix m, int depth) {
    const TextFont *f = e->font;
    uint32_t start, end;
    if (--*e->parts < 0) /* Bounds composite fan-out, not just depth. */
        return false;
    if (!glyph_range(f, glyph, &start, &end))
        return true; /* Empty glyph, such as a space. */
    int contours = s16(f, f->glyf + start);
    if (contours >= 0)
        return simple_glyph(e, f->glyf + start, f->glyf + end, contours, m);
    if (depth >= MAX_COMPOSITE_DEPTH)
        return false;
    uint32_t p = f->glyf + start + 10, limit = f->glyf + end;
    for (;;) {
        if (p + 4 > limit)
            return false;
        uint16_t flags = u16(f, p), component = u16(f, p + 2);
        p += 4;
        double dx = 0, dy = 0;
        if (flags & 1) {
            if (flags & 2) {
                dx = s16(f, p);
                dy = s16(f, p + 2);
            }
            p += 4;
        } else {
            if (p + 2 > limit)
                return false;
            if (flags & 2) {
                dx = (int8_t)f->data[p];
                dy = (int8_t)f->data[p + 1];
            }
            p += 2;
        }
        double a = 1, b = 0, c = 0, d = 1;
        if (flags & 8) {
            a = d = s16(f, p) / 16384.0;
            p += 2;
        } else if (flags & 0x40) {
            a = s16(f, p) / 16384.0;
            d = s16(f, p + 2) / 16384.0;
            p += 4;
        } else if (flags & 0x80) {
            a = s16(f, p) / 16384.0;
            b = s16(f, p + 2) / 16384.0;
            c = s16(f, p + 4) / 16384.0;
            d = s16(f, p + 6) / 16384.0;
            p += 8;
        }
        if (p > limit)
            return false;
        /* Point-matched (non-XY) offsets are rare and treated as zero. */
        if (!glyph_outline(e, component, matrix_mul(m, (Matrix){a, b, c, d, dx, dy}), depth + 1))
            return false;
        if (!(flags & 0x20))
            return true;
    }
}
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2
                : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (extra < 0) {
        *s = (const char *)p;
        return 0xfffd;
    }
    c &= extra ? 0x3f >> extra : 0x7f;
    for (int i = 0; i < extra; i++, p++) {
        if ((*p & 0xc0) != 0x80) {
            *s = (const char *)p;
            return 0xfffd;
        }
        c = c << 6 | (*p & 0x3f);
    }
    *s = (const char *)p;
    return c > 0x10ffff || (c >= 0xd800 && c < 0xe000) ? 0xfffd : c;
}
/* Appends a finished glyph outline; fails only when `out` cannot grow. */
static bool append_path(Path *out, const Path *glyph) {
    for (size_t i = 0; i < glyph->count; i++) {
        Edge g = glyph->edges[i];
        if ((g.begin && !path_move(out, g.a)) || !path_line(out, g.b))
            return false;
    }
    return true;
}
bool text_path(const TextFont *f, const char *utf8, double size, Point origin, Path *out,
               Point *caret) {
    if (!f || !utf8 || !out || !isfinite(size) || size <= 0 || size > 4096)
        return false;
    Hint hint = hint_init(f, size);
    TextMetrics metrics = text_metrics(f, size);
    double left = round(origin.x);
    Path glyph_path = {0};
    int parts = 0;
    Emit e = {f, &hint, &glyph_path, left, round(origin.y), &parts};
    int previous = -1;
    bool ok = true;
    while (ok && *utf8) {
        uint32_t code = utf8_next(&utf8);
        if (code == '\n') {
            e.x = left;
            e.baseline += metrics.line_height;
            previous = -1;
            continue;
        }
        if (code < 0x20 || code == 0x7f)
            continue;
        int glyph = glyph_index(f, code);
        if (previous >= 0)
            e.x += kerning(f, previous, glyph) * hint.scale;
        /* Each glyph is outlined separately: a malformed glyph is skipped
         * rather than failing the whole text or leaving a partial contour. */
        glyph_path.count = 0;
        glyph_path.started = false;
        parts = MAX_GLYPH_PARTS;
        if (glyph_outline(&e, glyph, (Matrix){1, 0, 0, 1, 0, 0}, 0))
            ok = append_path(out, &glyph_path);
        e.x += advance(f, glyph) * hint.scale;
        previous = glyph;
    }
    path_free(&glyph_path);
    if (caret)
        *caret = (Point){e.x, e.baseline};
    return ok;
}
