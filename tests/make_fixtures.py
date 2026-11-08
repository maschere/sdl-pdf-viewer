#!/usr/bin/env python3
"""Rebuild tiny classic-xref regression PDFs using only Python's standard library."""
from pathlib import Path
import base64
import zlib

ROOT = Path(__file__).parent / 'fixtures'
ROOT.mkdir(exist_ok=True)

def stream(data, extra=b'', compress=True):
    if compress:
        data = zlib.compress(data)
        extra += b' /Filter /FlateDecode'
    return b'<< /Length %d %s >>\nstream\n' % (len(data), extra) + data + b'\nendstream'

def write(name, objects, extra=b''):
    data = bytearray(b'%PDF-1.4\n%\xe2\xe3\xcf\xd3\n')
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(b'%d 0 obj\n' % i + obj + b'\nendobj\n')
    xref = len(data)
    data.extend(b'xref\n0 %d\n0000000000 65535 f \n' % len(offsets))
    for offset in offsets[1:]:
        data.extend(b'%010d 00000 n \n' % offset)
    data.extend(b'trailer\n<< /Size %d /Root 1 0 R %s >>\nstartxref\n%d\n%%%%EOF\n' % (len(offsets), extra, xref))
    (ROOT / name).write_bytes(data)

# Glyphs are authored as tiny outline rectangles, keeping the demo font-free.
FONT = {
'A':['01110','10001','10001','11111','10001','10001','10001'],
'B':['11110','10001','10001','11110','10001','10001','11110'],
'C':['01111','10000','10000','10000','10000','10000','01111'],
'D':['11110','10001','10001','10001','10001','10001','11110'],
'E':['11111','10000','10000','11110','10000','10000','11111'],
'G':['01111','10000','10000','10111','10001','10001','01110'],
'I':['11111','00100','00100','00100','00100','00100','11111'],
'K':['10001','10010','10100','11000','10100','10010','10001'],
'L':['10000','10000','10000','10000','10000','10000','11111'],
'M':['10001','11011','10101','10101','10001','10001','10001'],
'N':['10001','11001','11001','10101','10011','10011','10001'],
'O':['01110','10001','10001','10001','10001','10001','01110'],
'P':['11110','10001','10001','11110','10000','10000','10000'],
'R':['11110','10001','10001','11110','10100','10010','10001'],
'S':['01111','10000','10000','01110','00001','00001','11110'],
'T':['11111','00100','00100','00100','00100','00100','00100'],
'U':['10001','10001','10001','10001','10001','10001','01110'],
'V':['10001','10001','10001','10001','10001','01010','00100'],
'W':['10001','10001','10001','10101','10101','10101','01010'],
'X':['10001','10001','01010','00100','01010','10001','10001'],
'Y':['10001','10001','01010','00100','00100','00100','00100'],
' ':['00000']*7, '+':['00000','00100','00100','11111','00100','00100','00000'],
}

def text(label, x, y, scale=2):
    out = []
    for ch in label:
        for row, bits in enumerate(FONT[ch]):
            for col, bit in enumerate(bits):
                if bit == '1':
                    out.append(f'{x+col*scale} {y+(6-row)*scale} {scale} {scale} re')
        x += 6*scale
    return ('\n'.join(out)+'\nf\n').encode()

# A deterministic JPEG is embedded below; no imaging package is needed to regenerate.
JPEG_B64 = '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAIAAgDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDyOiiiv9UD/aw//9k='
jpeg = base64.b64decode(JPEG_B64)
content = (b'q\n0.07 0.11 0.2 rg 0 255 400 45 re f\n1 g\n' + text('VECTOR + IMAGES', 18, 270) +
    b'0.1 0.3 0.6 rg 285 190 100 50 re f\n1 g\n' + text('NEXT', 310, 210, 2) +
    b'0.1 0.45 0.35 rg 285 130 100 40 re f\n1 g\n' + text('WEB', 315, 145, 2) +
    b'q 120 0 0 120 20 40 cm /RGB Do Q\nq 120 0 0 120 155 40 cm /JPEG Do Q\n' +
    b'q 60 0 0 60 300 55 cm /Alpha Do Q\nq 40 0 0 20 20 10 cm /Indexed Do Q\nQ\n')
objects = [
    b'<< /Type /Catalog /Pages 2 0 R /Dests << /legacy [4 0 R /Fit] >> /Names << /Dests 15 0 R >> >>',
    b'<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /MediaBox [0 0 400 300] >>',
    b'<< /Type /Page /Parent 2 0 R /Contents [5 0 R 6 0 R] /Resources << /XObject << /RGB 7 0 R /JPEG 8 0 R /Alpha 9 0 R /Indexed 11 0 R >> >> /Annots [12 0 R 13 0 R 14 0 R 17 0 R] >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 420 340] /CropBox [10 20 410 320] /Rotate 90 /Contents 18 0 R /Annots [19 0 R] >>',
    stream(b'q\n'),stream(content+b'Q\n'),
    # Top row uses PNG Sub, bottom row PNG Up. Top red/green, bottom blue/yellow.
    stream(bytes([1,255,0,0,1,255,0,2,1,0,255,255,0,0]), b'/Type /XObject /Subtype /Image /Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace /DeviceRGB /DecodeParms << /Predictor 15 /Colors 3 /Columns 2 /BitsPerComponent 8 >>'),
    stream(jpeg,b'/Type /XObject /Subtype /Image /Width 8 /Height 8 /BitsPerComponent 8 /ColorSpace /DeviceRGB /Filter /DCTDecode',False),
    stream(bytes([0,0,255]),b'/Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceRGB /SMask 10 0 R'),
    stream(bytes([128]),b'/Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray'),
    stream(bytes([0b01000000]),b'/Type /XObject /Subtype /Image /Width 2 /Height 1 /BitsPerComponent 1 /ColorSpace [/Indexed /DeviceRGB 1 <ff000000ff00>]'),
    b'<< /Type /Annot /Subtype /Link /Rect [285 190 385 240] /Dest [4 0 R /Fit] >>',
    b'<< /Type /Annot /Subtype /Link /Rect [285 130 385 170] /A << /S /URI /URI (https://www.libsdl.org/) >> >>',
    b'<< /Type /Annot /Subtype /Link /Rect [0 0 10 10] /Dest /legacy >>',
    b'<< /Kids [16 0 R] >>',
    b'<< /Names [(chapter) << /D [4 0 R /XYZ null null null] >>] >>',
    b'<< /Type /Annot /Subtype /Link /Rect [10 0 20 10] /A << /S /GoTo /D (chapter) >> >>',
    stream(b'0.95 0.8 0.1 rg 10 20 400 300 re f\n0.1 0.2 0.5 rg 50 60 320 220 re f\n1 g\n'+text('BACK',100,150,5)),
    b'<< /Type /Annot /Subtype /Link /Rect [10 20 410 320] /Dest [3 0 R /Fit] >>',
]
write('demo.pdf',objects)

# Rectangles may name any two opposite corners. Zero-area links and links whose
# destination is missing must not hide the links beneath them.
write('reversed-rects.pdf',[
    b'<< /Type /Catalog /Pages 2 0 R >>',
    b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [100 100 0 0] >>',
    b'<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Annots [5 0 R 6 0 R 7 0 R] >>',
    stream(b'0 0 1 rg 10 10 80 80 re f'),
    b'<< /Type /Annot /Subtype /Link /Rect [60 60 60 60] /Dest [3 0 R /Fit] >>',
    b'<< /Type /Annot /Subtype /Link /Rect [40 40 20 20] /Dest /missing >>',
    b'<< /Type /Annot /Subtype /Link /Rect [90 90 10 10] /A << /S /URI /URI (https://example.org/) >> >>',
])

def simple(name, content, extras=b''):
    write(name,[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',b'<< /Type /Page /Parent 2 0 R /Contents 4 0 R '+extras+b' >>',stream(content)])

simple('vectors.pdf',b'1 0 0 rg 10 10 m 90 10 l 50 90 l f\n0 0 1 rg 20 20 60 60 re 30 30 40 40 re f*\n0 1 0 RG 3 w 1 J 1 j 5 90 m 25 50 75 50 95 90 c S\n')
simple('clipping.pdf',b'q 25 25 50 50 re W n 1 0 0 rg 0 0 100 100 re f Q 0 0 1 rg 0 0 10 10 re f')
simple('stroke-nonuniform.pdf',b'q 1 0 0 4 0 0 cm 2 w 10 20 m 90 20 l S Q q 4 0 0 1 0 0 cm 2 w 5 10 m 5 90 l S Q')
simple('font-missing.pdf',b'BT /F1 12 Tf (Hello) Tj ET')
simple('operator-unsupported.pdf',b'/GS1 gs')
simple('path-clipping.pdf',b'0 0 m 100 0 l 0 100 l h W n 1 0 0 rg 0 0 100 100 re f')
simple('malformed-operand.pdf',b'10 re')
simple('image-unsupported.pdf',b'/Missing Do')
simple('encrypted.pdf',b'')
# Same valid object structure, explicit encryption marker in trailer.
write('encrypted.pdf',[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [] /Count 0 >>'],b'/Encrypt << >>')
(ROOT/'broken.pdf').write_bytes(b'%PDF-1.4\ninvalid')
print(f'Wrote fixtures in {ROOT}')

# Minimal synthetic CFF fonts: authored here, with no third-party font assets.
def cff_index(items):
    if not items: return b'\0\0'
    payload = b''.join(items)
    offsets = [1]
    for item in items: offsets.append(offsets[-1]+len(item))
    offsize = max(1,(offsets[-1].bit_length()+7)//8)
    return len(items).to_bytes(2,'big')+bytes([offsize])+b''.join(x.to_bytes(offsize,'big') for x in offsets)+payload

def cff_int(n):
    if -107 <= n <= 107: return bytes([n+139])
    return b'\x1c'+int(n).to_bytes(2,'big',signed=True)

def cff_offset(n): return b'\x1d'+n.to_bytes(4,'big')

def make_cff(cid=False, glyph=None):
    # Glyph A: a filled triangular outline; fixed PDF widths control advances.
    glyph = glyph or cff_int(100)+cff_int(0)+b'\x15'+cff_int(300)+cff_int(700)+cff_int(300)+cff_int(-700)+b'\x05\x0e'
    chars = cff_index([b'\x0e',glyph])
    encoding = bytes([0,1,65])
    charset = b'\0'+(100 if cid else 34).to_bytes(2,'big')
    fds = cff_index([b''])
    fdselect = bytes([0,0,0])
    def top(enc,charset_at,chars_at,fds_at,select_at):
        if cid:
            return cff_int(0)+cff_int(0)+cff_int(0)+b'\x0c\x1e'+cff_offset(charset_at)+b'\x0f'+cff_offset(chars_at)+b'\x11'+cff_offset(fds_at)+b'\x0c\x24'+cff_offset(select_at)+b'\x0c\x25'
        return cff_offset(enc)+b'\x10'+cff_offset(charset_at)+b'\x0f'+cff_offset(chars_at)+b'\x11'
    prefix = b'\x01\x00\x04\x04'+cff_index([b'Demo'])
    top0 = cff_index([top(0,0,0,0,0)])
    start = len(prefix+top0+cff_index([])+cff_index([]))
    enc_at = start; charset_at = enc_at+len(encoding); chars_at=charset_at+len(charset);fds_at=chars_at+len(chars);select_at=fds_at+len(fds)
    return prefix+cff_index([top(enc_at,charset_at,chars_at,fds_at,select_at)])+cff_index([])+cff_index([])+encoding+charset+chars+fds+fdselect

for cid in [False,True]:
    objects=[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',b'<< /Type /Page /Parent 2 0 R /Resources << /Font << /F 5 0 R >> >> /Contents 4 0 R >>',
      stream(b'BT /F 50 Tf 10 10 Td [<0064> -100 <0064>] TJ ET' if cid else b'BT /F 50 Tf 10 10 Td [(A) -100 (A)] TJ ET')]
    if cid:
        objects += [b'<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /DescendantFonts [8 0 R] >>',b'<< /Type /FontDescriptor /FontFile3 7 0 R >>',stream(make_cff(True),b'/Subtype /CIDFontType0C'),b'<< /Type /Font /Subtype /CIDFontType0 /FontDescriptor 6 0 R /DW 600 /W [100 [800]] >>']
    else:
        objects += [b'<< /Type /Font /Subtype /Type1 /FontDescriptor 6 0 R /FirstChar 65 /LastChar 65 /Widths [800] >>',b'<< /Type /FontDescriptor /FontFile3 7 0 R >>',stream(make_cff(),b'/Subtype /Type1C')]
    write('cff-cid.pdf' if cid else 'cff-simple.pdf',objects)

def make_flex_glyph():
    # Five 200-unit squares, one edge of each drawn by a flex operator. A flex
    # that ends in the wrong place visibly skews the rest of its square.
    def nums(*values): return b''.join(cff_int(v) for v in values)
    flexes = [
        ((100,100), 34, (30,40,20,30,30,40,30), False),                       # hflex
        ((400,100), 35, (30,5,40,15,30,0,30,0,40,-15,30,-5,50), False),       # flex
        ((700,100), 36, (30,5,40,15,30,30,40,-10,30), False),                 # hflex1
        ((100,600), 37, (30,5,40,15,30,0,30,0,40,-10,30), False),             # flex1, horizontal
        ((400,600), 37, (5,30,15,40,0,30,0,30,-10,40,30), True),              # flex1, vertical
    ]
    out, at = b'', (0,0)
    for (x,y), op, args, vertical in flexes:
        out += nums(x-at[0], y-at[1])+b'\x15'+nums(*args)+b'\x0c'+bytes([op])
        out += nums(200,0,0,-200) if vertical else nums(0,200,-200,0)
        out += b'\x05'
        at = (x+200,y) if vertical else (x,y+200)
    return out+b'\x0e'

write('cff-flex.pdf',[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',b'<< /Type /Page /Parent 2 0 R /Resources << /Font << /F 5 0 R >> >> /Contents 4 0 R >>',
  stream(b'BT /F 100 Tf 0 0 Td (A) Tj ET'),b'<< /Type /Font /Subtype /Type1 /FontDescriptor 6 0 R /FirstChar 65 /LastChar 65 /Widths [1000] >>',b'<< /Type /FontDescriptor /FontFile3 7 0 R >>',stream(make_cff(glyph=make_flex_glyph()),b'/Subtype /Type1C')])

# Minimal synthetic Type 1 font (FontMatrix 1/2000), one feature per glyph.
T1_OPS = {'hstem':b'\x01','hlineto':b'\x06','vlineto':b'\x07','closepath':b'\x09','callsubr':b'\x0a','return':b'\x0b','hsbw':b'\x0d','endchar':b'\x0e','rmoveto':b'\x15','hmoveto':b'\x16','seac':b'\x0c\x06','div':b'\x0c\x0c','callothersubr':b'\x0c\x10','pop':b'\x0c\x11','setcurrentpoint':b'\x0c\x21'}

def t1_encrypt(data, key, pad):
    out, r = bytearray(), key
    for c in bytes(pad)+data:
        e = c^(r>>8)
        out.append(e)
        r = ((e+r)*52845+22719)&0xffff
    return bytes(out)

def t1_num(v):
    if -107 <= v <= 107: return bytes([v+139])
    if 108 <= v <= 1131: return bytes([247+((v-108)>>8),(v-108)&255])
    if -1131 <= v <= -108: return bytes([251+((-v-108)>>8),(-v-108)&255])
    return b'\xff'+v.to_bytes(4,'big',signed=True)

def t1_code(items, leniv):
    data = b''.join(T1_OPS[x] if isinstance(x,str) else t1_num(x) for x in items)
    return t1_encrypt(data,4330,leniv) if leniv >= 0 else data

def make_type1(leniv=4, hex_eexec=False):
    square = [0,200,'rmoveto',1600,2,'div','hlineto',800,'vlineto',-800,'hlineto','closepath','endchar']
    # A box whose bottom edge is a flex, preceded by a hint replacement.
    flex = [(400,0),(-300,30),(100,20),(200,0),(200,0),(100,-20),(100,-30)]
    flexy = [200,1000,'hsbw',4,1,3,'callothersubr','pop','callsubr',0,200,'rmoveto',1,'callsubr']
    for dx,dy in flex: flexy += [dx,dy,'rmoveto',2,'callsubr']
    flexy += [50,1000,200,0,'callsubr',800,'vlineto',-800,'hlineto','closepath','endchar']
    subrs = [[3,0,'callothersubr','pop','pop','setcurrentpoint','return'],[0,1,'callothersubr','return'],[0,2,'callothersubr','return'],['return'],[100,200,'hstem','return']]
    glyphs = {'.notdef':[0,500,'hsbw','endchar'],'A':[200,1000,'hsbw']+square,'acute':[100,400,'hsbw',0,'hmoveto',200,'hlineto',200,'vlineto',-200,'hlineto','closepath','endchar'],'Aacute':[200,1000,'hsbw',100,700,1200,65,194,'seac'],'flexy':flexy}
    private = b'dup /Private 8 dict dup begin\n/RD {string currentfile exch readstring pop} executeonly def\n/ND {noaccess def} executeonly def\n/NP {noaccess put} executeonly def\n/lenIV %d def\n/Subrs %d array\n' % (leniv,len(subrs))
    for i,subr in enumerate(subrs):
        code = t1_code(subr,leniv)
        private += b'dup %d %d RD ' % (i,len(code))+code+b' NP\n'
    private += b'ND\n2 index /CharStrings %d dict dup begin\n' % len(glyphs)
    for name,items in glyphs.items():
        code = t1_code(items,leniv)
        private += b'/%s %d RD ' % (name.encode(),len(code))+code+b' ND\n'
    private += b'end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n'
    clear = b'%!PS-AdobeFont-1.0: Fixture\n12 dict begin\n/FontName /Fixture def\n/Encoding 256 array\n0 1 255 {1 index exch /.notdef put} for\ndup 65 /A put\nreadonly def\n/PaintType 0 def\n/FontType 1 def\n/FontMatrix [0.0005 0 0 0.0005 0 0] readonly def\n/FontBBox {0 0 2000 2000} readonly def\ncurrentdict end\ncurrentfile eexec\n'
    cipher = t1_encrypt(private,55665,4)
    return clear+(cipher.hex().encode() if hex_eexec else cipher)+b'\n'+b'0'*64+b'\ncleartomark\n'

(ROOT/'type1-font.pfa').write_bytes(make_type1())
for name,leniv,hex_eexec in [('type1.pdf',4,False),('type1-hex.pdf',0,True)]:
    write(name,[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',b'<< /Type /Page /Parent 2 0 R /Resources << /Font << /F 5 0 R >> >> /Contents 4 0 R >>',
      stream(b'BT /F 50 Tf 0 0 Td (A) Tj 50 0 Td (B) Tj -50 50 Td (C) Tj ET'),b'<< /Type /Font /Subtype /Type1 /FontDescriptor 6 0 R /FirstChar 65 /LastChar 67 /Widths [0 0 0] /Encoding 8 0 R >>',b'<< /Type /FontDescriptor /FontName /Fixture /FontFile 7 0 R >>',stream(make_type1(leniv,hex_eexec)),b'<< /Type /Encoding /Differences [66 /Aacute /flexy] >>'])

# Reusable form, constant alpha, stitched gradient and a luminosity soft mask.
write('graphics.pdf',[
 b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',
 b'<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /XObject << /Form 5 0 R >> /ExtGState << /Half << /ca 0.5 >> /Soft << /SMask << /S /Luminosity /G 6 0 R >> >> >> /Shading << /Gradient 7 0 R >> >> >>',
 stream(b'q /Half gs 1 0 0 rg 0 0 50 50 re f Q /Form Do q /Soft gs 0 0 1 rg 50 0 50 50 re f Q q 0 50 100 50 re W n /Gradient sh Q'),
 stream(b'0 1 0 rg 0 0 20 20 re f',b'/Type /XObject /Subtype /Form /BBox [0 0 20 20] /Matrix [1 0 0 1 25 25]'),
 stream(b'0.5 g 0 0 100 100 re f',b'/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Group << /S /Transparency /CS /DeviceGray >>'),
 b'<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Function << /FunctionType 3 /Domain [0 1] /Functions [<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 1 0] /N 1 >> << /FunctionType 2 /Domain [0 1] /C0 [0 1 0] /C1 [0 0 1] /N 1 >>] /Bounds [0.5] /Encode [0 1 0 1] >> >>'
])

# A genuinely compressed page tree and PDF xref stream.
parts=[b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',b'<< /Type /Page /Parent 2 0 R /Contents 4 0 R >>']
header=b'';body=b''
for i,part in enumerate(parts,1): header+=b'%d %d ' % (i,len(body));body+=part+b' '
objstm=stream(header+body,b'/Type /ObjStm /N 3 /First %d' % len(header))
data=bytearray(b'%PDF-1.5\n');off4=len(data);data+=b'4 0 obj\n'+stream(b'0 0 1 rg 0 0 100 100 re f')+b'\nendobj\n';off5=len(data);data+=b'5 0 obj\n'+objstm+b'\nendobj\n';off6=len(data)
entries=[(0,0,65535),(2,5,0),(2,5,1),(2,5,2),(1,off4,0),(1,off5,0),(1,off6,0)]
xdata=b''.join(bytes([t])+a.to_bytes(4,'big')+b.to_bytes(2,'big') for t,a,b in entries)
data+=b'6 0 obj\n'+stream(xdata,b'/Type /XRef /Size 7 /Root 1 0 R /W [1 4 2]')+b'\nendobj\nstartxref\n%d\n%%%%EOF\n' % off6
(ROOT/'compressed.pdf').write_bytes(data)

write('radial.pdf',[
 b'<< /Type /Catalog /Pages 2 0 R >>',b'<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 100 100] >>',
 b'<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /ExtGState << /Mask << /SMask << /S /Luminosity /G 5 0 R >> >> >> >> >>',
 stream(b'/Mask gs 0 0 1 rg 0 0 100 100 re f'),
 stream(b'/S sh',b'/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Resources << /Shading << /S 6 0 R >> >>'),
 b'<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [50 50 0 50 50 40] /Extend [true false] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 1 1] /C1 [0 0 0] /N 1 >> >>'
])

(ROOT/'too-short.pdf').write_bytes(b'%PDF-1.4')
