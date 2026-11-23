"""Draw the demo library's album covers. Every one is
generated, so the player carries no one else's artwork. Each cover is
built round a different kind of palette -- a pastel, a near-monochrome, a
neon -- because showing the colours follow the art is the point.

python covers.py OUTDIR"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 600
OUT = sys.argv[1] if len(sys.argv) > 1 else 'covers'
os.makedirs(OUT, exist_ok=True)


def grad(top, bottom, size=(S, S)):
    im = Image.new('RGB', size)
    d = ImageDraw.Draw(im)
    for y in range(size[1]):
        t = y / (size[1] - 1)
        d.line([(0, y), (size[0], y)],
               fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return im


def grain(im, amount=10, seed=1):
    rnd = random.Random(seed)
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            n = rnd.randint(-amount, amount)
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)),
                        max(0, min(255, b + n)))
    return im


def sun_stripes():
    im = grad((8, 70, 130), (6, 40, 96))
    d = ImageDraw.Draw(im)
    d.ellipse([150, 70, 450, 370], fill=(255, 204, 40))
    d.ellipse([205, 125, 395, 315], fill=(255, 76, 48))
    for i in range(11):
        d.rectangle([0, 330 + i * 26, S, 342 + i * 26], fill=(255, 122, 30))
    return grain(im, 6, 1)


def neon_grid():
    im = grad((52, 6, 120), (170, 20, 150))
    d = ImageDraw.Draw(im)
    for r in range(150, 0, -6):
        c = (255, int(60 + (150 - r) * 1.2), int(180 - r * 0.5))
        d.ellipse([300 - r, 230 - r, 300 + r, 230 + r], fill=c)
    d.rectangle([0, 300, S, S], fill=(30, 4, 70))
    for i in range(14):
        y = 300 + int((i / 13) ** 2 * 300)
        d.line([(0, y), (S, y)], fill=(0, 255, 240), width=3)
    for i in range(-10, 11):
        d.line([(300, 300), (300 + i * 90, S)], fill=(0, 255, 240), width=3)
    return im


def quiet_machine():
    im = grad((196, 236, 40), (150, 214, 30))
    d = ImageDraw.Draw(im)
    for i in range(9):
        d.line([(60, 120 + i * 40), (540, 120 + i * 40)], fill=(20, 24, 20), width=4)
    d.ellipse([370, 370, 480, 480], fill=(255, 40, 120))
    return grain(im, 5, 3)


def wildflowers():
    im = grad((255, 176, 206), (196, 244, 170))
    d = ImageDraw.Draw(im)
    rnd = random.Random(4)
    for _ in range(26):
        x, y = rnd.randint(30, 570), rnd.randint(60, 580)
        d.line([(x, y), (x + rnd.randint(-20, 20), y + 140)], fill=(40, 160, 80), width=5)
        col = rnd.choice([(255, 60, 130), (255, 170, 20), (140, 80, 255), (255, 70, 70)])
        for k in range(6):
            a = k * math.pi / 3
            px, py = x + 18 * math.cos(a), y + 18 * math.sin(a)
            d.ellipse([px - 14, py - 14, px + 14, py + 14], fill=col)
        d.ellipse([x - 9, y - 9, x + 9, y + 9], fill=(255, 230, 60))
    return im.filter(ImageFilter.GaussianBlur(1))


def aurora():
    im = grad((14, 20, 90), (40, 10, 100))
    glow = Image.new('RGB', (S, S))
    d = ImageDraw.Draw(glow)
    for i in range(60):
        x = i * 10
        y = 220 + 70 * math.sin(i / 7)
        d.line([(x, y - 140), (x, y - 70)], fill=(220, 60, 255), width=12)
        d.line([(x, y - 80), (x, y + 30)], fill=(30, 255, 160), width=12)
    glow = glow.filter(ImageFilter.GaussianBlur(24))
    im = Image.composite(glow, im, glow.convert('L'))
    d = ImageDraw.Draw(im)
    d.polygon([(0, 520), (140, 420), (260, 500), (380, 400), (600, 530), (600, 600), (0, 600)],
              fill=(8, 10, 40))
    rnd = random.Random(5)
    for _ in range(80):
        x, y = rnd.randint(0, S), rnd.randint(0, 200)
        d.point((x, y), fill=(230, 240, 255))
    return im


def harbour():
    im = grad((255, 206, 110), (255, 140, 90))
    d = ImageDraw.Draw(im)
    for i in range(7):
        y = 330 + i * 34
        pts = [(x, y + 10 * math.sin(x / 40 + i)) for x in range(0, S + 10, 10)]
        d.line(pts, fill=(10, 80, 200), width=10)
    d.ellipse([380, 90, 500, 210], fill=(240, 40, 70))
    return grain(im, 6, 6)


def concrete():
    im = Image.new('RGB', (S, S), (255, 210, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([60, 60, 300, 300], fill=(20, 20, 20))
    d.pieslice([300, 300, 540, 540], 180, 360, fill=(20, 20, 20))
    d.ellipse([340, 80, 520, 260], outline=(20, 20, 20), width=24)
    d.rectangle([60, 380, 260, 540], fill=(255, 56, 36))
    return im


def velvet():
    im = grad((130, 20, 170), (56, 8, 100))
    d = ImageDraw.Draw(im)
    for i in range(18):
        a = i * math.pi / 9
        d.line([(300, 300), (300 + 260 * math.cos(a), 300 + 260 * math.sin(a))],
               fill=(255, 196, 40), width=4)
    d.ellipse([200, 200, 400, 400], fill=(255, 196, 40))
    d.ellipse([240, 240, 360, 360], fill=(130, 20, 170))
    return grain(im, 5, 8)


def orchard():
    im = grad((90, 226, 200), (40, 196, 180))
    d = ImageDraw.Draw(im)
    rnd = random.Random(9)
    for _ in range(14):
        x, y, r = rnd.randint(40, 560), rnd.randint(40, 560), rnd.randint(30, 70)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 76, 96))
        d.ellipse([x - r // 3, y - r // 2, x, y - r // 5], fill=(255, 196, 200))
    return im


def embers():
    im = grad((70, 6, 20), (150, 20, 10))
    glow = Image.new('RGB', (S, S))
    d = ImageDraw.Draw(glow)
    rnd = random.Random(10)
    for _ in range(220):
        x, y = rnd.gauss(300, 120), rnd.gauss(400, 100)
        r = rnd.randint(5, 20)
        d.ellipse([x - r, y - r, x + r, y + r],
                  fill=(255, rnd.randint(90, 220), rnd.randint(0, 40)))
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    im = Image.composite(glow, im, glow.convert('L'))
    return im


def halftone():
    im = Image.new('RGB', (S, S), (230, 20, 120))
    d = ImageDraw.Draw(im)
    for y in range(0, S + 20, 20):
        for x in range(0, S + 20, 20):
            dist = math.hypot(x - 380, y - 240)
            r = max(0, 10 - dist / 40)
            if r > 0:
                d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 230, 30))
    d.rectangle([0, 470, S, 520], fill=(40, 30, 140))
    return im


def moss():
    im = grad((20, 110, 50), (10, 70, 40))
    d = ImageDraw.Draw(im)
    rnd = random.Random(12)
    for _ in range(400):
        x, y, r = rnd.randint(0, S), rnd.randint(0, S), rnd.randint(4, 22)
        c = rnd.choice([(80, 200, 70), (170, 220, 50), (240, 150, 40), (30, 160, 90)])
        d.ellipse([x - r, y - r, x + r, y + r], fill=c)
    d.rectangle([0, 250, S, 330], fill=(255, 226, 110))
    return im.filter(ImageFilter.GaussianBlur(1.5))


ALBUMS = {
    'paper_lanterns': sun_stripes, 'neon_tides': neon_grid,
    'quiet_machines': quiet_machine, 'wildflower_static': wildflowers,
    'northern_lines': aurora, 'saltwater_hymns': harbour,
    'concrete_bloom': concrete, 'velvet_hours': velvet,
    'glass_orchard': orchard, 'ember_season': embers,
    'satellite_hearts': halftone, 'moss_and_mineral': moss,
}
for name, fn in ALBUMS.items():
    fn().save(f'{OUT}/{name}.jpg', quality=88)
print(len(ALBUMS), 'covers')
