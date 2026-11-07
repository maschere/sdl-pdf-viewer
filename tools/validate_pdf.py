#!/usr/bin/env python3
"""Optional development acceptance check; never used by the application.
Requires PyMuPDF, pypdf, Pillow and NumPy, plus the C test executables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import fitz
import numpy as np
from PIL import Image, ImageFilter
from pypdf import PdfReader

parser = argparse.ArgumentParser()
parser.add_argument('pdf', type=Path)
parser.add_argument('--build', type=Path, default=Path('build'))
parser.add_argument('--output', type=Path, default=Path('validation'))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)

def executable(name):
    for base in [args.build,args.build/'Release',args.build/'Debug']:
        for suffix in ['', '.exe']:
            path = (base/(name+suffix)).resolve()
            if path.is_file(): return str(path)
    raise SystemExit(f'Build {name} first (BUILD_TESTING=ON).')

reader = PdfReader(args.pdf)
links_path = args.output/'links.tsv'
internal = external = 0
with links_path.open('w') as output:
    for page_number,page in enumerate(reader.pages):
        for value in page.get('/Annots',[]):
            annotation=value.get_object()
            if annotation.get('/Subtype')!='/Link': continue
            action=annotation.get('/A',{}).get_object() if annotation.get('/A') else {}
            rect=[float(v) for v in annotation['/Rect']]
            url=str(action.get('/URI',''))
            if url:
                if not url.lower().startswith(('http://','https://')): continue
                target=-1; external+=1
            else:
                destination=action.get('/D',annotation.get('/Dest'))
                if isinstance(destination,str):
                    destination=reader.named_destinations[str(destination)].dest_array
                target=reader.get_page_number(destination[0].get_object());internal+=1
            output.write(f'{page_number}\t{(rect[0]+rect[2])/2}\t{(rect[1]+rect[3])/2}\t{target}\t{url}\n')
subprocess.run([executable('check_links'),str(args.pdf),str(links_path)],check=True)
subprocess.run([executable('render_document'),str(args.pdf),str(args.output)],check=True)
reference=fitz.open(args.pdf)
rows=[]
for index,page in enumerate(reference):
    own=Image.open(args.output/f'page-{index+1:03}.ppm').convert('RGB')
    # Match the native acceptance helper's fitted 1600x1000 surface, then box-filter 2x.
    scale=min(1600/page.rect.width,1000/page.rect.height)
    pix=page.get_pixmap(matrix=fitz.Matrix(scale,scale),alpha=False)
    high=Image.new('RGB',(1600,1000),(16,16,16))
    raw=Image.frombytes('RGB',(pix.width,pix.height),pix.samples)
    high.paste(raw,((1600-pix.width)//2,(1000-pix.height)//2))
    ref=high.resize((800,500),Image.Resampling.BOX)
    own_array=np.asarray(own,dtype=np.float32);ref_array=np.asarray(ref,dtype=np.float32)
    own_blur=np.asarray(own.filter(ImageFilter.GaussianBlur(.6)),dtype=np.float32)
    ref_blur=np.asarray(ref.filter(ImageFilter.GaussianBlur(.6)),dtype=np.float32)
    rows.append({'page':index+1,'mae_255':float(np.abs(own_array-ref_array).mean()),
                 'blurred_mae_255':float(np.abs(own_blur-ref_blur).mean())})
report={'file':args.pdf.name,'sha256':hashlib.sha256(args.pdf.read_bytes()).hexdigest(),
        'pages_rendered':len(rows),'links':{'internal':internal,'http':external},
        'mean_mae_255':float(np.mean([row['mae_255'] for row in rows])),
        'max_mae_255':max(row['mae_255'] for row in rows),'pages':rows}
(args.output/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({key:value for key,value in report.items() if key!='pages'},indent=2))
