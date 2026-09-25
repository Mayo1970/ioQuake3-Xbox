#!/usr/bin/env python3
"""Convert a 128x128 PNG to an Xbox TitleImage.xbx (XPR0, DXT1, no mipmaps)."""
import struct
import sys
import zlib

SIZE = 128
FORMAT_DXT1_128 = 0x07710C29  # dma A | 2D | DXT1 | 1 mip | log2 w=7 | log2 h=7


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("not a PNG")
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
    if depth != 8 or interlace or ctype not in (0, 2, 3, 4, 6):
        sys.exit("unsupported PNG (need 8-bit, non-interlaced)")
    if (w, h) != (SIZE, SIZE):
        sys.exit("PNG must be %dx%d, got %dx%d" % (SIZE, SIZE, w, h))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        rows.append(cur)
        prev = cur
    px = []
    for cur in rows:
        for x in range(w):
            if ctype == 6:
                px.append(tuple(cur[x * 4:x * 4 + 4]))
            elif ctype == 2:
                px.append(tuple(cur[x * 3:x * 3 + 3]) + (255,))
            elif ctype == 0:
                px.append((cur[x],) * 3 + (255,))
            elif ctype == 4:
                px.append((cur[x * 2],) * 3 + (cur[x * 2 + 1],))
            else:
                i = cur[x]
                a = trns[i] if trns and i < len(trns) else 255
                px.append(tuple(plte[i * 3:i * 3 + 3]) + (a,))
    return px


def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def encode_block(block):
    opaque = [p for p in block if p[3] >= 128]
    if not opaque:
        return struct.pack("<HHI", 0, 0, 0xFFFFFFFF)  # fully transparent
    has_alpha = len(opaque) != 16
    lo = [min(p[i] for p in opaque) for i in range(3)]
    hi = [max(p[i] for p in opaque) for i in range(3)]
    best = None
    diag = [(lo, hi), ([lo[0], hi[1], lo[2]], [hi[0], lo[1], hi[2]])]
    for a, b in diag:
        # Pull endpoints in slightly to reduce banding on the block extremes.
        e0 = [(a[i] * 15 + b[i]) // 16 for i in range(3)]
        e1 = [(b[i] * 15 + a[i]) // 16 for i in range(3)]
        c0, c1 = to565(*e0), to565(*e1)
        if has_alpha:
            if c0 > c1:
                c0, c1 = c1, c0
        elif c0 < c1:
            c0, c1 = c1, c0
        p0, p1 = from565(c0), from565(c1)
        if has_alpha:
            pal = [p0, p1, tuple((p0[i] + p1[i]) // 2 for i in range(3))]
        else:
            pal = [p0, p1,
                   tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)),
                   tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3))]
        idx, err = 0, 0
        for n, p in enumerate(block):
            if has_alpha and p[3] < 128:
                idx |= 3 << (2 * n)
                continue
            d = [sum((p[i] - q[i]) ** 2 for i in range(3)) for q in pal]
            k = d.index(min(d))
            err += d[k]
            idx |= k << (2 * n)
        if best is None or err < best[0]:
            best = (err, c0, c1, idx)
    return struct.pack("<HHI", best[1], best[2], best[3])


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: png2xbx.py in.png out.xbx")
    px = read_png(sys.argv[1])
    tex = b""
    for by in range(0, SIZE, 4):
        for bx in range(0, SIZE, 4):
            tex += encode_block([px[(by + y) * SIZE + bx + x]
                                 for y in range(4) for x in range(4)])
    hdr = struct.pack("<4sIIIIIIII", b"XPR0", 0x800 + len(tex), 0x800,
                      0x00040001, 0, 0, FORMAT_DXT1_128, 0, 0xFFFFFFFF)
    hdr += b"\xAD" * (0x800 - len(hdr))
    open(sys.argv[2], "wb").write(hdr + tex)
    print("wrote %s (%d bytes)" % (sys.argv[2], 0x800 + len(tex)))


if __name__ == "__main__":
    main()
