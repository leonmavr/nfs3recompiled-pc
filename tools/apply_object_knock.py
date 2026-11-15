"""Trees and lamp posts knocked down like the signs; never patch input EXEs.

src/nfs3hp/native_objects.cpp has the why.  sub_430da0, a car touching the
track's objects, asks sub_41d350(car eax, index edx) what to do at 0x430f02;
objectHitBefore is called just before (and may make the object a sign for
that call), objectHitAfter just after with its answer in eax.
"""
from pathlib import Path

HEADER = "// Port (tools/apply_object_knock.py): defined in native_objects.cpp.\n"

DECLARATION = ("void objectHitBefore(win32::WinApplication* app, x86::reg32 car, x86::reg32 index);\n"
               "void objectHitAfter(win32::WinApplication* app, x86::reg32 answer);\n")

CALL = ("    cpu.esp -= 4;\n"
        "    sub_41d350(app, cpu.sync());\n"
        "    cpu.reload();\n")

# (generated file, address and bytes of the instruction, original code, patched code)
SITES = [
    ("nfs3hp.7.cpp", "00430f02  e849c4feff",
     CALL,
     "    objectHitBefore(app, cpu.eax, cpu.edx); /* port: trees and lamp posts knocked down (tools/apply_object_knock.py) */\n"
     + CALL +
     "    objectHitAfter(app, cpu.eax); /* port (tools/apply_object_knock.py) */\n"),
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
