#!/usr/bin/env python3
# Writes noise WAVs and compares the 16-bit stereo WAVs warble writes, for
# files.sh. Standard library only.
#
#   wavtool.py gen OUT.wav RATE BITS SECONDS SEED
#   wavtool.py same A.wav B.wav         exit 0 if the samples are identical
#   wavtool.py differ A.wav B.wav PCT   exit 0 if at least PCT % differ
import random
import struct
import sys


def read(path):
    """(bits, samples as bytes) of a PCM WAV"""
    with open(path, 'rb') as f:
        buf = f.read()
    if buf[0:4] != b'RIFF' or buf[8:12] != b'WAVE':
        sys.exit('%s: not a WAV file' % path)
    pos, bits, data = 12, None, None
    while pos + 8 <= len(buf):
        cid = buf[pos:pos + 4]
        size = struct.unpack('<I', buf[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            bits = struct.unpack('<H', buf[pos + 22:pos + 24])[0]
        elif cid == b'data':
            data = buf[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    if bits is None or data is None:
        sys.exit('%s: no fmt or data chunk' % path)
    return bits, data


def gen(path, rate, bits, seconds, seed):
    rnd = random.Random(seed)
    width = bits // 8
    top = 1 << (bits - 1)
    out = bytearray()
    for _ in range(2 * rate * seconds):
        v = rnd.randrange(-top, top) >> 2               # -12 dBFS
        out += (v & ((1 << bits) - 1)).to_bytes(width, 'little')
    hdr = b'RIFF' + struct.pack('<I', 36 + len(out)) + b'WAVE'
    hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate,
                                 rate * 2 * width, 2 * width, bits)
    hdr += b'data' + struct.pack('<I', len(out))
    with open(path, 'wb') as f:
        f.write(hdr + out)


def samples16(path):
    bits, data = read(path)
    if bits != 16:
        sys.exit('%s: %d-bit, not 16' % (path, bits))
    return struct.unpack('<%dh' % (len(data) // 2), data)


def main(argv):
    cmd = argv[1]
    if cmd == 'gen':
        gen(argv[2], int(argv[3]), int(argv[4]), int(argv[5]), int(argv[6]))
        return 0
    a, b = samples16(argv[2]), samples16(argv[3])
    if len(a) != len(b):
        print('%d samples against %d' % (len(a), len(b)))
        return 1
    d = sum(1 for x, y in zip(a, b) if x != y)
    pct = 100 * d / len(a)
    if cmd == 'same':
        print('%d of %d samples differ' % (d, len(a)))
        return 0 if d == 0 else 1
    if cmd == 'differ':
        print('%.0f%% of samples differ' % pct)
        return 0 if pct >= float(argv[4]) else 1
    sys.exit('unknown command ' + cmd)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
