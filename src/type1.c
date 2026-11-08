#include "type1.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPERANDS 256
#define MAX_PS_OPERANDS 32
#define MAX_SUBRS 65536
#define MAX_DEPTH 10
#define MAX_INSTRUCTIONS 1000000u
#define EEXEC_KEY 55665
#define CHARSTRING_KEY 4330

typedef struct {
    const unsigned char *data;
    size_t size;
} Bytes;
typedef struct {
    Bytes name, code;
} Glyph;
struct Type1 {
    unsigned char *plain; /* The decrypted private portion; everything points into it. */
    Bytes *subrs;
    size_t subr_count;
    Glyph *glyphs;
    size_t glyph_count, glyph_capacity;
    int encoding[256];
    Matrix matrix;
};

/* The Adobe StandardEncoding, used by fonts that select it and always by seac. */
static const char *const standard_names[256] = {
    [32] = "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand",
    "quoteright", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period",
    "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "colon", "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E",
    "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore",
    "quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p",
    "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright",
    "asciitilde",
    [161] = "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section", "currency",
    "quotesingle", "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl",
    [177] = "endash", "dagger", "daggerdbl", "periodcentered",
    [182] = "paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright",
    "guillemotright", "ellipsis", "perthousand",
    [191] = "questiondown",
    [193] = "grave", "acute", "circumflex", "tilde", "macron", "breve", "dotaccent", "dieresis",
    [202] = "ring", "cedilla",
    [205] = "hungarumlaut", "ogonek", "caron", "emdash",
    [225] = "AE",
    [227] = "ordfeminine",
    [232] = "Lslash", "Oslash", "OE", "ordmasculine",
    [241] = "ae",
    [245] = "dotlessi",
    [248] = "lslash", "oslash", "oe", "germandbls",
};

int type1_glyph(const Type1 *f, const char *name, size_t length) {
    for (size_t i = 0; i < f->glyph_count; i++)
        if (f->glyphs[i].name.size == length && !memcmp(f->glyphs[i].name.data, name, length))
            return (int)i;
    return -1;
}
int type1_builtin_glyph(const Type1 *f, unsigned code) { return code < 256 ? f->encoding[code] : -1; }
int type1_standard_glyph(const Type1 *f, unsigned code) {
    const char *name = code < 256 ? standard_names[code] : NULL;
    return name ? type1_glyph(f, name, strlen(name)) : -1;
}
void type1_close(Type1 *f) {
    if (!f)
        return;
    free(f->plain);
    free(f->subrs);
    free(f->glyphs);
    free(f);
}

/* A PostScript tokenizer, sufficient for the fixed layout of Type 1 programs. */
typedef struct {
    const unsigned char *data;
    size_t size, at;
} Scan;
static bool is_space(unsigned c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
static bool is_delimiter(unsigned c) { return c && strchr("()<>[]{}/%", (int)c); }
static bool token(Scan *s, Bytes *t) {
    for (;;) {
        while (s->at < s->size && is_space(s->data[s->at]))
            s->at++;
        if (s->at >= s->size)
            return false;
        if (s->data[s->at] != '%')
            break;
        while (s->at < s->size && s->data[s->at] != '\n' && s->data[s->at] != '\r')
            s->at++;
    }
    size_t begin = s->at;
    unsigned c = s->data[s->at++];
    if (c == '(') {
        for (int depth = 1; depth && s->at < s->size;) {
            unsigned d = s->data[s->at++];
            if (d == '\\' && s->at < s->size)
                s->at++;
            else if (d == '(')
                depth++;
            else if (d == ')')
                depth--;
        }
    } else if (c == '/' || !is_delimiter(c))
        while (s->at < s->size && !is_space(s->data[s->at]) && !is_delimiter(s->data[s->at]))
            s->at++;
    t->data = s->data + begin;
    t->size = s->at - begin;
    return true;
}
static bool is_token(Bytes t, const char *text) {
    return t.size == strlen(text) && !memcmp(t.data, text, t.size);
}
static bool copy_token(Bytes t, char *out, size_t capacity) {
    if (!t.size || t.size >= capacity)
        return false;
    memcpy(out, t.data, t.size);
    out[t.size] = 0;
    return true;
}
/* Decimal or radix ("8#101") integers. */
static bool token_int(Bytes t, long *value) {
    char text[32], *end;
    if (!copy_token(t, text, sizeof(text)))
        return false;
    *value = strtol(text, &end, 10);
    if (*end == '#' && *value >= 2 && *value <= 36)
        *value = strtol(end + 1, &end, (int)*value);
    return !*end && end != text;
}
static bool token_number(Bytes t, double *value) {
    char text[64], *end;
    if (!copy_token(t, text, sizeof(text)))
        return false;
    *value = strtod(text, &end);
    return !*end && end != text && isfinite(*value);
}
static void decrypt(unsigned char *data, size_t size, unsigned key) {
    uint16_t r = (uint16_t)key;
    for (size_t i = 0; i < size; i++) {
        unsigned char cipher = data[i];
        data[i] = (unsigned char)(cipher ^ (r >> 8));
        r = (uint16_t)((cipher + r) * 52845u + 22719u);
    }
}
static int hex_value(unsigned c) {
    return c >= '0' && c <= '9'   ? (int)c - '0'
           : c >= 'a' && c <= 'f' ? (int)c - 'a' + 10
           : c >= 'A' && c <= 'F' ? (int)c - 'A' + 10
                                  : -1;
}

typedef struct {
    Type1 *font;
    Scan scan;
    long len_iv;
    Bytes names[256]; /* The built-in encoding, resolved once glyphs are known. */
    bool standard;
    jmp_buf jump;
    char error[160];
} Parser;
static void parse_error(Parser *p, const char *message) {
    snprintf(p->error, sizeof(p->error), "%s", message);
    longjmp(p->jump, 1);
}
static bool next_int(Parser *p, long *value) {
    Bytes t;
    return token(&p->scan, &t) && token_int(t, value);
}
/* Called with the scanner on the RD token's terminator: one separator byte,
 * then the charstring, decrypted in place. */
static Bytes charstring(Parser *p, long length) {
    Scan *s = &p->scan;
    if (length < 0 || s->at >= s->size || (size_t)length > s->size - s->at - 1)
        parse_error(p, "Truncated Type 1 charstring");
    s->at++;
    unsigned char *data = (unsigned char *)s->data + s->at;
    size_t size = (size_t)length;
    s->at += size;
    if (p->len_iv >= 0) {
        if ((size_t)p->len_iv > size)
            parse_error(p, "Type 1 charstring shorter than lenIV");
        decrypt(data, size, CHARSTRING_KEY);
        data += p->len_iv;
        size -= (size_t)p->len_iv;
    }
    return (Bytes){data, size};
}
static void parse_subrs(Parser *p) {
    Type1 *f = p->font;
    Bytes t;
    long count, index, length;
    if (!next_int(p, &count) || count < 0 || count > MAX_SUBRS)
        parse_error(p, "Invalid Type 1 Subrs array");
    free(f->subrs);
    f->subrs = count ? calloc((size_t)count, sizeof(*f->subrs)) : NULL;
    f->subr_count = 0;
    if (count && !f->subrs)
        parse_error(p, "Out of memory");
    f->subr_count = (size_t)count;
    if (!token(&p->scan, &t)) /* "array" */
        parse_error(p, "Truncated Type 1 Subrs array");
    for (;;) {
        size_t before = p->scan.at;
        if (!token(&p->scan, &t))
            return;
        if (!is_token(t, "dup")) {
            p->scan.at = before;
            return;
        }
        if (!next_int(p, &index) || !next_int(p, &length) || index < 0 || index >= count ||
            !token(&p->scan, &t))
            parse_error(p, "Invalid Type 1 subroutine");
        f->subrs[index] = charstring(p, length);
        /* The terminator is either one token (NP, |) or "noaccess put". */
        if (token(&p->scan, &t) && is_token(t, "noaccess"))
            token(&p->scan, &t);
    }
}
static void parse_charstrings(Parser *p) {
    Type1 *f = p->font;
    Bytes t;
    while (token(&p->scan, &t)) {
        if (is_token(t, "end"))
            return;
        if (t.data[0] != '/')
            continue;
        long length;
        Bytes name = {t.data + 1, t.size - 1};
        if (!next_int(p, &length) || !token(&p->scan, &t))
            parse_error(p, "Invalid Type 1 CharStrings entry");
        Bytes code = charstring(p, length);
        if (f->glyph_count == f->glyph_capacity) {
            size_t capacity = f->glyph_capacity ? f->glyph_capacity * 2 : 256;
            Glyph *glyphs = realloc(f->glyphs, capacity * sizeof(*glyphs));
            if (!glyphs)
                parse_error(p, "Out of memory");
            f->glyphs = glyphs;
            f->glyph_capacity = capacity;
        }
        f->glyphs[f->glyph_count++] = (Glyph){name, code};
    }
}
static void parse_encoding(Parser *p) {
    Bytes t;
    if (!token(&p->scan, &t))
        return;
    if (is_token(t, "StandardEncoding")) {
        p->standard = true;
        return;
    }
    while (token(&p->scan, &t) && !is_token(t, "def")) {
        long code;
        if (is_token(t, "dup") && next_int(p, &code) && token(&p->scan, &t) && code >= 0 &&
            code < 256 && t.data[0] == '/')
            p->names[code] = (Bytes){t.data + 1, t.size - 1};
    }
}
static void parse_matrix(Parser *p) {
    Bytes t;
    double m[6];
    if (!token(&p->scan, &t)) /* '[' or '{' */
        parse_error(p, "Truncated Type 1 FontMatrix");
    for (int i = 0; i < 6; i++)
        if (!token(&p->scan, &t) || !token_number(t, &m[i]))
            parse_error(p, "Invalid Type 1 FontMatrix");
    if (m[0] * m[3] - m[1] * m[2] == 0)
        parse_error(p, "Singular Type 1 FontMatrix");
    p->font->matrix = (Matrix){m[0], m[1], m[2], m[3], m[4], m[5]};
}
/* Decrypts everything after "eexec", which may be binary or hexadecimal. */
static void decrypt_private(Parser *p, const unsigned char *data, size_t size) {
    while (size && is_space(*data)) {
        data++;
        size--;
    }
    if (size < 4)
        parse_error(p, "Truncated Type 1 private section");
    bool hex = true;
    for (int i = 0; i < 4; i++)
        hex = hex && hex_value(data[i]) >= 0;
    unsigned char *plain = malloc(size);
    if (!plain)
        parse_error(p, "Out of memory");
    p->font->plain = plain;
    size_t n = size;
    if (hex) {
        n = 0;
        int high = -1;
        for (size_t i = 0; i < size; i++) {
            int v = hex_value(data[i]);
            if (v < 0) {
                if (is_space(data[i]))
                    continue;
                break;
            }
            if (high < 0)
                high = v;
            else {
                plain[n++] = (unsigned char)(high << 4 | v);
                high = -1;
            }
        }
        if (n < 4)
            parse_error(p, "Truncated Type 1 private section");
    } else
        memcpy(plain, data, size);
    decrypt(plain, n, EEXEC_KEY);
    p->scan = (Scan){plain + 4, n - 4, 0};
}
Type1 *type1_open(const unsigned char *data, size_t size, char *error, size_t capacity) {
    Type1 *f = calloc(1, sizeof(*f));
    Parser *p = calloc(1, sizeof(*p));
    if (!f || !p) {
        free(f);
        free(p);
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    if (setjmp(p->jump)) {
        snprintf(error, capacity, "%s", p->error);
        type1_close(f);
        free(p);
        return NULL;
    }
    p->font = f;
    p->len_iv = 4;
    f->matrix = (Matrix){.001, 0, 0, .001, 0, 0};
    p->scan = (Scan){data, size, 0};
    Bytes t;
    bool eexec = false;
    while (!eexec && token(&p->scan, &t)) {
        if (is_token(t, "eexec"))
            eexec = true;
        else if (is_token(t, "/FontMatrix"))
            parse_matrix(p);
        else if (is_token(t, "/Encoding"))
            parse_encoding(p);
    }
    if (!eexec)
        parse_error(p, "Type 1 font has no eexec section");
    decrypt_private(p, data + p->scan.at, size - p->scan.at);
    while (token(&p->scan, &t)) {
        if (is_token(t, "/lenIV")) {
            if (!next_int(p, &p->len_iv) || p->len_iv > 256)
                parse_error(p, "Invalid Type 1 lenIV");
        } else if (is_token(t, "/Subrs"))
            parse_subrs(p);
        else if (is_token(t, "/CharStrings")) {
            parse_charstrings(p);
            break;
        }
    }
    if (!f->glyph_count)
        parse_error(p, "Type 1 font has no glyphs");
    for (unsigned code = 0; code < 256; code++) {
        Bytes name = p->names[code];
        f->encoding[code] = p->standard ? type1_standard_glyph(f, code)
                            : name.data ? type1_glyph(f, (const char *)name.data, name.size)
                                        : -1;
    }
    free(p);
    return f;
}

typedef struct {
    Type1 *font;
    Path *out;
    Matrix matrix;
    double stack[MAX_OPERANDS], ps[MAX_PS_OPERANDS];
    unsigned n, ps_n, budget;
    Point p, origin, flex_points[7];
    double side_bearing;
    int flex_count;
    bool open, flex, done, seac;
    jmp_buf jump;
    char error[160];
} Machine;
static void bad(Machine *m, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(m->error, sizeof(m->error), format, args);
    va_end(args);
    longjmp(m->jump, 1);
}
static Point output(Machine *m, Point p) {
    if (!isfinite(p.x) || !isfinite(p.y))
        bad(m, "Type 1 coordinate overflow");
    return matrix_point(m->matrix, p);
}
static void check(Machine *m, bool ok) {
    if (!ok)
        bad(m, "Cannot rasterize Type 1 outline");
}
static void close_contour(Machine *m) {
    if (m->open)
        check(m, path_close(m->out));
    m->open = false;
}
static void begin_contour(Machine *m) {
    close_contour(m);
    check(m, path_move(m->out, output(m, m->p)));
    m->open = true;
}
/* During a flex, movetos only collect the curve's reference points. */
static void move_by(Machine *m, double x, double y) {
    m->p.x += x;
    m->p.y += y;
    if (!m->flex)
        begin_contour(m);
}
static void line_by(Machine *m, double x, double y) {
    if (!m->open)
        begin_contour(m);
    m->p.x += x;
    m->p.y += y;
    check(m, path_line(m->out, output(m, m->p)));
}
static void curve_by(Machine *m, double x1, double y1, double x2, double y2, double x3, double y3) {
    if (!m->open)
        begin_contour(m);
    Point a = {m->p.x + x1, m->p.y + y1}, b = {a.x + x2, a.y + y2}, c = {b.x + x3, b.y + y3};
    m->p = c;
    check(m, path_curve(m->out, output(m, a), output(m, b), output(m, c)));
}
static int small_int(Machine *m, double value) {
    if (!(value > -1e7 && value < 1e7))
        bad(m, "Invalid Type 1 integer operand");
    return (int)value;
}
static void other_subr(Machine *m) {
    if (m->n < 2)
        bad(m, "Too few callothersubr operands");
    int other = small_int(m, m->stack[--m->n]), count = small_int(m, m->stack[--m->n]);
    if (count < 0 || (unsigned)count > m->n || count > MAX_PS_OPERANDS)
        bad(m, "Invalid callothersubr operand count");
    /* Moved to the PostScript stack so that the first argument is on top. */
    m->ps_n = 0;
    for (int i = 0; i < count; i++)
        m->ps[m->ps_n++] = m->stack[--m->n];
    if (other == 1) {
        m->flex = true;
        m->flex_count = 0;
    } else if (other == 2) {
        if (!m->flex || m->flex_count == 7)
            bad(m, "Invalid Type 1 flex point");
        m->flex_points[m->flex_count++] = m->p;
    } else if (other == 0) {
        if (!m->flex || m->flex_count != 7 || !m->open || m->ps_n != 3)
            bad(m, "Invalid Type 1 flex");
        m->flex = false;
        const Point *q = m->flex_points; /* q[0] is only the flex reference point. */
        check(m, path_curve(m->out, output(m, q[1]), output(m, q[2]), output(m, q[3])));
        check(m, path_curve(m->out, output(m, q[4]), output(m, q[5]), output(m, q[6])));
        m->ps_n--; /* Leaves x and y for "pop pop setcurrentpoint". */
    }
    /* Others, such as hint replacement (3), simply hand back their arguments. */
}
static void run(Machine *m, Bytes bytes, int depth);
static void run_part(Machine *m, int glyph, Point origin, int depth) {
    if (glyph < 0)
        bad(m, "Type 1 seac component is missing");
    close_contour(m);
    m->n = 0;
    m->done = false;
    m->origin = origin;
    run(m, m->font->glyphs[glyph].code, depth);
}
static void seac(Machine *m, const double *a, int depth) {
    if (m->seac)
        bad(m, "Nested Type 1 seac");
    m->seac = true;
    Type1 *f = m->font;
    int base = type1_standard_glyph(f, (unsigned)small_int(m, a[3]));
    int accent = type1_standard_glyph(f, (unsigned)small_int(m, a[4]));
    Point origin = m->origin;
    /* The accent is placed relative to the composite's own side bearing. */
    Point accent_origin = {origin.x + m->side_bearing - a[0] + a[1], origin.y + a[2]};
    run_part(m, base, origin, depth + 1);
    run_part(m, accent, accent_origin, depth + 1);
    close_contour(m);
    m->done = true;
}
static void run(Machine *m, Bytes bytes, int depth) {
    if (depth > MAX_DEPTH)
        bad(m, "Type 1 subroutine depth limit");
    size_t at = 0;
    double *s = m->stack;
    while (at < bytes.size && !m->done) {
        if (!m->budget--)
            bad(m, "Type 1 instruction limit");
        unsigned c = bytes.data[at++];
        if (c >= 32) {
            unsigned extra = c <= 246 ? 0 : c <= 254 ? 1 : 4;
            if (extra > bytes.size - at)
                bad(m, "Truncated Type 1 number");
            double value;
            const unsigned char *b = bytes.data + at;
            if (c <= 246)
                value = (int)c - 139;
            else if (c <= 250)
                value = ((int)c - 247) * 256 + b[0] + 108;
            else if (c <= 254)
                value = -((int)c - 251) * 256 - b[0] - 108;
            else
                value = (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                                  (uint32_t)b[2] << 8 | b[3]);
            at += extra;
            if (m->n == MAX_OPERANDS)
                bad(m, "Type 1 operand stack overflow");
            s[m->n++] = value;
            continue;
        }
        if (c == 12) {
            if (at >= bytes.size)
                bad(m, "Truncated Type 1 escape operator");
            c = 256u + bytes.data[at++];
        }
        /* Operands required by each operator; 0 for those taking none or a variable count. */
        unsigned need = c == 4 || c == 6 || c == 7 || c == 22 || c == 10 ? 1
                        : c == 5 || c == 13 || c == 21 || c == 256 + 12  ? 2
                        : c == 30 || c == 31 || c == 256 + 7             ? 4
                        : c == 256 + 6                                   ? 5
                        : c == 8                                         ? 6
                                                                         : 0;
        if (m->n < need)
            bad(m, "Too few Type 1 operands");
        const double *a = s + m->n - need;
        switch (c) {
        case 10: {
            int id = small_int(m, s[--m->n]);
            if (id < 0 || (size_t)id >= m->font->subr_count || !m->font->subrs[id].data)
                bad(m, "Invalid Type 1 subroutine");
            run(m, m->font->subrs[id], depth + 1);
            continue;
        }
        case 11:
            return;
        case 256 + 12:
            if (a[1] == 0)
                bad(m, "Type 1 division by zero");
            s[m->n - 2] = a[0] / a[1];
            m->n--;
            continue;
        case 256 + 16:
            other_subr(m);
            continue;
        case 256 + 17:
            if (!m->ps_n)
                bad(m, "Type 1 pop without a result");
            if (m->n == MAX_OPERANDS)
                bad(m, "Type 1 operand stack overflow");
            s[m->n++] = m->ps[--m->ps_n];
            continue;
        case 1:
        case 3:
        case 256 + 0:
        case 256 + 1:
        case 256 + 2:
        case 256 + 33: /* Only follows a flex, whose end point is already current. */
            break;
        case 4:
            move_by(m, 0, a[0]);
            break;
        case 5:
            line_by(m, a[0], a[1]);
            break;
        case 6:
            line_by(m, a[0], 0);
            break;
        case 7:
            line_by(m, 0, a[0]);
            break;
        case 8:
            curve_by(m, a[0], a[1], a[2], a[3], a[4], a[5]);
            break;
        case 9:
            close_contour(m);
            break;
        case 13:
            m->side_bearing = a[0];
            m->p = (Point){m->origin.x + a[0], m->origin.y};
            break;
        case 256 + 7:
            m->side_bearing = a[0];
            m->p = (Point){m->origin.x + a[0], m->origin.y + a[1]};
            break;
        case 14:
            close_contour(m);
            m->done = true;
            break;
        case 21:
            move_by(m, a[0], a[1]);
            break;
        case 22:
            move_by(m, a[0], 0);
            break;
        case 30:
            curve_by(m, 0, a[0], a[1], a[2], a[3], 0);
            break;
        case 31:
            curve_by(m, a[0], 0, a[1], a[2], 0, a[3]);
            break;
        case 256 + 6:
            seac(m, a, depth);
            break;
        default:
            if (c >= 256)
                bad(m, "Unsupported Type 1 charstring operator 12 %u", c - 256);
            bad(m, "Unsupported Type 1 charstring operator %u", c);
        }
        m->n = 0;
    }
}
bool type1_outline(Type1 *f, int glyph, Matrix transform, Path *out, char *error, size_t capacity) {
    if (glyph < 0 || (size_t)glyph >= f->glyph_count)
        return true;
    Machine *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(error, capacity, "Out of memory");
        return false;
    }
    bool ok = false;
    if (setjmp(m->jump))
        snprintf(error, capacity, "%s", m->error);
    else {
        m->font = f;
        m->out = out;
        m->matrix = matrix_mul(transform, f->matrix);
        m->budget = MAX_INSTRUCTIONS;
        run(m, f->glyphs[glyph].code, 0);
        close_contour(m);
        ok = true;
    }
    free(m);
    return ok;
}
