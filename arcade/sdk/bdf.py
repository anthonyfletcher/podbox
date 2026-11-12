"""Reading a BDF font, for gen_font.py and gen_textfont.py; pbgc reads a
cart's with src/bdf.js, by the same rules.

    read(path) -> (ascent, descent, {codepoint: Glyph})

A glyph's rows are its BBX's, top first, each an int whose high bit (bit
w - 1) is the leftmost pixel. Characters with ENCODING -1 are left out."""

from collections import namedtuple

Glyph = namedtuple('Glyph', 'adv w h xo yo rows')


def read(path):
    ascent = descent = None
    glyphs = {}
    cp = adv = bbx = rows = None
    for line in open(path, encoding='utf-8'):
        p = line.split()
        if not p:
            continue
        if p[0] == 'FONT_ASCENT':
            ascent = int(p[1])
        elif p[0] == 'FONT_DESCENT':
            descent = int(p[1])
        elif p[0] == 'STARTCHAR':
            cp, adv, bbx, rows = None, 0, None, None
        elif p[0] == 'ENCODING':
            cp = int(p[1])
        elif p[0] == 'DWIDTH':
            adv = int(p[1])
        elif p[0] == 'BBX':
            bbx = [int(v) for v in p[1:5]]
        elif p[0] == 'BITMAP':
            rows = []
        elif p[0] == 'ENDCHAR':
            if cp is not None and cp >= 0 and bbx and rows is not None:
                w, h, xo, yo = bbx
                bits = [int(r, 16) >> (len(r) * 4 - w) if w else 0
                        for r in rows[:h]]
                glyphs[cp] = Glyph(adv, w, h, xo, yo, bits)
            rows = None
        elif rows is not None:
            rows.append(p[0])
    return ascent, descent, glyphs


def art(g):
    """a glyph's rows as strings of # (ink) and . (blank)"""
    return [''.join('#' if v >> (g.w - 1 - x) & 1 else '.'
                    for x in range(g.w)) for v in g.rows]
