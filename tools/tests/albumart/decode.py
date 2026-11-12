#!/usr/bin/env python3
"""Compile the current image pipeline for an ipod6g host configuration.

Pixel pipeline uses ASan/UBSan. Linked metadata stream helpers use their
current, uninstrumented code so ASan does not root unused metadata tables.
Host I/O opens only fixtures below this directory. No simulator UI/device runs.
Requires host gcc, make, SDL2 development files and Python Pillow.
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
from PIL import Image, ImageDraw, ImageOps
from host import ROOT, OUT, FLAGS, CC
SUPPORT = Path(__file__).resolve().parent
args = [CC, *FLAGS]
# The CheckWPS port must not retain the removed global callback API.
assert 'image_row_hook' not in (ROOT / 'tools/checkwps/stubs.c').read_text()
image_files = ['apps-ipod/draw/'+s+'.c' for s in ['jpeg_load','png_load','gif_load','webp_load','resize','bmp','image_load']]
image_files += ['apps-ipod/files/'+s+'.c' for s in ['byte_stream','inflate']]
metadata_files = ['lib/rbcodec/metadata/'+s+'.c' for s in ['id3tags','ogg','vorbis','metadata_common','replaygain']]
objects = []
for file in image_files+metadata_files:
    obj = OUT/(Path(file).stem+'.o')
    flags = ['-fno-sanitize=all'] if file in metadata_files else []
    subprocess.run(args+flags+['-c',str(ROOT/file),'-o',str(obj)],check=True)
    objects.append(str(obj))
subprocess.run(['cc','-fsanitize=address,undefined','-c',str(SUPPORT/'host_io.c'),'-o',str(OUT/'host_io.o')],check=True)
subprocess.run(args+(['-DREQUEST_CALLBACKS'] if 'bool (*progress)' in (ROOT/'apps-ipod/draw/resize.h').read_text() else [])+[str(SUPPORT/'decode_probe.c')]+objects+[str(OUT/'host_io.o'),'-Wl,--gc-sections','-o',str(OUT/'decode_probe')],check=True)

fixtures=OUT/'fixtures';fixtures.mkdir(exist_ok=True)
im=Image.new('RGB',(400,240));draw=ImageDraw.Draw(im)
for box,color in [((0,0,199,119),'red'),((200,0,399,119),'green'),((0,120,199,239),'blue'),((200,120,399,239),'yellow')]:
    draw.rectangle(box,fill=color)
im.save(fixtures/'baseline.jpg',quality=95)
im.save(fixtures/'progressive.jpg',quality=95,progressive=True)
im.convert('CMYK').save(fixtures/'cmyk.jpg',quality=95)
for orientation in range(1,9):
    orientation_tag=Image.Exif();orientation_tag[274]=orientation
    im.save(fixtures/f'orientation{orientation}.jpg',quality=95,exif=orientation_tag)
exif=Image.Exif();exif[274]=6
im.save(fixtures/'orientation6.jpg',quality=95,exif=exif)
im.save(fixtures/'rgb.png');im.save(fixtures/'rgb.bmp');im.save(fixtures/'image.gif')
a=im.convert('RGBA');a.putalpha(128);a.save(fixtures/'alpha.png')
im.save(fixtures/'lossy.webp',lossless=False);im.save(fixtures/'lossless.webp',lossless=True)
x=(fixtures/'orientation6.jpg').read_bytes()
assert x[2:4]==b'\xff\xe0'
n=int.from_bytes(x[4:6],'big');x=x[:2]+x[4+n:]
(fixtures/'exif-first.jpg').write_bytes(x)

logs=[]
def test(file, expected=0, extra=(), label=''):
    target=fixtures/(file.name+label+'.ppm')
    cmd=[str(OUT/'decode_probe'),str(file),str(target),*map(str,extra)]
    p=subprocess.run(cmd,capture_output=True,text=True)
    logs.append(f'COMMAND {shlex.join(cmd)}\nEXIT {p.returncode}\n{p.stdout}{p.stderr}')
    print(file.name,label,p.returncode,p.stdout.strip())
    assert p.returncode == expected
    assert 'runtime error:' not in p.stderr and 'AddressSanitizer' not in p.stderr

for f in sorted(fixtures.iterdir()):
    if f.suffix in ['.jpg','.png','.bmp','.gif','.webp']:
        test(f,4 if f.name=='lossless.webp' else 0)
test(fixtures/'baseline.jpg',4,(1024,),'-small-buffer')
test(fixtures/'rgb.png',4,(1024,),'-small-buffer')
# A valid UTF-16LE APIC description ends at byte 19; the original parser picks 17.
apic=bytes([1])+b'image/png\0'+bytes([3,255,254,65,0,0,0])
assert len(apic)==18
# PNG's shorter MIME makes its correct position 18 (JPEG's was 19).
track=fixtures/'utf16-apic.bin';track.write_bytes(apic+(fixtures/'rgb.png').read_bytes())
test(track,4,(1024*1024,16),'-parsed-offset')
test(track,0,(1024*1024,18),'-correct-offset')
(OUT/'decode-results.log').write_text('\n'.join(logs))
checks={}
for name in ['baseline.jpg','progressive.jpg','cmyk.jpg','rgb.png','alpha.png','rgb.bmp','exif-first.jpg']:
    pic=Image.open(str(fixtures/name)+'.ppm')
    checks[name]={'size':pic.size,'quadrants':[pic.getpixel((int(pic.width*x),int(pic.height*y))) for x,y in [(0.25,0.25),(0.75,0.25),(0.25,0.75),(0.75,0.75)]]}
checks['orientation_expected']=ImageOps.exif_transpose(Image.open(fixtures/'exif-first.jpg')).size
checks['mp4_jpeg_match']=x[:4]==b'\xff\xd8\xff\xe0'
(OUT/'pixel-checks.json').write_text(json.dumps(checks,indent=2)+'\n')

for orientation in range(1,9):
    name=f'orientation{orientation}.jpg'
    actual=Image.open(str(fixtures/name)+'.ppm')
    expected=ImageOps.exif_transpose(Image.open(fixtures/name)).convert('RGB')
    assert actual.width*expected.height==actual.height*expected.width
    for x,y in [(0.25,0.25),(0.75,0.25),(0.25,0.75),(0.75,0.75)]:
        aa=actual.getpixel((int(actual.width*x),int(actual.height*y)))
        ee=expected.getpixel((int(expected.width*x),int(expected.height*y)))
        assert max(abs(a-b) for a,b in zip(aa,ee))<40,(orientation,aa,ee)
print('PASS: EXIF orientations 1-8 dimensions and quadrant pixels')

for name in ['baseline.jpg', 'rgb.png', 'rgb.bmp', 'image.gif', 'lossy.webp']:
    cmd = [str(OUT / 'decode_probe'), str(fixtures / name), str(OUT / 'unused.ppm')]
    result = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, ART_TEST_CALLBACKS='1'))
    (OUT / ('callbacks-' + name + '.log')).write_text(shlex.join(cmd) + '\n' + result.stdout + result.stderr)
    assert result.returncode == 0, (name, result.stderr)
print('PASS: request cancellation and nested independent decode for JPEG/PNG/BMP/GIF/WebP')
