"""The port's changes to the race's effects, at the top of their generated functions; never patch input EXEs.

src/nfs3hp/native_effects.cpp: sub_42ac40, a car's collision effects every
frame -- sparks, dust, the sounds of a hit -- hooked as the vertex loops are
(tools/apply_native_vertices.py): a call at the top of the generated function,
which stays whole behind it.
"""
import sys
from pathlib import Path

HEADER = "// Port (tools/apply_native_effects.py): defined in native_effects.cpp.\n"

SITES = [
    ("nfs3hp", "sub_42ac40", "collisionEffects"),
    # The camera put into a view, before its matrix is made from it.
    ("nfs3hp", "sub_422850", "cameraToView"),
    # A particle made (its kind in eax): the dust, dirt, snow and sparks taken
    # over by the port's own (native_particles.cpp).
    ("nfs3hp", "sub_4ca850", "particleSpawn"),
]


def apply(root):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from tools.apply_native_vertices import apply as apply_natives
    apply_natives(root, sites=SITES, header=HEADER, marker="tools/apply_native_effects.py")


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
