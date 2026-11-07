#ifndef CFF_H
#define CFF_H
#include "raster.h"
typedef struct Cff Cff;
Cff *cff_open(const unsigned char *data, size_t size, char *error, size_t capacity);
void cff_close(Cff *font);
/* identity=true maps a CID through the charset; false uses CFF's built-in encoding. */
bool cff_outline(Cff *font, unsigned code, bool identity, Matrix transform, Path *out, char *error,
                 size_t capacity);
#endif
