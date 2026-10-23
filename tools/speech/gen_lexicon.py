#!/usr/bin/env python3
"""Write the speech dictionary, cmudict.bin, from the CMU Pronouncing
Dictionary.

usage: gen_lexicon.py <cmudict.dict> <cmudict.bin>

cmudict.dict is github.com/cmusphinx/cmudict's; its licence ships beside the
output. Only each word's first pronunciation is kept.

The layout, all integers little-endian, read by apps-ipod/speech/lexicon.c:
    "PBLX", u16 version (1), u16 block size, u32 blocks, u32 words
    u32 offset of each block, from the start of the file
    the blocks: each entry is u8 prefix (bytes shared with the entry
    before; 0 for a block's first), u8 suffix length, the suffix, u8 phone
    count, the phones. Words are lowercase ASCII, sorted bytewise.
A phone is ARPABET index * 4 + stress, stress 0, 1 or 2 for a vowel and 3
for a consonant, the index in ARPABET's order below.
"""
import struct
import sys

ARPABET = ("AA AE AH AO AW AY B CH D DH EH ER EY F G HH IH IY JH K L M N NG "
           "OW OY P R S SH T TH UH UW V W Y Z ZH").split()
BLOCK = 32


def phone_code(p):
    if p[-1] in "012":
        return ARPABET.index(p[:-1]) * 4 + int(p[-1])
    return ARPABET.index(p) * 4 + 3


def main():
    words = {}
    for line in open(sys.argv[1], encoding="utf8"):
        f = line.split("#")[0].split()
        if f and "(" not in f[0]:
            words.setdefault(f[0], f[1:])
    keys = sorted(words, key=lambda w: w.encode("ascii"))

    blocks = []
    for b in range(0, len(keys), BLOCK):
        out = bytearray()
        prev = b""
        for i, w in enumerate(keys[b:b + BLOCK]):
            wb = w.encode("ascii")
            n = 0
            if i:
                while n < min(len(wb), len(prev), 255) and wb[n] == prev[n]:
                    n += 1
            phones = bytes(phone_code(p) for p in words[w])
            out += bytes([n, len(wb) - n]) + wb[n:] + bytes([len(phones)])
            out += phones
            prev = wb
        blocks.append(bytes(out))

    header = b"PBLX" + struct.pack("<HHII", 1, BLOCK, len(blocks), len(keys))
    offset = len(header) + 4 * len(blocks)
    index = bytearray()
    for blk in blocks:
        index += struct.pack("<I", offset)
        offset += len(blk)
    with open(sys.argv[2], "wb") as f:
        f.write(header + index + b"".join(blocks))
    print("%d words in %d blocks, %d bytes" % (len(keys), len(blocks), offset))


main()
