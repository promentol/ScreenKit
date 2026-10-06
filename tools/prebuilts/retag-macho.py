#!/usr/bin/env python3
"""Rewrite the LC_BUILD_VERSION platform of Mach-O objects inside a static archive.

Upstream ANGLE prebuilts exist for iOS but not tvOS, and the two are identical
code for the same arch on the same host -- only the platform field differs, which
is enough for ld to refuse:

    ld: building for 'tvOS-simulator', but linking in object file built for 'iOS-simulator'

This flips that one dword in place. The field keeps its size, so no load command
is resized -- which is exactly what `vtool -set-build-version` cannot do on a .o
("not enough space to hold load commands"). vtool is also unusable here for a
second reason: its platform name table has `iossim` but no tvOS-simulator entry.

SPIKE TOOL. Shipping should build ANGLE from source with gn target_platform="tvos".

    retag-macho.py --from ios-simulator --to tvos-simulator lib*.a
"""
import argparse
import struct
import sys

# Mach-O platform ids (mach-o/loader.h)
PLATFORMS = {
    "macos": 1, "ios": 2, "tvos": 3, "watchos": 4, "bridgeos": 5,
    "maccatalyst": 6, "ios-simulator": 7, "tvos-simulator": 8,
    "watchos-simulator": 9, "driverkit": 10,
}
LC_BUILD_VERSION = 0x32
MH_MAGIC_64 = 0xFEEDFACF


def patch_macho(buf, base, src, dst):
    if struct.unpack_from("<I", buf, base)[0] != MH_MAGIC_64:
        return 0
    ncmds = struct.unpack_from("<I", buf, base + 16)[0]
    off, changed = base + 32, 0
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", buf, off)
        if cmd == LC_BUILD_VERSION:
            if struct.unpack_from("<I", buf, off + 8)[0] == src:
                struct.pack_into("<I", buf, off + 8, dst)
                changed += 1
        if cmdsize == 0:
            break
        off += cmdsize
    return changed


def patch_file(path, src, dst):
    with open(path, "rb") as f:
        buf = bytearray(f.read())

    if bytes(buf[:8]) != b"!<arch>\n":          # bare Mach-O, not an archive
        n = patch_macho(buf, 0, src, dst)
        with open(path, "wb") as f:
            f.write(buf)
        return n, 1

    pos, total, members = 8, 0, 0
    while pos + 60 <= len(buf):
        name = bytes(buf[pos:pos + 16]).decode("ascii", "replace")
        try:
            size = int(bytes(buf[pos + 48:pos + 58]).decode("ascii").strip())
        except ValueError:
            break
        data = pos + 60
        # BSD long-name form "#1/<len>": the name sits at the start of the data
        # area and `size` already counts it.
        if name.startswith("#1/"):
            data += int(name[3:].strip())
        total += patch_macho(buf, data, src, dst)
        members += 1
        pos = pos + 60 + size + (size % 2)      # members are 2-byte aligned
    with open(path, "wb") as f:
        f.write(buf)
    return total, members


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", required=True, choices=sorted(PLATFORMS))
    ap.add_argument("--to", dest="dst", required=True, choices=sorted(PLATFORMS))
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    src, dst = PLATFORMS[args.src], PLATFORMS[args.dst]
    grand = 0
    for path in args.files:
        n, m = patch_file(path, src, dst)
        grand += n
        print(f"{path}: {n} objects retagged {args.src} -> {args.dst} ({m} members)")
    if grand == 0:
        print("ERROR: nothing retagged -- already correct, or unexpected format",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
