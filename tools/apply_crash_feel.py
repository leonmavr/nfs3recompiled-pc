"""Crashes you feel: the crash's and the scrape's notes; never patch input EXEs.

src/nfs3hp/native_crash.cpp has the why.  sub_4182b0 starts one of the game's
sounds; at 0x4182dd its play block (ebp-0x14) has just been filled with
SND's defaults (sub_4e9b20) and the patch is in esi: crashNote sets the
block's note there for the crash and the scrape.  (The shake, the port's own
crash and the landing are called from native_effects.cpp and
native_surface.cpp.)
"""
from pathlib import Path

HEADER = "// Port (tools/apply_crash_feel.py): defined in native_crash.cpp.\n"

DECLARATION = "void crashNote(win32::WinApplication* app, x86::reg32 patch, x86::reg32 frame);\n"

MOV = "    cpu.al = app->getMemory<x86::reg8>(cpu.ebp + x86::reg32(-4) /* -0x4 */);"

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.3.cpp", "004182dd  8a45fc", MOV,
     "    crashNote(app, cpu.esi, cpu.ebp); /* port: a crash's note by the hit (tools/apply_crash_feel.py) */\n" + MOV),
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
