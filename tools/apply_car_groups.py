"""The car lists the menus sort get room for every car, not fifty.

`sub_43a580` walks the loaded cars, works out which of four groups each belongs
to (`sub_439730`) and appends its record to that group's list.  The lists are
fifty pointers each, 200 bytes apart:

    0x5552d8  0x5553a0  0x555468  0x555530

and the counts at 0x5555f8, which stay where the game keeps them.  A group's
address is `group * 200`, worked out with shifts in the two places that need
it, so this patch does two things: moves the four lists into the port's own
section, room for every car the list can hold in each (kCarGroupTable in
src/nfs3hp/car_arrays.h), and
turns `group * 200` into `group * kCarGroupStride` where the shift sequence
has finished.  The records, the order, the sort and the comparator are the
game's own.

This is the last of the fifty-car limits: the fifty-first car used to run off
the end of the fourth list into whatever followed (the counts, then live
globals), and the sort then compared a pointer that was never a car.
"""
from pathlib import Path

FILE = "nfs3hp.10.cpp"

# 200 bytes, fifty records: what the game multiplies a group number by.
OLD_STRIDE = 200

# (instruction address, the statement it emitted, and what follows it) for the
# two places that finish a group's address with `group * 200`.
STRIDE_SITES = [
    # 0043a605  c1e003  -shl eax, 3   (the scan appending a record)
    ("0043a605", "    cpu.eax <<= 3 /*0x3*/ % 32;"),
    # 0043a6c4  c1e203  -shl edx, 3   (the lookup of one group's records)
    ("0043a6c4", "    cpu.edx <<= 3 /*0x3*/ % 32;"),
]

# The lists themselves: the immediate each instruction loads, and the name it
# becomes.  The four the sort walks, and the one the counts are kept in step
# with -- `[eax*4 + 0x5552d4]` is the first list read one record back, so that
# it works out as `table + 4 * count` with no special case.
BASE_SITES = [
    # 0043a59d  b8d8525500  -mov eax, 0x5552d8   (clear the four lists)
    ("0043a59d", "cpu.eax = 5591768 /*0x5552d8*/;", "cpu.eax = kCarGroupTable;"),
    # 0043a60a  89b8d8525500  -mov [eax + 0x5552d8], edi
    ("0043a60a",
     "app->getMemory<x86::reg32>(cpu.eax + x86::reg32(5591768) /* 0x5552d8 */) = cpu.edi;",
     "app->getMemory<x86::reg32>(cpu.eax + x86::reg32(kCarGroupTable)) = cpu.edi;"),
    # 0043a61f  890c85d4525500  -mov [eax*4 + 0x5552d4], ecx
    ("0043a61f",
     "app->getMemory<x86::reg32>(x86::reg32(5591764) /* 0x5552d4 */ + cpu.eax * 4) = cpu.ecx;",
     "app->getMemory<x86::reg32>(x86::reg32(kCarGroupTable - 4) + cpu.eax * 4) = cpu.ecx;"),
    # 0043a638  b8d8525500  -mov eax, 0x5552d8   (sort the first list)
    ("0043a638", "cpu.eax = 5591768 /*0x5552d8*/;", "cpu.eax = kCarGroupTable + 0 * kCarGroupStride;"),
    # 0043a652  b8a0535500  -mov eax, 0x5553a0
    ("0043a652", "cpu.eax = 5591968 /*0x5553a0*/;", "cpu.eax = kCarGroupTable + 1 * kCarGroupStride;"),
    # 0043a66c  b868545500  -mov eax, 0x555468
    ("0043a66c", "cpu.eax = 5592168 /*0x555468*/;", "cpu.eax = kCarGroupTable + 2 * kCarGroupStride;"),
    # 0043a686  b830555500  -mov eax, 0x555530
    ("0043a686", "cpu.eax = 5592368 /*0x555530*/;", "cpu.eax = kCarGroupTable + 3 * kCarGroupStride;"),
    # 0043a6c7  8b848ad8525500  -mov eax, [edx + ecx*4 + 0x5552d8]
    ("0043a6c7",
     "cpu.eax = app->getMemory<x86::reg32>(cpu.edx + x86::reg32(5591768) /* 0x5552d8 */ + cpu.ecx * 4);",
     "cpu.eax = app->getMemory<x86::reg32>(cpu.edx + x86::reg32(kCarGroupTable) + cpu.ecx * 4);"),
]

# What the clear covers: four lists, not one.
FILL = (("0043a595", "cpu.edx = 800 /*0x320*/;", "cpu.edx = kCarGroupTableSize;"),)

INCLUDE = '#include "nfs3hp.h"\n#include "car_arrays.h"\n'
GAME_INCLUDE = '#include "nfs3hp.h"\n'

NOTE = "    // Port (tools/apply_car_groups.py): %s\n"


def site(text, address, old, new, what=None):
    """`old` replaced by `new` in what `address` emitted, under a port note."""
    eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"
    # A comment may or may not be indented, depending on what came before it.
    anchor = "// " + address
    found = text.find(anchor)
    if found < 0:
        raise RuntimeError("%s: no instruction %s" % (FILE, address))
    start = text.rfind("\n", 0, found) + 1
    ends = [text.find(eol + indent + "// 00", found) for indent in ("", "    ")]
    ends = [end for end in ends if end >= 0]
    if not ends:
        raise RuntimeError("%s: runaway block at %s" % (FILE, address))
    stop = min(ends)
    block = text[start:stop]
    if new in block:
        return text
    if block.count(old) != 1:
        raise RuntimeError("%s: unexpected shape at %s" % (FILE, address))
    note = NOTE % what if what else ""
    return text[:start] + block.replace(old, note + new, 1) + text[stop:]


def apply(root: Path) -> None:
    path = root / "src/nfs3hp/disassembly" / FILE
    text = path.read_text(newline="")
    eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"

    for address, old, new in FILL:
        text = site(text, address, old, new, "four lists of every car, not fifty.")

    for address, old, new in BASE_SITES:
        text = site(text, address, old, new, "the car groups moved to kCarGroupTable.")

    # `group * 200` -> `group * kCarGroupStride`, after the shifts have run.
    for address, shr in STRIDE_SITES:
        register = "eax" if "cpu.eax" in shr else "edx"
        fix = (shr + eol + "    cpu.%s = x86::reg32(cpu.%s / %d * kCarGroupStride);"
               " /* 200 was fifty records a group */" % (register, register, OLD_STRIDE))
        text = site(text, address, shr, fix)

    include = INCLUDE.replace("\n", eol)
    if include not in text:
        original = GAME_INCLUDE.replace("\n", eol)
        if not text.startswith(original):
            raise RuntimeError("%s: does not start with its own include" % FILE)
        text = include + text[len(original):]

    path.write_text(text, newline="")


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
