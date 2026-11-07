"""Builds FONT.WORLD, the cart text font beyond ASCII, from Fusion Pixel 8 px
(proportional, Japanese forms; OFL-1.1) and textfont.txt beside this file.

    python gen_textfont.py fusion.bdf --header core_textfont.h
                           [--file font.bin] [--sheet sheet.png]

Every glyph is a 12-row cell, a byte a row (the high bit on the left, at
most 8 wide), and an advance: 13 bytes. Capitals stand on rows 3 to 8,
lower case on 5 to 8; the baseline is under row 8.

Fusion supplies ASCII, punctuation, symbols, kana, Hangul and the
ideographs. Its accented capitals drop their accents, and it has no Greek or
Cyrillic, so every accented letter is composed here -- the base letter, then
each accent of its canonical decomposition from textfont.txt, stacked above
or hung below -- and the Greek and Cyrillic letters come from textfont.txt.

--header writes the part compiled into the firmware: everything below
U+3000, sorted by codepoint. --file writes the rest, which the player reads
a glyph at a time:

    "PBTF", version 1, height, ascent, 0
    256 pages, one for each high byte of the codepoint: the page's offset
    in the file (u32, 0 for none) and 32 bytes, a bit for each low byte
    present; each page's glyphs follow, in codepoint order, 13 bytes each.

--sheet draws every Greek, Cyrillic and composed letter, four times size,
for reviewing by eye."""

import os
import sys
import struct
import unicodedata
import zlib

HEIGHT, ASCENT, CELL_W = 12, 9, 8
CAP_TOP, X_TOP = 3, 5

here = os.path.dirname(os.path.abspath(__file__))


# ---- reading ----------------------------------------------------------------

def read_bdf(path):
    """codepoint -> (advance, [12 row bytes]) for glyphs no wider than a cell"""
    glyphs = {}
    cp = adv = bbx = rows = None
    for line in open(path, encoding='utf-8'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'ENCODING':
            cp, bbx, rows = int(p[1]), None, None
        elif p[0] == 'DWIDTH':
            adv = int(p[1])
        elif p[0] == 'BBX':
            bbx = [int(v) for v in p[1:5]]
        elif p[0] == 'BITMAP':
            rows = []
        elif p[0] == 'ENDCHAR':
            if cp is not None and bbx and rows is not None and cp < 0x10000:
                w, h, xo, yo = bbx
                if adv <= CELL_W and xo >= 0 and xo + w <= CELL_W:
                    glyphs[cp] = (adv, cell_of(rows, w, xo,
                                               ASCENT - (yo + h)))
            cp = rows = None
        elif rows is not None:
            rows.append(p[0])
    return glyphs


def cell_of(hexrows, w, xo, top):
    """BDF rows (hex, padded to whole bytes) to cell rows, placed at xo"""
    cell = [0] * HEIGHT
    for i, h in enumerate(hexrows):
        y = top + i
        if not (0 <= y < HEIGHT) or w == 0:
            continue
        v = int(h, 16) >> (len(h) * 4 - w)
        cell[y] = (v << (CELL_W - w - xo)) & 0xff
    return cell


def read_art(path):
    """glyphs (cp -> (adv, cell)), sames (cp -> cp), marks (cp -> rows)"""
    glyphs, sames, marks = {}, {}, {}
    lines = open(path, encoding='utf-8').read().split('\n')
    i = 0
    while i < len(lines):
        p = lines[i].split()
        i += 1
        if not p or p[0] == '#':
            continue
        if p[0] == 'same':
            sames[int(p[1][2:], 16)] = int(p[2][2:], 16)
            continue
        # the art: rows of only # (ink) and . (blank)
        art = []
        while i < len(lines) and lines[i].strip() and \
                set(lines[i].strip()) <= set('#.'):
            art.append(lines[i].strip())
            i += 1
        if p[0] == 'mark':
            marks[int(p[1][2:], 16)] = art
        elif p[0] == 'glyph':
            row = int(p[2])
            w = max(len(a) for a in art)
            cell = [0] * HEIGHT
            for k, a in enumerate(art):
                v = 0
                for x, ch in enumerate(a):
                    if ch == '#':
                        v |= 0x80 >> x
                cell[row + k] = v
            glyphs[int(p[1][2:], 16)] = (w + 1, cell)
    return glyphs, sames, marks


# ---- composing --------------------------------------------------------------

def ink(cell):
    """rows with ink: (top, bottom), and columns: (left, right); None empty"""
    ys = [y for y in range(HEIGHT) if cell[y]]
    if not ys:
        return None
    bits = 0
    for v in cell:
        bits |= v
    xs = [x for x in range(CELL_W) if bits & (0x80 >> x)]
    return ys[0], ys[-1], xs[0], xs[-1]


BELOW = {0x0327, 0x0328, 0x0326, 0x0323}


def compose(cp, font, marks):
    """cp drawn as its base letter with its accents, or None"""
    d = unicodedata.normalize('NFD', chr(cp))
    if len(d) < 2:
        return None
    base, accents = ord(d[0]), [ord(c) for c in d[1:]]
    if any(a not in marks for a in accents):
        return None
    above = [a for a in accents if a not in BELOW]
    # an i or j takes its accents without its dot
    if above and base == ord('i') and 0x131 in font:
        base = 0x131
    if above and base == ord('j') and 0x237 in font:
        base = 0x237
    if base not in font:
        return None
    adv, cell = font[base]
    cell = list(cell)
    box = ink(cell)
    if not box:
        return None
    top, bottom, left, right = box
    mid2 = left + right             # twice the ink's middle column
    # the accents above, the first nearest the letter, a blank row between
    # the letter and them where the cell has room
    heights = [len(marks[a]) for a in above]
    floor = top - 2 if top - 2 - sum(heights) + 1 >= 0 else top - 1
    floor = max(floor, sum(heights) - 1)
    widest = adv - 1
    for a in above:
        art = marks[a]
        w = max(len(r) for r in art)
        x0 = max(0, (mid2 + 1 - w) // 2)
        for k, r in enumerate(art):
            y = floor - (len(art) - 1) + k
            for x, ch in enumerate(r):
                if ch == '#' and 0 <= y < HEIGHT and x0 + x < CELL_W:
                    cell[y] |= 0x80 >> (x0 + x)
        widest = max(widest, x0 + w)
        floor -= len(art)
    for a in accents:
        if a not in BELOW:
            continue
        art = marks[a]
        w = max(len(r) for r in art)
        x0 = max(0, (mid2 + 1 - w) // 2)
        for k, r in enumerate(art):
            y = ASCENT + k
            for x, ch in enumerate(r):
                if ch == '#' and y < HEIGHT and x0 + x < CELL_W:
                    cell[y] |= 0x80 >> (x0 + x)
        widest = max(widest, x0 + w)
    return max(adv, widest + 1), cell


# the letters composed, where Unicode decomposes them
COMPOSED = [(0x00C0, 0x024F), (0x1E00, 0x1EFF), (0x0370, 0x03FF),
            (0x0400, 0x045F), (0x0490, 0x0491)]


def build(bdf, art):
    fusion = read_bdf(bdf)
    glyphs, sames, marks = read_art(art)
    font = dict(fusion)
    # the letters drawn here, then those shared with Latin
    for cp, g in glyphs.items():
        font[cp] = g
    for cp, src in sames.items():
        font[cp] = font[src]
    composed = {}
    for lo, hi in COMPOSED:
        for cp in range(lo, hi + 1):
            g = compose(cp, font, marks)
            if g:
                composed[cp] = g
    font.update(composed)
    return font, sorted(set(glyphs) | set(sames) | set(composed))


# ---- writing ----------------------------------------------------------------

def record(g):
    adv, cell = g
    return bytes([adv]) + bytes(cell)


def write_header(path, font):
    cps = sorted(cp for cp in font if 0x20 <= cp < 0x3000)
    with open(path, 'w', newline='\n') as f:
        f.write('/* GNU General Public License (version 2+)\n *\n'
                ' * Generated by tools/arcade/gen_textfont.py; do not edit.\n'
                ' * FONT.WORLD below U+3000, from Fusion Pixel 8 px (SIL Open'
                ' Font\n * License 1.1, '
                'https://github.com/TakWolf/fusion-pixel-font) and\n'
                ' * tools/arcade/textfont.txt: an advance, then 12 rows a '
                'byte each,\n * the high bit on the left. */\n\n')
        f.write('#define TEXTFONT_CORE %d\n\n' % len(cps))
        f.write('static const uint16_t textfont_cp[TEXTFONT_CORE] = {\n')
        for i in range(0, len(cps), 9):
            f.write('    ' + ', '.join('0x%04x' % c for c in cps[i:i + 9]) +
                    ',\n')
        f.write('};\n\nstatic const uint8_t textfont_glyph[TEXTFONT_CORE]'
                '[13] = {\n')
        for cp in cps:
            r = record(font[cp])
            f.write('    {' + ','.join('%d' % b for b in r) + '},\n')
        f.write('};\n')
    return len(cps)


def write_file(path, font):
    cps = sorted(cp for cp in font if 0x3000 <= cp < 0x10000)
    pages = {}
    for cp in cps:
        pages.setdefault(cp >> 8, []).append(cp)
    head = b'PBTF' + bytes([1, HEIGHT, ASCENT, 0])
    table_size = 256 * 36
    offset = len(head) + table_size
    table, body = b'', b''
    for page in range(256):
        if page not in pages:
            table += struct.pack('<I', 0) + bytes(32)
            continue
        bits = bytearray(32)
        for cp in pages[page]:
            bits[(cp & 0xff) >> 3] |= 1 << (cp & 7)
        table += struct.pack('<I', offset + len(body)) + bytes(bits)
        for cp in pages[page]:
            body += record(font[cp])
    with open(path, 'wb') as f:
        f.write(head + table + body)
    return len(cps), len(head) + len(table) + len(body)


def write_png(path, rows):
    """rows of 0/1 pixels, as a black-on-white PNG"""
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(255 - 255 * v for v in r) for r in rows)

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h,
                                                           8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def write_sheet(path, font, mine):
    """the Greek, Cyrillic and composed letters, a run a line, four times
    size, with Fusion's ASCII letters first to compare against"""
    runs = [list(range(0x41, 0x5b)), list(range(0x61, 0x7b))]
    run = []
    for cp in mine:
        if run and (cp - run[-1] > 1 or len(run) >= 32):
            runs.append(run)
            run = []
        run.append(cp)
    runs.append(run)
    scale, gap = 4, 2
    lines = []
    for r in runs:
        line = [[0] * 2 for _ in range(HEIGHT)]
        for cp in r:
            adv, cell = font[cp]
            for y in range(HEIGHT):
                line[y] += [(cell[y] >> (7 - x)) & 1 for x in range(adv)] + \
                    [0] * gap
        lines.append(line)
    width = max(len(l[0]) for l in lines) + 2
    out = []
    for l in lines:
        for y in range(HEIGHT):
            row = l[y] + [0] * (width - len(l[y]))
            big = [v for v in row for _ in range(scale)]
            out += [big] * scale
        out += [[0] * (width * scale)] * (scale * 2)
    write_png(path, out)


def main():
    args = sys.argv[1:]
    bdf = args[0]
    opt = {args[i]: args[i + 1] for i in range(1, len(args) - 1, 2)}
    font, mine = build(bdf, os.path.join(here, 'textfont.txt'))
    if '--header' in opt:
        n = write_header(opt['--header'], font)
        print('%s: %d glyphs' % (opt['--header'], n))
    if '--file' in opt:
        n, size = write_file(opt['--file'], font)
        print('%s: %d glyphs, %d bytes' % (opt['--file'], n, size))
    if '--sheet' in opt:
        write_sheet(opt['--sheet'], font, mine)
        print('%s: %d letters' % (opt['--sheet'], len(mine)))


if __name__ == '__main__':
    main()
