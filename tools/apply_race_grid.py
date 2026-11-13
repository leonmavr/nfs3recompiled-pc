"""A full grid with traffic, and Hot Pursuit's grid; never patch input EXEs.

src/nfs3hp/native_ai.cpp (gridTraffic, pursuitGrid) has the why.  The game
counts a race's cars in sub_473bf0 (a single race) and sub_474640 (Hot
Pursuit), then makes them one after the other from the counts; each native
function is called where its count is settled and before the cars it counts
are made:

- sub_473bf0, 0x473e3e: the traffic counted (eight cars in all, none with a
  full grid), reached only with traffic on; the traffic made just after.
- sub_474640, 0x474b15: where every branch that picks the opponents, cops and
  traffic comes together, before the opponents are made.
"""
from pathlib import Path

HEADER = "// Port (tools/apply_race_grid.py): defined in native_ai.cpp.\n"

DECLARATION = "void gridTraffic(win32::WinApplication* app);\nvoid pursuitGrid(win32::WinApplication* app);\n"

XOR_ECX = "    cpu.ecx ^= x86::reg32(x86::sreg32(cpu.ecx));"

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.18.cpp", "00473e3e  31c9", XOR_ECX,
     "    gridTraffic(app); /* port: a full grid's traffic (tools/apply_race_grid.py) */\n" + XOR_ECX),
    ("nfs3hp.18.cpp", "00474b15  31c9", XOR_ECX,
     "    pursuitGrid(app); /* port: Hot Pursuit's grid (tools/apply_race_grid.py) */\n" + XOR_ECX),
]

NAMESPACE = "namespace nfs3hp\n{\n"


def apply(root):
    for name in dict.fromkeys(site[0] for site in SITES):
        path = root / "src/nfs3hp/disassembly" / name
        with path.open("r", newline="") as source:
            text = source.read()
        eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"

        def native(s):
            return eol.join(s.split("\n"))

        declaration = native(HEADER + DECLARATION)
        if declaration not in text:
            namespace = native(NAMESPACE)
            if text.count(namespace) != 1:
                raise RuntimeError("%s: namespace opening changed" % name)
            text = text.replace(namespace, namespace + declaration, 1)

        for _, instruction, old, new in (site for site in SITES if site[0] == name):
            old, new = native(old), native(new)
            anchor = "    // " + instruction + "  "
            start = text.find(anchor)
            if start < 0:
                raise RuntimeError("%s: no instruction %s" % (name, instruction))
            stop = text.find(eol + "L_0x", start + len(anchor))
            following = text.find(eol + "    // 00", start + len(anchor))
            if stop < 0 or (0 <= following < stop):
                stop = following
            if stop < 0:
                raise RuntimeError("%s: runaway block at %s" % (name, instruction))
            block = text[start:stop]
            if new in block:
                continue
            if block.count(old) != 1:
                raise RuntimeError("%s: unexpected shape at %s" % (name, instruction))
            text = text[:start] + block.replace(old, new, 1) + text[stop:]

        with path.open("w", newline="") as out:
            out.write(text)


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
