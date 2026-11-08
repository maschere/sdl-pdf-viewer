#ifndef TYPE1_H
#define TYPE1_H
#include "raster.h"

/* Embedded PostScript Type 1 font programs (PDF FontFile streams). */
typedef struct Type1 Type1;
Type1 *type1_open(const unsigned char *data, size_t size, char *error, size_t capacity);
void type1_close(Type1 *font);
/* Glyph lookups return a glyph index, or -1 when the font has no such glyph. */
int type1_glyph(const Type1 *font, const char *name, size_t length);
int type1_builtin_glyph(const Type1 *font, unsigned code);
int type1_standard_glyph(const Type1 *font, unsigned code);
/* Appends a glyph's outline; a glyph index of -1 draws nothing. */
bool type1_outline(Type1 *font, int glyph, Matrix transform, Path *out, char *error,
                   size_t capacity);
#endif
