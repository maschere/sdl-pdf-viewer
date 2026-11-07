/* CFF 1 and Type 2 outlines, written from the Adobe CFF/Type 2 specifications.
 * Hint instructions are parsed but outlines are rendered without grid fitting. */
#include "cff.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
typedef struct {
    const unsigned char *data;
    size_t size;
} Bytes;
typedef struct {
    unsigned count, offsize;
    const unsigned char *offsets, *data;
    size_t length;
} Index;
typedef struct {
    int charset, encoding, charstrings, fdarray, fdselect, private_at, private_size, subrs;
    bool cid;
    Matrix matrix;
} Dict;
typedef struct {
    unsigned char op;
    Point a, b, c;
} Command;
typedef struct {
    Command *commands;
    size_t count, capacity;
    bool ready;
} Glyph;
struct Cff {
    Bytes bytes;
    Index strings, chars, global, *locals;
    unsigned fdcount;
    unsigned short *cidmap, encoding[256];
    unsigned char *fdselect;
    Glyph *glyphs;
    Matrix matrix;
    jmp_buf jump;
    char error[160];
    size_t total_commands;
};
static void bad(Cff *f, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(f->error, sizeof(f->error), fmt, a);
    va_end(a);
    longjmp(f->jump, 1);
}
static unsigned byte(Cff *f, size_t *at) {
    if (*at >= f->bytes.size)
        bad(f, "Truncated CFF");
    return f->bytes.data[(*at)++];
}
static unsigned uintn(Cff *f, size_t *at, unsigned n) {
    unsigned v = 0;
    if (n > 4)
        bad(f, "Invalid CFF integer");
    while (n--)
        v = (v << 8) | byte(f, at);
    return v;
}
static unsigned off(const unsigned char *p, unsigned n) {
    unsigned v = 0;
    while (n--)
        v = (v << 8) | *p++;
    return v;
}
static Index read_index(Cff *f, size_t *at) {
    Index i = {0};
    i.count = uintn(f, at, 2);
    if (!i.count)
        return i;
    i.offsize = byte(f, at);
    if (i.offsize < 1 || i.offsize > 4)
        bad(f, "Invalid CFF INDEX offset size");
    size_t table = (size_t)(i.count + 1) * i.offsize;
    if (table > f->bytes.size - *at)
        bad(f, "CFF INDEX outside font");
    i.offsets = f->bytes.data + *at;
    *at += table;
    unsigned first = off(i.offsets, i.offsize),
             last = off(i.offsets + i.count * i.offsize, i.offsize);
    if (first != 1 || last < 1 || last - 1 > f->bytes.size - *at)
        bad(f, "Invalid CFF INDEX offsets");
    i.data = f->bytes.data + *at;
    i.length = last - 1;
    *at += i.length;
    return i;
}
static Bytes index_item(Cff *f, Index i, unsigned n) {
    if (n >= i.count)
        bad(f, "CFF INDEX index out of range");
    unsigned a = off(i.offsets + n * i.offsize, i.offsize),
             b = off(i.offsets + (n + 1) * i.offsize, i.offsize);
    if (a < 1 || b < a || b - 1 > i.length)
        bad(f, "Invalid CFF INDEX item");
    return (Bytes){i.data + a - 1, b - a};
}
static double number(Cff *f, Bytes b, size_t *at, unsigned c, bool real) {
#define NEXT() ((*at < b.size) ? b.data[(*at)++] : (bad(f, "Truncated CFF number"), 0))
    if (c >= 32 && c <= 246)
        return (int)c - 139;
    if (c >= 247 && c <= 250) {
        unsigned v = NEXT();
        return (c - 247) * 256 + v + 108;
    }
    if (c >= 251 && c <= 254) {
        unsigned v = NEXT();
        return -(double)((c - 251) * 256 + v + 108);
    }
    if (c == 28) {
        unsigned a = NEXT(), d = NEXT();
        return (int16_t)((a << 8) | d);
    }
    if (c == 29 && real) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v = (v << 8) | NEXT();
        return (int32_t)v;
    }
    if (c == 255 && !real) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++)
            v = (v << 8) | NEXT();
        return (int32_t)v / 65536.0;
    }
    if (c == 30 && real) {
        char text[96];
        size_t n = 0;
        bool done = false;
        while (!done) {
            unsigned v = NEXT();
            for (int shift = 4; shift >= 0; shift -= 4) {
                unsigned h = (v >> shift) & 15;
                if (h == 15) {
                    done = true;
                    break;
                }
                const char *s = h <= 9    ? NULL
                                : h == 10 ? "."
                                : h == 11 ? "E"
                                : h == 12 ? "E-"
                                : h == 14 ? "-"
                                          : NULL;
                if (h <= 9) {
                    if (n + 1 >= sizeof(text))
                        bad(f, "CFF real too long");
                    text[n++] = (char)('0' + h);
                } else if (s) {
                    while (*s) {
                        if (n + 1 >= sizeof(text))
                            bad(f, "CFF real too long");
                        text[n++] = *s++;
                    }
                } else
                    bad(f, "Invalid CFF real");
            }
        }
        text[n] = 0;
        char *end;
        double x = strtod(text, &end);
        if (*end || !isfinite(x))
            bad(f, "Invalid CFF real");
        return x;
    }
    bad(f, "Invalid CFF number");
    return 0;
#undef NEXT
}
static int offset_arg(Cff *f, double n) {
    if (!isfinite(n) || n < 0 || n > f->bytes.size || n != floor(n))
        bad(f, "Invalid CFF offset");
    return (int)n;
}
static Dict dict(Cff *f, Bytes bytes, Matrix default_matrix) {
    Dict d = {0};
    d.matrix = default_matrix;
    double s[64];
    unsigned n = 0;
    size_t at = 0;
    while (at < bytes.size) {
        unsigned c = bytes.data[at++];
        if (c >= 32 || c == 28 || c == 29 || c == 30) {
            if (n == 64)
                bad(f, "CFF DICT stack limit");
            s[n++] = number(f, bytes, &at, c, true);
            continue;
        }
        unsigned op = c;
        if (c == 12) {
            if (at >= bytes.size)
                bad(f, "Truncated DICT operator");
            op = 1200 + bytes.data[at++];
        }
        if (op == 15 && n == 1)
            d.charset = offset_arg(f, s[0]);
        else if (op == 16 && n == 1)
            d.encoding = offset_arg(f, s[0]);
        else if (op == 17 && n == 1)
            d.charstrings = offset_arg(f, s[0]);
        else if (op == 18 && n == 2) {
            d.private_size = offset_arg(f, s[0]);
            d.private_at = offset_arg(f, s[1]);
        } else if (op == 19 && n == 1)
            d.subrs = offset_arg(f, s[0]);
        else if (op == 1230)
            d.cid = true;
        else if (op == 1236 && n == 1)
            d.fdarray = offset_arg(f, s[0]);
        else if (op == 1237 && n == 1)
            d.fdselect = offset_arg(f, s[0]);
        else if (op == 1207 && n == 6)
            d.matrix = (Matrix){s[0], s[1], s[2], s[3], s[4], s[5]};
        else if (op == 1206 && (n != 1 || s[0] != 2))
            bad(f, "Only Type 2 charstrings supported");
        n = 0;
    }
    if (n)
        bad(f, "Unterminated CFF DICT");
    return d;
}
static Index private_subrs(Cff *f, Dict d) {
    Index empty = {0};
    if (!d.private_size)
        return empty;
    if ((size_t)d.private_at + d.private_size > f->bytes.size)
        bad(f, "CFF Private outside font");
    Dict p = dict(f, (Bytes){f->bytes.data + d.private_at, (size_t)d.private_size},
                  (Matrix){1, 0, 0, 1, 0, 0});
    if (!p.subrs)
        return empty;
    size_t at = (size_t)d.private_at + p.subrs;
    return read_index(f, &at);
}
static void load(Cff *f) {
    size_t at = 0;
    if (byte(f, &at) != 1)
        bad(f, "Only CFF version 1 supported");
    (void)byte(f, &at);
    unsigned header = byte(f, &at);
    (void)byte(f, &at);
    if (header < 4 || header > f->bytes.size)
        bad(f, "Invalid CFF header");
    at = header;
    Index names = read_index(f, &at), tops = read_index(f, &at);
    if (names.count != 1 || tops.count != 1)
        bad(f, "Expected one embedded CFF font");
    f->strings = read_index(f, &at);
    f->global = read_index(f, &at);
    Dict top = dict(f, index_item(f, tops, 0), (Matrix){.001, 0, 0, .001, 0, 0});
    f->matrix = top.matrix;
    at = (size_t)top.charstrings;
    f->chars = read_index(f, &at);
    if (!f->chars.count)
        bad(f, "Empty CFF font");
    f->glyphs = calloc(f->chars.count, sizeof(*f->glyphs));
    f->cidmap = calloc(65536, sizeof(*f->cidmap));
    f->fdselect = calloc(f->chars.count, 1);
    if (!f->glyphs || !f->cidmap || !f->fdselect)
        bad(f, "Out of memory");
    unsigned short charset[65536] = {0};
    if (top.charset <= 2) {
        if (top.cid)
            bad(f, "CID CFF requires explicit charset");
        for (unsigned i = 1; i < f->chars.count; i++)
            charset[i] = (unsigned short)i;
    } else {
        at = (size_t)top.charset;
        unsigned fmt = byte(f, &at), gid = 1;
        if (fmt > 2)
            bad(f, "Invalid CFF charset");
        while (gid < f->chars.count) {
            unsigned sid = uintn(f, &at, 2), left = fmt ? uintn(f, &at, fmt == 1 ? 1 : 2) : 0;
            if (left >= f->chars.count - gid || sid + left > 65535)
                bad(f, "CFF charset range overflow");
            for (unsigned i = 0; i <= left; i++)
                charset[gid++] = (unsigned short)(sid + i);
        }
    }
    if (top.cid) {
        for (unsigned i = 0; i < f->chars.count; i++)
            f->cidmap[charset[i]] = (unsigned short)i;
    } else {
        if (top.encoding <= 1) {
            if (top.encoding == 1)
                bad(f, "Expert CFF encoding unsupported");
            for (unsigned code = 32; code <= 126; code++)
                for (unsigned gid = 1; gid < f->chars.count; gid++)
                    if (charset[gid] == code - 31)
                        f->encoding[code] = (unsigned short)gid;
        } else {
            at = (size_t)top.encoding;
            unsigned fmt = byte(f, &at), format = fmt & 127, count = byte(f, &at), gid = 1;
            if (format == 0) {
                for (unsigned i = 0; i < count; i++) {
                    unsigned code = byte(f, &at);
                    if (gid >= f->chars.count)
                        bad(f, "CFF encoding outside charset");
                    f->encoding[code] = (unsigned short)gid++;
                }
            } else if (format == 1) {
                for (unsigned i = 0; i < count; i++) {
                    unsigned first = byte(f, &at), left = byte(f, &at);
                    if (first + left > 255)
                        bad(f, "Invalid CFF encoding range");
                    for (unsigned j = 0; j <= left; j++) {
                        if (gid >= f->chars.count)
                            bad(f, "CFF encoding outside charset");
                        f->encoding[first + j] = (unsigned short)gid++;
                    }
                }
            } else
                bad(f, "Invalid CFF encoding");
            if (fmt & 128) {
                unsigned count2 = byte(f, &at);
                for (unsigned i = 0; i < count2; i++) {
                    unsigned code = byte(f, &at), sid = uintn(f, &at, 2);
                    for (unsigned j = 1; j < f->chars.count; j++)
                        if (charset[j] == sid)
                            f->encoding[code] = (unsigned short)j;
                }
            }
        }
    }
    f->fdcount = 1;
    if (top.cid) {
        at = (size_t)top.fdarray;
        Index fds = read_index(f, &at);
        if (!fds.count || fds.count > 256)
            bad(f, "Invalid CFF FDArray");
        f->fdcount = fds.count;
        f->locals = calloc(fds.count, sizeof(*f->locals));
        if (!f->locals)
            bad(f, "Out of memory");
        for (unsigned i = 0; i < fds.count; i++) {
            Dict fd = dict(f, index_item(f, fds, i), (Matrix){1, 0, 0, 1, 0, 0});
            f->locals[i] = private_subrs(f, fd);
        }
        at = (size_t)top.fdselect;
        unsigned fmt = byte(f, &at);
        if (fmt == 0) {
            for (unsigned i = 0; i < f->chars.count; i++)
                f->fdselect[i] = (unsigned char)byte(f, &at);
        } else if (fmt == 3) {
            unsigned ranges = uintn(f, &at, 2), first = uintn(f, &at, 2);
            if (first || !ranges)
                bad(f, "Invalid CFF FDSelect");
            for (unsigned i = 0; i < ranges; i++) {
                unsigned fd = byte(f, &at), end = uintn(f, &at, 2);
                if (end < first || end > f->chars.count)
                    bad(f, "Invalid FDSelect range");
                for (unsigned j = first; j < end; j++)
                    f->fdselect[j] = (unsigned char)fd;
                first = end;
            }
            if (first != f->chars.count)
                bad(f, "Incomplete FDSelect");
        } else
            bad(f, "Unsupported FDSelect format");
        for (unsigned i = 0; i < f->chars.count; i++)
            if (f->fdselect[i] >= f->fdcount)
                bad(f, "Invalid FDSelect index");
    } else {
        f->locals = calloc(1, sizeof(*f->locals));
        if (!f->locals)
            bad(f, "Out of memory");
        f->locals[0] = private_subrs(f, top);
    }
}
Cff *cff_open(const unsigned char *data, size_t size, char *error, size_t cap) {
    Cff *f = calloc(1, sizeof(*f));
    if (!f) {
        snprintf(error, cap, "Out of memory");
        return NULL;
    }
    f->bytes = (Bytes){data, size};
    if (setjmp(f->jump)) {
        snprintf(error, cap, "%s", f->error);
        cff_close(f);
        return NULL;
    }
    load(f);
    return f;
}
void cff_close(Cff *f) {
    if (!f)
        return;
    if (f->glyphs)
        for (unsigned i = 0; i < f->chars.count; i++)
            free(f->glyphs[i].commands);
    free(f->glyphs);
    free(f->locals);
    free(f->fdselect);
    free(f->cidmap);
    free(f);
}
typedef struct {
    Cff *f;
    Glyph *g;
    Index local;
    double stack[48];
    unsigned n, hints;
    bool width, open;
    Point p;
    unsigned budget;
} Machine;
static void command(Machine *m, unsigned char op, Point a, Point b, Point c) {
    Glyph *g = m->g;
    if (g->count == g->capacity) {
        size_t n = g->capacity ? g->capacity * 2 : 64;
        Command *p = realloc(g->commands, n * sizeof(*p));
        if (!p)
            bad(m->f, "Out of memory");
        g->commands = p;
        g->capacity = n;
    }
    if (++m->f->total_commands > 1000000)
        bad(m->f, "CFF outline command limit");
    g->commands[g->count++] = (Command){op, a, b, c};
}
static void close_contour(Machine *m) {
    if (m->open) {
        command(m, 3, (Point){0}, (Point){0}, (Point){0});
        m->open = false;
    }
}
static void move(Machine *m, double x, double y) {
    close_contour(m);
    m->p.x += x;
    m->p.y += y;
    command(m, 0, m->p, (Point){0}, (Point){0});
    m->open = true;
}
static void line(Machine *m, double x, double y) {
    if (!m->open)
        bad(m->f, "Line before CFF move");
    m->p.x += x;
    m->p.y += y;
    command(m, 1, m->p, (Point){0}, (Point){0});
}
static void bezier(Machine *m, double x1, double y1, double x2, double y2, double x3, double y3) {
    if (!m->open)
        bad(m->f, "Curve before CFF move");
    Point a = {m->p.x + x1, m->p.y + y1}, b = {a.x + x2, a.y + y2};
    m->p = (Point){b.x + x3, b.y + y3};
    command(m, 2, a, b, m->p);
}
static void remove_width(Machine *m, unsigned expected) {
    if (!m->width && m->n == expected + 1) {
        memmove(m->stack, m->stack + 1, --m->n * sizeof(double));
    }
    m->width = true;
}
static int bias(Index i) { return i.count < 1240 ? 107 : i.count < 33900 ? 1131 : 32768; }
static void run(Machine *m, Bytes bytes, int depth) {
    if (depth > 10)
        bad(m->f, "CFF subroutine depth limit");
    size_t at = 0;
    double *s = m->stack;
    while (at < bytes.size) {
        if (!m->budget--)
            bad(m->f, "CFF instruction limit");
        unsigned c = bytes.data[at++];
        if (c >= 32 || c == 28) {
            if (m->n == 48)
                bad(m->f, "CFF operand stack overflow");
            s[m->n++] = number(m->f, bytes, &at, c, false);
            continue;
        }
        unsigned n = m->n;
        if (c == 1 || c == 3 || c == 18 || c == 23 || c == 19 || c == 20) {
            if (!m->width && n % 2)
                remove_width(m, n - 1);
            m->width = true;
            if (m->n % 2)
                bad(m->f, "Invalid CFF hints");
            m->hints += m->n / 2;
            m->n = 0;
            if (c == 19 || c == 20) {
                size_t skip = (m->hints + 7) / 8;
                if (skip > bytes.size - at)
                    bad(m->f, "Truncated CFF hint mask");
                at += skip;
            }
            continue;
        }
        if (c == 10 || c == 29) {
            if (!n)
                bad(m->f, "Empty subroutine stack");
            Index index = c == 10 ? m->local : m->f->global;
            int id = (int)s[--m->n] + bias(index);
            if (id < 0)
                bad(m->f, "Invalid CFF subroutine");
            run(m, index_item(m->f, index, (unsigned)id), depth + 1);
            continue;
        }
        if (c == 11)
            return;
        if (c == 14) {
            if (n > 1)
                bad(m->f, "Composite endchar unsupported");
            close_contour(m);
            m->n = 0;
            return;
        }
        if (c == 21 || c == 22 || c == 4) {
            remove_width(m, c == 21 ? 2 : 1);
            n = m->n;
            if (n != (c == 21 ? 2u : 1u))
                bad(m->f, "Invalid CFF move");
            move(m, c == 4 ? 0 : s[0], c == 22 ? 0 : s[c == 21 ? 1 : 0]);
        } else if (c == 5) {
            if (n % 2)
                bad(m->f, "Invalid rlineto");
            for (unsigned i = 0; i < n; i += 2)
                line(m, s[i], s[i + 1]);
        } else if (c == 6 || c == 7) {
            bool horizontal = c == 6;
            for (unsigned i = 0; i < n; i++) {
                line(m, horizontal ? s[i] : 0, horizontal ? 0 : s[i]);
                horizontal = !horizontal;
            }
        } else if (c == 8) {
            if (n % 6)
                bad(m->f, "Invalid rrcurveto");
            for (unsigned i = 0; i < n; i += 6)
                bezier(m, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
        } else if (c == 24) {
            if (n < 8 || (n - 2) % 6)
                bad(m->f, "Invalid rcurveline");
            unsigned i = 0;
            for (; i + 2 < n; i += 6)
                bezier(m, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
            line(m, s[i], s[i + 1]);
        } else if (c == 25) {
            if (n < 8 || (n - 6) % 2)
                bad(m->f, "Invalid rlinecurve");
            unsigned i = 0;
            for (; i + 6 < n; i += 2)
                line(m, s[i], s[i + 1]);
            bezier(m, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]);
        } else if (c == 26 || c == 27) {
            if (n < 4 || (n % 4 != 0 && n % 4 != 1))
                bad(m->f, "Invalid hh/vvcurveto");
            unsigned i = n % 4;
            double first = i ? s[0] : 0;
            for (; i < n; i += 4) {
                if (c == 27)
                    bezier(m, s[i], first, s[i + 1], s[i + 2], s[i + 3], 0);
                else
                    bezier(m, first, s[i], s[i + 1], s[i + 2], 0, s[i + 3]);
                first = 0;
            }
        } else if (c == 30 || c == 31) {
            if (n < 4 || (n % 4 != 0 && n % 4 != 1))
                bad(m->f, "Invalid hv/vhcurveto");
            bool horizontal = c == 31;
            unsigned i = 0;
            while (i + 3 < n) {
                double extra = n - i == 5 ? s[i + 4] : 0;
                if (horizontal)
                    bezier(m, s[i], 0, s[i + 1], s[i + 2], extra, s[i + 3]);
                else
                    bezier(m, 0, s[i], s[i + 1], s[i + 2], s[i + 3], extra);
                i += n - i == 5 ? 5 : 4;
                horizontal = !horizontal;
            }
        } else
            bad(m->f, "Unsupported Type 2 charstring operator %u", c);
        m->n = 0;
    }
}
bool cff_outline(Cff *f, unsigned code, bool identity, Matrix transform, Path *out, char *error,
                 size_t cap) {
    if (setjmp(f->jump)) {
        snprintf(error, cap, "%s", f->error);
        return false;
    }
    unsigned gid =
        identity ? (code < 65536 ? f->cidmap[code] : 0) : (code < 256 ? f->encoding[code] : 0);
    if (code && !gid)
        bad(f, "CFF character has no matching glyph");
    Glyph *g = &f->glyphs[gid];
    if (!g->ready) {
        g->count = 0;
        Machine m = {0};
        m.f = f;
        m.g = g;
        m.local = f->locals[f->fdselect[gid]];
        m.budget = 100000;
        run(&m, index_item(f, f->chars, gid), 0);
        close_contour(&m);
        g->ready = true;
    }
    Matrix matrix = matrix_mul(transform, f->matrix);
    for (size_t i = 0; i < g->count; i++) {
        Command c = g->commands[i];
        bool ok = true;
        if (c.op == 0)
            ok = path_move(out, matrix_point(matrix, c.a));
        else if (c.op == 1)
            ok = path_line(out, matrix_point(matrix, c.a));
        else if (c.op == 2)
            ok = path_curve(out, matrix_point(matrix, c.a), matrix_point(matrix, c.b),
                            matrix_point(matrix, c.c));
        else
            ok = path_close(out);
        if (!ok)
            bad(f, "Cannot rasterize CFF outline");
    }
    return true;
}
