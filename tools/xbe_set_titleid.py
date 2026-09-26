#!/usr/bin/env python3
"""Set the certificate Title ID, which cxbe always writes as 0xFFFF0002."""
import struct
import sys

BASE_ADDR_OFF = 0x104
CERT_ADDR_OFF = 0x118
TITLE_ID_OFF = 0x8


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: xbe_set_titleid.py default.xbe HEXID")
    xbe = bytearray(open(sys.argv[1], "rb").read())
    if xbe[:4] != b"XBEH":
        sys.exit("bad XBE input")

    new_id = int(sys.argv[2], 16)
    base, = struct.unpack_from("<I", xbe, BASE_ADDR_OFF)
    cert, = struct.unpack_from("<I", xbe, CERT_ADDR_OFF)
    off = cert - base + TITLE_ID_OFF
    old_id, = struct.unpack_from("<I", xbe, off)
    if old_id != new_id:
        struct.pack_into("<I", xbe, off, new_id)
        open(sys.argv[1], "wb").write(xbe)
    print("XBE title ID: 0x%08X -> 0x%08X" % (old_id, new_id))


if __name__ == "__main__":
    main()
