#!/usr/bin/env python3
"""Small deterministic GIF/APNG and PDF fixtures, using Python's standard library."""
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).parent / 'fixtures'

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def rgba_pixels(w, h, color):
    return bytes(color) * (w * h)

def apng(poster=False):
    signature = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', 4, 4, 8, 6, 0, 0, 0)
    out = signature + chunk(b'IHDR', ihdr) + chunk(b'acTL', struct.pack('>II', 5, 0))
    def image(w, h, pixels):
        return zlib.compress(b''.join(b'\0' + pixels[y*w*4:(y+1)*w*4] for y in range(h)))
    if poster:
        out += chunk(b'IDAT', image(4, 4, rgba_pixels(4, 4, (255, 255, 0, 255))))
    first = bytearray(4*4*4)
    for y in range(2):
        for x in range(2): first[(y*4+x)*4:(y*4+x+1)*4] = bytes((255, 0, 0, 255))
    frames = [(4,4,0,0,0,0,first),
              (2,2,2,0,1,0,rgba_pixels(2,2,(0,255,0,255))),
              (2,2,0,2,2,0,rgba_pixels(2,2,(0,0,255,255))),
              (1,1,0,0,0,1,bytes((0,255,0,128))),
              (1,1,1,1,0,0,bytes((0,0,0,0)))]
    seq = 0
    for i,(w,h,x,y,dispose,blend,pixels) in enumerate(frames):
        out += chunk(b'fcTL', struct.pack('>IIIIIHHBB',seq,w,h,x,y,1,10,dispose,blend))
        seq += 1
        compressed = image(w,h,pixels)
        if i == 0 and not poster: out += chunk(b'IDAT', compressed)
        else:
            out += chunk(b'fdAT', struct.pack('>I',seq)+compressed)
            seq += 1
    return out + chunk(b'IEND', b'')

def gif():
    out = bytearray(b'GIF89a'+struct.pack('<HHBBB',4,4,0x81,0,0))
    out += bytes((0,0,0,255,0,0,0,255,0,0,0,255))
    frames = [(0,0,2,2,1,1),(2,0,2,2,2,2),(0,2,2,2,3,3),(3,3,1,1,1,2)]
    for x,y,w,h,dispose,color in frames:
        out += b'!\xf9\x04' + bytes(((dispose<<2)|1,10,0,0,0))
        out += b',' + struct.pack('<HHHHB',x,y,w,h,0)
        # Clear the LZW dictionary before each pixel, keeping all codes 3 bits.
        codes = [v for _ in range(w*h) for v in (4,color)] + [5]
        bits = sum(code << (3*i) for i,code in enumerate(codes))
        payload = bits.to_bytes((len(codes)*3+7)//8,'little')
        out += bytes((2,len(payload))) + payload + b'\0'
    return bytes(out+b';')

def pdf(streams):
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Rotate 90 '
               b'/Resources << >> /Contents 4 0 R /Annots [5 0 R 8 0 R] >>',
               b'<< /Length 0 >>\nstream\n\nendstream']
    for i, (name, content, subject, rect) in enumerate(streams):
        base = 5 + 3*i
        # Exercise both ASCII and XeLaTeX's UTF-16BE metadata.
        encoded = subject.encode() if i == 0 else b'\xfe\xff' + subject.encode('utf-16-be')
        objects += [b'<< /Type /Annot /Subtype /FileAttachment /Rect ['+rect.encode()+
                    b'] /FS %d 0 R /Subj <%s> >>' % (base+1,encoded.hex().encode()),
                    b'<< /Type /Filespec /F ('+name.encode()+b') /EF << /F %d 0 R >> >>' % (base+2),
                    b'<< /Type /EmbeddedFile /Length %d >>\nstream\n' % len(content)+content+b'\nendstream']
    out=bytearray(b'%PDF-1.4\n'); offsets=[0]
    for i,obj in enumerate(objects,1):
        offsets.append(len(out)); out+=b'%d 0 obj\n'%i+obj+b'\nendobj\n'
    at=len(out);out+=b'xref\n0 %d\n0000000000 65535 f \n'%len(offsets)
    for offset in offsets[1:]:out+=b'%010d 00000 n \n'%offset
    out+=b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n'%(len(offsets),at)
    return out

if __name__ == '__main__':
    ROOT.mkdir(exist_ok=True)
    g,a=gif(),apng()
    (ROOT/'animation-disposal.gif').write_bytes(g)
    (ROOT/'animation-disposal.apng').write_bytes(a)
    (ROOT/'animation-poster.apng').write_bytes(apng(poster=True))
    (ROOT/'animation-metadata.pdf').write_bytes(pdf([
        ('test.gif',g,'SDLAnim fps=12 autoplay=false interpolation=nearest','10 20 70 80'),
        ('test.apng',a,'SDLAnim fps=29.97 autoplay=true interpolation=bicubic','90 100 170 180')]))
