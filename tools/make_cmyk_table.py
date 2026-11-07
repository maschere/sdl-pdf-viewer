#!/usr/bin/env python3
"""Offline color characterization for the default DeviceCMYK approximation.

Produces only sampled RGB color values, not PDF content or renderer code.
PyMuPDF is a development-time reference, never an application dependency.
"""
from pathlib import Path
import fitz

N = 9
samples = bytearray()
for c in range(N):
    for m in range(N):
        for y in range(N):
            for k in range(N):
                samples.extend(round(v*255/(N-1)) for v in (c,m,y,k))
source = fitz.Pixmap(fitz.csCMYK,N**4,1,bytes(samples),False)
result = fitz.Pixmap(fitz.csRGB,source).samples
lines = ['/* Default DeviceCMYK color samples. Regenerate with tools/make_cmyk_table.py. */',
         '#ifndef CMYK_TABLE_H', '#define CMYK_TABLE_H',
         'static const unsigned char cmyk_samples[19683] = {']
for start in range(0,len(result),27):
    lines.append('    '+','.join(str(v) for v in result[start:start+27])+',')
lines += ['};','#endif','']
(Path(__file__).parents[1]/'src/cmyk_table.h').write_text('\n'.join(lines))
