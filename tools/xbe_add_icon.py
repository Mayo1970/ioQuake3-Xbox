#!/usr/bin/env python3
"""Add or replace the $$XTIMAGE title image section in an XBE built by cxbe."""
import struct
import sys

SEC_SIZE = 56
NAME = b"$$XTIMAGE\0"


def rup(v, a):
    return (v + a - 1) & ~(a - 1)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: xbe_add_icon.py default.xbe TitleImage.xbx")
    xbe = bytearray(open(sys.argv[1], "rb").read())
    img = open(sys.argv[2], "rb").read()
    if xbe[:4] != b"XBEH" or img[:4] != b"XPR0":
        sys.exit("bad XBE or XBX input")

    base, hdr_size, img_size = struct.unpack_from("<III", xbe, 0x104)
    nsec, sec_addr = struct.unpack_from("<II", xbe, 0x11C)
    sec_off = sec_addr - base

    secs = []
    for i in range(nsec):
        s = list(struct.unpack_from("<9I", xbe, sec_off + i * SEC_SIZE))
        name_off = s[5] - base
        s.append(bytes(xbe[name_off:xbe.index(0, name_off)]))
        secs.append(s)
    for s in secs:
        if s[9] == NAME[:-1]:
            if s[4] != len(img):
                sys.exit("existing $$XTIMAGE has a different size, rebuild the XBE")
            xbe[s[3]:s[3] + len(img)] = img
            open(sys.argv[1], "wb").write(xbe)
            print("XBE icon: $$XTIMAGE replaced in place")
            return

    first_raw = min(s[3] for s in secs)
    new_hdr = rup(hdr_size, 4)
    need = (nsec + 1) * SEC_SIZE + 4 + len(NAME)
    if new_hdr + need + 4 > first_raw:
        sys.exit("no room in XBE header page for the extra section")

    # Copy the table to the header slack instead of growing it in place.
    table_off = new_hdr
    ref_off = table_off + (nsec + 1) * SEC_SIZE
    name_off = ref_off + 4
    xbe[table_off:table_off + nsec * SEC_SIZE] = xbe[sec_off:sec_off + nsec * SEC_SIZE]

    raw_addr = rup(len(xbe), 0x1000)
    xbe.extend(b"\0" * (raw_addr - len(xbe)))
    xbe.extend(img)
    xbe.extend(b"\0" * (rup(len(xbe), 0x1000) - len(xbe)))

    vaddr = rup(base + img_size, 0x1000)
    vsize = rup(len(img), 4)
    struct.pack_into("<9I20s", xbe, table_off + nsec * SEC_SIZE,
                     0, vaddr, vsize, raw_addr, len(img),
                     base + name_off, 0, base + ref_off, base + ref_off + 2,
                     b"\0" * 20)
    xbe[ref_off:ref_off + 4] = b"\0\0\0\0"
    xbe[name_off:name_off + len(NAME)] = NAME

    struct.pack_into("<I", xbe, 0x108, name_off + len(NAME))
    struct.pack_into("<I", xbe, 0x10C, vaddr + vsize - base)
    struct.pack_into("<II", xbe, 0x11C, nsec + 1, base + table_off)
    open(sys.argv[1], "wb").write(xbe)
    print("XBE icon: $$XTIMAGE added (%d bytes at 0x%X)" % (len(img), raw_addr))


if __name__ == "__main__":
    main()
