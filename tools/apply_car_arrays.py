"""The per-car tables get room for every id a car can have.

As the game scans `carmodel` it fills six arrays indexed by a car's place in
the load order, each sized for fifty cars and packed against the next:

    A 0x554fa8  16-bit    B 0x55500a  16-bit    C 0x555138  16-bit
    D 0x5551a2  16-bit    E 0x55506c  32-bit    F 0x555214  32-bit

That is the ceiling the port kept hitting.  The table `sub_439a50` memoises
into was raised to 256 (tools/apply_car_slots.py) and the cars still stopped at
fifty, because these overflowed: the fifty-first car wrote into the array
packed after its own, and past the last one sits live data (0x5552d8).

Every access is `base + index * element`, with the index already scaled by the
time it reaches the base, so the base is all this has to change.  Each array
moves to a section of its own, kCarArrayA-kCarArrayF in
src/nfs3hp/car_arrays.h, with room for every car the list can hold.  The
scan, the code that reads
the tables back, and the terminator it writes one past the last car are all
the game's own, unchanged.

One thing this cannot reach: the car menus do not name the arrays in code but
through five list records in the game's data, twenty bytes each from 0x5564ac
-- the choice a list drives, its name, and the three tables of A, B and E it
shows ("cars", "car2", "carShowcase", "carCompare1", "carCompare2").  Left
pointing at the old addresses those lists came up empty: the Player Car row
showed the game's own "BAD TEXT INDEX 0" line and the lists scrolled through
nothing.  nfs3hp_main.cpp repoints them as the game starts.
"""
from pathlib import Path

DISASSEMBLY = "src/nfs3hp/disassembly"
GAME_INCLUDE = '#include "nfs3hp.h"\n'

CONSTRUCTOR_FILE = "nfs3hp.cpp"
CONSTRUCTOR_INCLUDE = GAME_INCLUDE + '#include "car_arrays.h"\n'

# The last entry of the game's own section list, and the port's appended after
# it: the per-car tables, in memory of the port's own, clear of everything the
# game loads.  Everything else in the list is the game's.
LAST_SECTION = "{ voodoo2a::s__rsrcSegment, 0xa97200, 1024 } })"
LAST_SECTION_PORT = ("{ voodoo2a::s__rsrcSegment, 0xa97200, 1024 }, "
                     "{ nullptr, kCarArrayBase, kCarArraySize } })")

# The six arrays, and the addresses inside them the generated code names
# outright, as (old address, bytes into the array).
ARRAYS = [
    ("kCarArrayA", [(0x554fa8, 0), (0x554fac, 4), (0x554fae, 6)]),
    ("kCarArrayB", [(0x55500a, 0), (0x55500e, 4), (0x555010, 6)]),
    ("kCarArrayC", [(0x555138, 0), (0x55513c, 4)]),
    ("kCarArrayD", [(0x5551a2, 0), (0x5551a4, 2)]),
    ("kCarArrayE", [(0x55506c, 0)]),
    ("kCarArrayF", [(0x555214, 0)]),
]

ARRAY_INCLUDE = GAME_INCLUDE + '#include "car_arrays.h"\n'


def rename(text):
    """`text` with every array address the generated code names made the port's.

    Two spellings carry the address: `x86::reg32(5590952) /* 0x554fa8 */` for a
    memory operand, and the bare `5590952 /*0x554fa8*/` a `mov eax, imm32` is
    written as.  The generated comment is kept, saying what the address was.
    """
    for name, addresses in ARRAYS:
        for address, offset in addresses:
            value = name if offset == 0 else "%s + %d" % (name, offset)
            was = "port: car array, was 0x%x" % address
            text = text.replace("x86::reg32(%d) /* 0x%x */" % (address, address),
                                "x86::reg32(%s) /* %s */" % (value, was))
            text = text.replace("%d /*0x%x*/" % (address, address),
                                "%s /* %s */" % (value, was))
    return text


def add_include(text, eol, include, what):
    include = include.replace("\n", eol)
    if include in text:
        return text
    original = GAME_INCLUDE.replace("\n", eol)
    if not text.startswith(original):
        raise RuntimeError("%s: does not start with its own include" % what)
    return include + text[len(original):]


def apply(root: Path) -> None:
    # The section, so the addresses the arrays moved to are the machine's.
    path = root / DISASSEMBLY / CONSTRUCTOR_FILE
    text = path.read_text(newline="")
    eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"
    if LAST_SECTION_PORT not in text:
        if text.count(LAST_SECTION) != 1:
            raise RuntimeError("%s: the section list changed" % CONSTRUCTOR_FILE)
        text = text.replace(LAST_SECTION, LAST_SECTION_PORT, 1)
    text = add_include(text, eol, CONSTRUCTOR_INCLUDE, CONSTRUCTOR_FILE)
    path.write_text(text, newline="")

    # The arrays themselves.
    for file in sorted((root / DISASSEMBLY).glob("*.cpp")):
        text = file.read_text(newline="")
        out = rename(text)
        if out == text:
            continue
        eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"
        out = add_include(out, eol, ARRAY_INCLUDE, file.name)
        file.write_text(out, newline="")


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
