#!/usr/bin/env python3
"""Clear XINIT_LIMIT_DEVKIT_MEMORY, which cxbe always sets, so 128 MB units can use all RAM."""
import struct
import sys

INIT_FLAGS_OFF = 0x124
LIMIT_64MB = 0x4


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: xbe_clear_limit64.py default.xbe")
    xbe = bytearray(open(sys.argv[1], "rb").read())
    if xbe[:4] != b"XBEH":
        sys.exit("bad XBE input")

    flags = struct.unpack_from("<I", xbe, INIT_FLAGS_OFF)[0]
    if flags & LIMIT_64MB:
        struct.pack_into("<I", xbe, INIT_FLAGS_OFF, flags & ~LIMIT_64MB)
        open(sys.argv[1], "wb").write(xbe)
    print("XBE init flags: 0x%X -> 0x%X" % (flags, flags & ~LIMIT_64MB))


if __name__ == "__main__":
    main()
