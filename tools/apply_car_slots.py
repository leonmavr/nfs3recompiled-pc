"""The car table holds every id a car can have, not the first 50.

sub_439a50 turns a car's id into its record:

    if (id > 0x31) return 0;                 // 0x439a5d
    record = table[id];                      // 0x439a69, table at 0x5f3c00
    if (!record) { ...walk the loaded cars to find it... }
    table[id] = record;                      // 0x439a97
    return record;

Fifty slots, and the car count is kept in the dword straight after the table
(0x5f3cc8), so it cannot be lengthened where it lies.  The port keeps the table
instead (src/nfs3hp/native_car_slots.cpp): the same pointers in the same order,
256 of them, in host memory.  This patch is the three places the table is
touched -- the bound, the read and the write -- and nothing else: the walk that
fills a slot, and every caller, are the game's own.

A car's id is one byte in its fedata.eng (TECHNICAL.md, Adding a car), so 256
is the whole of the range; the retail cars use 2 to 49, and a car added beyond
that used to be rejected by the bound above.
"""
from pathlib import Path

FILE = "nfs3hp.9.cpp"

# The three instructions that make up the table access, and what they become.
SITES = [
    # 00439a5d  83f831  -cmp eax, 0x31
    ("00439a5d  83f831", "cmp eax, 0x31", "cmp eax, kCarSlots - 1"),
    # 00439a69  8b98003c5f00  -mov ebx, dword ptr [eax + 0x5f3c00]
    ("00439a69  8b98003c5f00",
     "cpu.ebx = app->getMemory<x86::reg32>(cpu.eax + x86::reg32(6241280) /* 0x5f3c00 */);",
     "cpu.ebx = carSlotRecord(cpu.eax >> 2);"),
    # 00439a97  890495003c5f00  -mov dword ptr [edx*4 + 0x5f3c00], eax
    ("00439a97  890495003c5f00",
     "app->getMemory<x86::reg32>(x86::reg32(6241280) /* 0x5f3c00 */ + cpu.edx * 4) = cpu.eax;",
     "setCarSlotRecord(cpu.edx, cpu.eax);"),
]

HEADER = '''namespace nfs3hp
{
// Port (tools/apply_car_slots.py): defined in native_car_slots.cpp.
x86::reg32 carSlotRecord(x86::reg32 index);
void setCarSlotRecord(x86::reg32 index, x86::reg32 record);
'''

INCLUDE = '''#include "nfs3hp.h"
#include "car_slots.h"
'''


def apply(root: Path) -> None:
    path = root / "src/nfs3hp/disassembly" / FILE
    text = path.read_text(newline="")
    eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"

    def native(s):
        return eol.join(s.split("\n"))

    # The declarations, once, at the top of the port's own block.
    header = native(HEADER)
    if header not in text:
        anchor = native("namespace nfs3hp\n{\n")
        if text.count(anchor) != 1:
            raise RuntimeError("%s: namespace opening changed" % FILE)
        text = text.replace(anchor, header, 1)

    # kCarSlots, from the port's header.
    include = native(INCLUDE)
    if include not in text:
        original = native('#include "nfs3hp.h"\n')
        if not text.startswith(original):
            raise RuntimeError("%s: does not start with its own include" % FILE)
        text = include + text[len(original):]

    for instruction, old, new in SITES:
        old, new = native(old), native(new)
        anchor = "    // " + instruction
        start = text.find(anchor)
        if start < 0:
            raise RuntimeError("%s: no instruction %s" % (FILE, instruction))
        # Everything this one instruction generated: from its own address
        # comment up to the next one.
        stop = text.find(eol + "    // 00", start + len(anchor))
        if stop < 0:
            raise RuntimeError("%s: runaway block at %s" % (FILE, instruction))
        block = text[start:stop]
        if new in block:
            continue
        if block.count(old) != 1:
            raise RuntimeError("%s: unexpected shape at %s" % (FILE, instruction))
        text = text[:start] + block.replace(old, new, 1) + text[stop:]

    path.write_text(text, newline="")


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
