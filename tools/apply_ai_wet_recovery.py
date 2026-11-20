"""Beginner opponents recover from a slide as Expert ones do; never patch input EXEs.

src/nfs3hp/native_ai.cpp has the why (aiSlideSkill): sub_407fa0, an opponent
off its line or sliding, gives a racer a push back onto it every half second of
the race's clock -- its force times 10 and its turning times 50 -- unless the
skill level ([0x6fd4f0]) is Beginner (1), when it is times 1.1, next to
nothing.  In the rain or snow Beginner opponents slid, could not come back and
spun round -- and made quicker (NFS_AI_BEGINNER) they slide in the dry too.
The skill level that comparison reads comes from aiSlideSkill instead, which
says Expert for it in any weather.
"""
from pathlib import Path

HEADER = "// Port (tools/apply_ai_wet_recovery.py): defined in native_ai.cpp.\n"

DECLARATION = "x86::reg32 aiSlideSkill(win32::WinApplication* app);\n"

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.1.cpp", "00408459  833df0d46f0001",
     "        x86::reg32 tmp1 = app->getMemory<x86::reg32>(x86::reg32(7329008) /* 0x6fd4f0 */);",
     "        x86::reg32 tmp1 = aiSlideSkill(app); /* port: the skill level, Expert's recovery at Beginner (tools/apply_ai_wet_recovery.py) */"),
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
