"""Unpack the car and art archives of a retail disc into the installed layout.

The 1998 disc keeps some of its data packed: every car -- the 22 player cars,
the cop cars, and each of traffic's 17 bodies -- is a BIGF archive, `car.viv`,
holding that car's model (`car.fce`), its texture (`fedata.fsh`), its text
(`fedata.eng` and the other languages), its sounds and its dashboard; the
disc's `fedata/art` holds the showcase pictures the same way.  The game itself
only ever asks for the unpacked files, and its installer is what unpacks them:
a folder copied straight off the disc therefore has no car models in it, and
the game starts with 44 "unable to open file" lines and no cars.

This does what the installer does, in place:

    python3 tools/extract_viv.py .

The arguments are the folders to search (`fedata/` and `gamedata/` by
convention); each archive is unpacked beside itself.  A file that is already
there is left alone unless --force is given, so extracting over a working copy
cannot lose anything.  The archives themselves are kept: nothing reads them
once their contents are out, and they are the disc's own data.

BIGF, EA's archive of the period: "BIGF", the file's size and the number of
entries, then the offset the first entry's data starts at, all four bytes wide
and big-endian; then for each entry its offset and size the same way and its
name, NUL-terminated and with no padding, so the table is not aligned.
"""
import argparse
import struct
import sys
from pathlib import Path

HEADER = 16


def entries(data):
    """(name, offset, size) for every file an archive holds."""
    if data[:4] != b"BIGF":
        raise ValueError("not a BIGF archive")
    count, = struct.unpack(">I", data[8:12])
    at = HEADER
    for _ in range(count):
        offset, size = struct.unpack(">II", data[at:at + 8])
        at += 8
        end = data.index(b"\0", at)
        name = data[at:end].decode("latin-1")
        at = end + 1
        yield name, offset, size


def extract(archive, force):
    data = archive.read_bytes()
    try:
        found = list(entries(data))
    except (ValueError, struct.error) as bad:
        print("%s: %s" % (archive, bad), file=sys.stderr)
        return 0, 0
    folder = archive.parent
    written = skipped = 0
    for name, offset, size in found:
        # The archives are flat; a name with a separator in it would be a
        # path out of the folder, which no disc has.
        if "/" in name or "\\" in name or name in (".", ".."):
            print("%s: skipping %r" % (archive, name), file=sys.stderr)
            continue
        out = folder / name
        if out.exists() and not force:
            skipped += 1
            continue
        out.write_bytes(data[offset:offset + size])
        written += 1
    return written, skipped


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folders", nargs="*", default=["."], help="folders to search for *.viv")
    parser.add_argument("--force", action="store_true", help="overwrite files that are already there")
    args = parser.parse_args()

    archives = sorted({p for folder in args.folders or ["."]
                       for p in Path(folder).rglob("*")
                       if p.is_file() and p.suffix.lower() == ".viv"})
    if not archives:
        print("no .viv archives under %s" % " ".join(args.folders or ["."]), file=sys.stderr)
        return 1

    total = 0
    for archive in archives:
        written, skipped = extract(archive, args.force)
        total += written
        if skipped:
            print("%s: %d written, %d already there" % (archive, written, skipped))
        else:
            print("%s: %d written" % (archive, written))
    print("%d archives, %d files" % (len(archives), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
