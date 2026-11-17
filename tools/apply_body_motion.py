"""The cars' bodies drawn leaning on their springs; never patch input EXEs.

src/nfs3hp/native_body.cpp has the why: each part of a car's model drawn by
sub_49de20 goes through bodyMotionPart first, which draws it through the
original with a leaned matrix and position for that call alone.
"""
from pathlib import Path

HEADER = "// Port (tools/apply_body_motion.py): defined in native_body.cpp.\n"

DECLARATION = "bool bodyMotionPart(win32::WinApplication* app, x86::CPU& cpu);\n"

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.23.cpp", "0049de20  56",
     "    app->getMemory<x86::reg32>(cpu.esp-4) = cpu.esi;",
     "    if (bodyMotionPart(app, cpu.sync())) /* port: a car's part drawn leaned (tools/apply_body_motion.py) */\n"
     "    {\n"
     "        cpu.reload();\n"
     "        return;\n"
     "    }\n"
     "    app->getMemory<x86::reg32>(cpu.esp-4) = cpu.esi;"),
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
