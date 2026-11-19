"""Tyre marks that last and look laid by a tyre; never patch input EXEs.

src/nfs3hp/native_skids.cpp has the why: the ring of tyre-mark blocks made
longer (its allocation in sub_4c70f0, its two bounds in sub_4c6cb0) and each
piece reshaded as it is laid (skidLaid, at sub_4c6cb0's end).
"""
from pathlib import Path

HEADER = "// Port (tools/apply_tyre_marks.py): defined in native_skids.cpp.\n"

DECLARATION = ("x86::reg32 tyreMarkBlocks();\n"
               "void skidLaid(win32::WinApplication* app);\n"
               "x86::reg32 markTint(win32::WinApplication* app, x86::sreg32 frame);\n")

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.27.cpp", "004c70f5  ba00d00000",
     "    cpu.edx = 53248 /*0xd000*/;",
     "    cpu.edx = tyreMarkBlocks() * 0x340; /* port: the ring's size (tools/apply_tyre_marks.py) */"),
    ("nfs3hp.27.cpp", "004c6dfc  83ff40",
     "        x86::reg32 tmp2 = x86::reg32(x86::sreg32(64 /*0x40*/));",
     "        x86::reg32 tmp2 = tyreMarkBlocks(); /* port (tools/apply_tyre_marks.py) */"),
    ("nfs3hp.27.cpp", "004c6e16  83f83f",
     "        x86::reg32 tmp2 = x86::reg32(x86::sreg32(63 /*0x3f*/));",
     "        x86::reg32 tmp2 = tyreMarkBlocks() - 1; /* port (tools/apply_tyre_marks.py) */"),
    ("nfs3hp.27.cpp", "004c6ee6  ff400c",
     "    (app->getMemory<x86::reg32>(cpu.eax + x86::reg32(12) /* 0xc */))++;",
     "    (app->getMemory<x86::reg32>(cpu.eax + x86::reg32(12) /* 0xc */))++;\n"
     "    skidLaid(app); /* port: a piece of a tyre mark reshaded (tools/apply_tyre_marks.py) */"),
    # The piece's picture byte: the low two bits are which skd picture, the
    # rest the ground's tint (skidLaid wrote it).
    ("nfs3hp.27.cpp", "004c704a  8a02",
     "    cpu.al = app->getMemory<x86::reg8>(cpu.edx);",
     "    cpu.al = app->getMemory<x86::reg8>(cpu.edx) & 3; /* port: the picture, not the tint (tools/apply_tyre_marks.py) */"),
    # The piece's colour: the ground's rut instead of white (the renderer
    # leaves the black picture's RGB out for these four).
    ("nfs3hp.27.cpp", "004c7068  81caffffff00",
     "    cpu.edx |= x86::reg32(x86::sreg32(16777215 /*0xffffff*/));",
     "    cpu.edx |= markTint(app, cpu.ebp); /* port: the rut's own colour (tools/apply_tyre_marks.py) */"),
    ("nfs3hp.27.cpp", "004c708a  81caffffff00",
     "    cpu.edx |= x86::reg32(x86::sreg32(16777215 /*0xffffff*/));",
     "    cpu.edx |= markTint(app, cpu.ebp); /* port: the rut's own colour (tools/apply_tyre_marks.py) */"),
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
