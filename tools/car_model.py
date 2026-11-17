"""Look at a car's model, export it, or give a car a different one.

A car's body is `car.fce`: the mesh, its parts and its paint colours.  The
format is EA's NFS3 FCE, and it is documented -- TheXDS/VivLib carries the
specification (`docs/FileFormats/FCE-specs.md`) -- with every table offset
relative to the end of a fixed 0x1f04-byte header:

    header
      0x0004  triangles, 0x0008 vertices, 0x000c texture pages
      0x0010  vertex table      (12 bytes each: x, y, z as floats, part-local)
      0x0014  normals table     (12 bytes each, one per vertex)
      0x0018  triangles table   (56 bytes each, indices local to the part)
      0x001c  reserved #1 (32 bytes a vertex), #2 and #3 (12 bytes a vertex)
      0x0028  half the model's size, x y z as floats
      0x00f8  parts; 0x00fc each part's origin, 0x03fc/0x04fc its first vertex
              and vertex count, 0x05fc/0x06fc its first triangle and count
      0x07fc  primary colours, 0x0900 secondary (16 each, hue/saturation/
              brightness/alpha as int32)
      0x0e04  part names, 64 bytes each
    ...and after the header, the tables, in the order the offsets give.

A triangle is texture page, three vertex indices, a flags dword and three (u, v)
pairs.  The parts are what the model is made of and they are named -- the
"hi body" and its four wheels, then "medium", "small" and "tiny" bodies, the
game's levels of detail.

    python3 tools/car_model.py info hgts
    python3 tools/car_model.py export hgts --out /tmp
    python3 tools/car_model.py import mycar mycar.obj
    python3 tools/car_model.py swap hgts mycar --from diab
    python3 tools/car_model.py exhausts [hgts]       # mark the exhausts (car_exhausts.py)

`export` writes an .obj (and .mtl) to look at or edit in Blender; `import` is
the other way and is the one that makes a car of your own -- see below; `swap`
puts another car's body -- its `car.fce`, and with `--textures` its paint
textures, `--dashboard` its dashboard and `--showcase` its picture -- into a
car's archive and beside it, which is how a car gets a body nobody has modelled
for it yet: the physics stay yours, the shell is someone else's.

Writing a brand new FCE is `import`, and nothing in the format stood in its way
after all.  Everything the game draws from is in the tables above; the three
reserved areas are stale scratch.  #1 is the 32 bytes a vertex the renderer
works in (sub_4bf4c0 writes x, y, z and 1/z over it and sub_49cfa0 zeroes its
+0x14), so a retail file holds the *screen* coordinates of the last frame that
drew the car; #2 and #3 are 12 bytes a vertex nothing reads.  Zeroing all three
in a retail `car.fce` still renders and races without a flaw, so `import`
leaves them zero.  `info --reserved` prints what a retail file puts there.

What a mesh still has to bring is what the game cannot work out for itself:

  - parts, one per `o`/`g` group.  The first is the body.  A group whose name
    says which wheel it is ("wheel_FL", "front left wheel") becomes one of the
    names the game knows -- "high left front wheel" -- because those are the
    parts it steers and rolls; anything else keeps its own name.
  - four levels of detail.  The game picks one by distance, so a mesh with only
    the near one would be a car that is drawn at one distance.  `--lods dup`
    (the default) gives the missing levels the near one's geometry again, which
    costs triangles and never shows the wrong thing; `--lods single` leaves
    them out.
  - (u, v) per corner, into the car's one texture page.  That texture is the
    car's own `fedata.fsh`, so a new body either paints itself with the retail
    car's layout or ships a new `fedata.fsh`.  A corner without a `vt` takes
    `--uv`, one texel of that texture.
  - the paint stays the car's: `import` keeps the palette, the headlight and
    brake light positions and the corners of the header nothing has documented
    from the model it replaces (`--base` takes them from another car).

The round trip is exact: `export hgts --lod all` then `import` of that .obj
gives back 821 vertices, 1104 triangles and a car that renders identically.
"""
import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from car_perf import CARMODEL, CarFileError, bigf_entries, find_car, read_carp_text, replace_archive_entry

HEADER_SIZE = 0x1F04
VERTEX_SIZE = 12
NORMAL_SIZE = 12
TRIANGLE_SIZE = 0x38
MAX_PARTS = 64

# material flags, from the spec's MaterialFlags enum
FLAGS = {
    0x00: "none", 0x01: "no blending", 0x02: "high blending", 0x04: "no culling",
    0x08: "semitrans", 0x09: "semitrans, no blending", 0x0A: "semitrans, high blending",
    0x05: "no culling, no blending", 0x06: "no culling, high blending",
    0x0C: "no culling, semitrans", 0x0D: "no culling, semitrans, no blending",
    0x0E: "no culling, semitrans, high blending",
}


@dataclass
class Part:
    index: int
    name: str
    origin: tuple
    first_vertex: int
    vertices: int
    first_triangle: int
    triangles: int
    lod: str


@dataclass
class Model:
    triangles: list = field(default_factory=list)   # (page, i0, i1, i2, flags, (u0,v0), (u1,v1), (u2,v2))
    vertices: list = field(default_factory=list)    # (x, y, z)
    normals: list = field(default_factory=list)
    parts: list = field(default_factory=list)
    half_size: tuple = (0.0, 0.0, 0.0)
    colours: list = field(default_factory=list)     # primary palette, (h, s, b, a)
    secondary: list = field(default_factory=list)
    pages: int = 1
    reserved: list = field(default_factory=list)    # (name, offset, bytes)
    source: str = ""

    def vertex(self, part, index):
        """A vertex of a part, moved to the car's own space by the part's origin."""
        local = self.vertices[index]
        origin = part.origin
        return (local[0] + origin[0], local[1] + origin[1], local[2] + origin[2])

    def bounds(self):
        points = [self.vertex(part, part.first_vertex + i)
                  for part in self.parts for i in range(part.vertices)
                  if part.first_vertex + i < len(self.vertices)] or self.vertices
        if not points:
            return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
        xs, ys, zs = zip(*points)
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def _u32(data, at):
    return struct.unpack_from("<I", data, at)[0]


def _f32(data, at):
    return struct.unpack_from("<f", data, at)[0]


def _text(data, at, width):
    raw = data[at:at + width].split(b"\0")[0]
    return raw.decode("latin-1", "replace").strip()


def parse_fce(data, source=""):
    """A `Model` out of an FCE's bytes."""
    if len(data) < HEADER_SIZE:
        raise CarFileError("%s: too short for an FCE header" % (source or "fce"))
    count = _u32(data, 0x04)
    vertices = _u32(data, 0x08)
    model = Model(source=source)
    model.pages = max(1, _u32(data, 0x0C))
    model.half_size = (_f32(data, 0x28), _f32(data, 0x2C), _f32(data, 0x30))

    vertex_at = HEADER_SIZE + _u32(data, 0x10)
    normal_at = HEADER_SIZE + _u32(data, 0x14)
    triangle_at = HEADER_SIZE + _u32(data, 0x18)
    model.vertices = [struct.unpack_from("<3f", data, vertex_at + i * VERTEX_SIZE)
                      for i in range(vertices)]
    if normal_at + vertices * NORMAL_SIZE <= len(data):
        model.normals = [struct.unpack_from("<3f", data, normal_at + i * NORMAL_SIZE)
                         for i in range(vertices)]
    for i in range(count):
        at = triangle_at + i * TRIANGLE_SIZE
        page, i0, i1, i2 = struct.unpack_from("<4I", data, at)
        flags = _u32(data, at + 0x1C)
        uv = struct.unpack_from("<6f", data, at + 0x20)
        model.triangles.append((page, i0, i1, i2, flags,
                                (uv[0], uv[3]), (uv[1], uv[4]), (uv[2], uv[5])))

    parts = _u32(data, 0xF8)
    for p in range(min(parts, MAX_PARTS)):
        origin = struct.unpack_from("<3f", data, 0x00FC + p * 12)
        name = _text(data, 0x0E04 + p * 64, 64)
        model.parts.append(Part(p, name, origin,
                                _u32(data, 0x03FC + p * 4), _u32(data, 0x04FC + p * 4),
                                _u32(data, 0x05FC + p * 4), _u32(data, 0x06FC + p * 4),
                                "?"))
    assign_lods(model.parts)
    for table, at in (("primary", 0x0800), ("secondary", 0x0904)):
        entries = []
        for c in range(min(_u32(data, at - 4), 16)):
            entries.append(struct.unpack_from("<4I", data, at + c * 16))
        if table == "primary":
            model.colours = entries
        else:
            model.secondary = entries
    for name, header_at, stride in (("reserved #1", 0x1C, 0x20), ("reserved #2", 0x20, 0x0C),
                                    ("reserved #3", 0x24, 0x0C)):
        offset = _u32(data, header_at)
        if offset:
            start = HEADER_SIZE + offset
            model.reserved.append((name, offset, data[start:start + vertices * stride]))
    return model


def assign_lods(parts):
    """Mark each part with its level of detail.

    The parts come in LOD order -- the high-detail body and its wheels, then
    medium, small, tiny -- and only some of them are named for it: the HSV
    calls its wheels "high left front wheel", the Diablo just "left front
    wheel", and both end with something like "high headlights" that belongs to
    the high group but sits after the rest.
    """
    current = "high"
    for part in parts:
        low = part.name.lower()
        for tag in ("medium", "small", "tiny"):
            if low.startswith(tag):
                current = tag
        part.lod = "high" if low.startswith(("hi ", "high")) else current
    return parts


def lod_parts(model, lod):
    """The parts of one level of detail (all of them when lod is None)."""
    if not lod or lod == "all":
        return list(model.parts)
    return [part for part in model.parts if part.lod == lod]


# ------------------------------------------------------------------ reading

def car_archive(car):
    return car.folder / "car.viv"


def model_bytes(folder, wanted="car.fce"):
    """(bytes, from_archive) for a file of the car -- the archive first."""
    folder = Path(folder)
    archive = folder / "car.viv"
    if archive.exists():
        data = archive.read_bytes()
        try:
            for name, offset, size in bigf_entries(data):
                if name.lower() == wanted.lower():
                    return data[offset:offset + size], True
        except (CarFileError, struct.error, ValueError):
            pass
    loose = folder / wanted
    for candidate in (loose, folder / wanted.upper()):
        if candidate.exists():
            return candidate.read_bytes(), False
    raise CarFileError("%s: no %s" % (folder, wanted))


def load_model(car, wanted="car.fce"):
    data, from_archive = model_bytes(car.folder, wanted)
    return parse_fce(data, "%s/%s" % (car.key, wanted)), from_archive


# ------------------------------------------------------------------ writing

def page_colours(model):
    """The colour of each of the model's texture pages -- the car's paint, out
    of its own palette (a page a colour, hue/saturation/brightness each) -- or
    None where the file carries no palette.  The rule the OBJ export's materials
    and the exhaust marker's colour switch both take."""
    if not model.colours:
        return None
    out = []
    for page in range(model.pages):
        entry = model.colours[min(page, len(model.colours) - 1)]
        out.append(_hsb_to_rgb(entry[0], entry[1], entry[2]))
    return out


def _hsb_to_rgb(hue, saturation, bright):
    h = (hue / 255.0) * 6.0
    s = saturation / 255.0
    v = bright / 255.0
    sector = int(math.floor(h))
    frac = h - sector
    p = v * (1.0 - s)
    q = v * (1.0 - s * frac)
    t = v * (1.0 - s * (1.0 - frac))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][sector % 6]
    return (r, g, b)


def export_obj(car, model, parts, out_dir, up="z"):
    """Write `<car>.obj` and `.mtl` for the chosen parts."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    obj_path = out_dir / ("%s.obj" % car.key)
    mtl_path = out_dir / ("%s.mtl" % car.key)

    def point(vertex):
        x, y, z = vertex
        if up == "z":                       # Blender: Z up, Y forward
            return (x, -z, y)
        return (x, y, z)

    lines = ["# %s -- %s" % (car.key, car.display_name() or car.key),
             "# %d parts, %d vertices, %d triangles"
             % (len(parts), sum(p.vertices for p in parts), sum(p.triangles for p in parts)),
             "# exported by tools/car_model.py", "mtllib %s" % mtl_path.name, ""]
    for part in model.parts:
        for i in range(part.vertices):
            index = part.first_vertex + i
            if index < len(model.vertices):
                lines.append("v %.5f %.5f %.5f" % point(model.vertex(part, index)))
    if model.normals:
        for normal in model.normals:
            lines.append("vn %.4f %.4f %.4f" % point(normal))

    uvs = {}
    uv_lines = []
    faces = []
    for part in parts:
        faces.append(("o %s" % (part.name.replace(" ", "_") or ("part%d" % part.index)),
                      part, part.first_triangle, part.triangles))
    for name_line, part, first, count in faces:
        lines.append(name_line)
        emitted = set()
        for index in range(first, min(first + count, len(model.triangles))):
            page, i0, i1, i2, flags, *corners = model.triangles[index]
            material = "page%d" % page
            if material not in emitted:
                lines.append("usemtl %s" % material)
                emitted.add(material)
            refs = []
            for corner, vertex_index in zip(corners, (i0, i1, i2)):
                key = (round(corner[0], 5), round(corner[1], 5))
                slot = uvs.get(key)
                if slot is None:
                    slot = len(uv_lines) + 1
                    uvs[key] = slot
                    uv_lines.append("vt %.5f %.5f" % (corner[0], corner[1]))
                refs.append(slot)
            g0, g1, g2 = (i0 + part.first_vertex, i1 + part.first_vertex,
                          i2 + part.first_vertex)
            if model.normals:
                lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d"
                             % (g0 + 1, refs[0], g0 + 1, g1 + 1, refs[1], g1 + 1,
                                g2 + 1, refs[2], g2 + 1))
            else:
                lines.append("f %d/%d %d/%d %d/%d" % (g0 + 1, refs[0], g1 + 1, refs[1],
                                                      g2 + 1, refs[2]))
    # texture coordinates come after the faces' indices are known, but OBJ wants
    # them before; rebuild the file with them in place.
    head = [line for line in lines if line.startswith(("v ", "vn "))]
    body = [line for line in lines if line.startswith(("f ", "o ", "usemtl", "#", "mtllib"))]
    obj_path.write_text("\n".join(head + uv_lines + [""] + body) + "\n")

    materials = ["# %s" % car.key, ""]
    paints = page_colours(model) or [(0.7, 0.7, 0.72)] * model.pages
    for page, colour in enumerate(paints):
        materials += ["newmtl page%d" % page, "Kd %.4f %.4f %.4f" % colour,
                      "Ka 0.1 0.1 0.1", "d 1.0", "illum 2", ""]
    mtl_path.write_text("\n".join(materials))
    return obj_path, mtl_path


def swap_files(car, donor, what):
    """Put the donor's files into the car's archive and beside it."""
    done = []
    for name in what:
        try:
            payload, _ = model_bytes(donor.folder, name)
        except CarFileError:
            continue
        mode = replace_archive_entry(car_archive(car), name, payload)
        (car.folder / name).write_bytes(payload)
        done.append((name, mode, len(payload)))
    return done


def preview(car, model, parts, out=None, show=True):
    """Draw the mesh with matplotlib -- no textures, shaded so the shape reads."""
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    palette = [(0.72, 0.16, 0.18), (0.16, 0.42, 0.78), (0.20, 0.60, 0.30), (0.85, 0.62, 0.10),
               (0.55, 0.30, 0.70), (0.30, 0.65, 0.68), (0.70, 0.40, 0.20), (0.42, 0.42, 0.46)]

    def to_view(vertex):
        x, y, z = vertex
        return (x, -z, y)          # Blender's axes: Z up, Y forward

    light = (0.45, -0.55, 0.70)
    length = math.sqrt(sum(c * c for c in light))
    light = tuple(c / length for c in light)

    faces, colours = [], []
    for part in parts:
        base = palette[part.index % len(palette)]
        for index in range(part.first_triangle,
                           min(part.first_triangle + part.triangles, len(model.triangles))):
            _page, i0, i1, i2 = model.triangles[index][:4]
            try:
                corners = [to_view(model.vertex(part, i0 + part.first_vertex)),
                           to_view(model.vertex(part, i1 + part.first_vertex)),
                           to_view(model.vertex(part, i2 + part.first_vertex))]
            except IndexError:
                continue
            normal = [0.0, 0.0, 0.0]
            for k in range(3):
                x1, y1, z1 = corners[k]
                x2, y2, z2 = corners[(k + 1) % 3]
                normal[0] += (y1 - y2) * (z1 + z2)
                normal[1] += (z1 - z2) * (x1 + x2)
                normal[2] += (x1 - x2) * (y1 + y2)
            size = math.sqrt(sum(c * c for c in normal))
            if size:
                normal = [c / size for c in normal]
            shade = 0.42 + 0.58 * abs(sum(normal[k] * light[k] for k in range(3)))
            faces.append(corners)
            colours.append(tuple(min(1.0, c * shade) for c in base))

    figure = plt.figure(figsize=(12.0, 5.4))
    low, high = model.bounds()
    centre = [(low[i] + high[i]) / 2.0 for i in range(3)]
    radius = max(high[i] - low[i] for i in range(3)) / 2.0 * 1.02
    centre = to_view(centre)
    views = (("three-quarter", 18, 35), ("side", 2, 0))
    for slot, (label, elev, azim) in enumerate(views, start=1):
        ax = figure.add_subplot(1, 2, slot, projection="3d")
        patch = Poly3DCollection(faces, facecolors=colours, edgecolor=(0, 0, 0, 0.10),
                                 linewidths=0.12)
        patch.set_alpha(0.90)
        ax.add_collection3d(patch)
        ax.set_xlim(centre[0] - radius, centre[0] + radius)
        ax.set_ylim(centre[1] - radius, centre[1] + radius)
        ax.set_zlim(centre[2] - radius, centre[2] + radius)
        ax.set_box_aspect((1, 1, 1), zoom=1.55)
        ax.set_proj_type("ortho")
        ax.set_axis_off()
        ax.view_init(elev=elev, azim=azim)
        ax.set_title(label, fontsize=9, color="#666666")
    figure.suptitle("%s   --   %s   (%d parts, %d triangles)"
                    % (car.display_name() or car.key, car.key, len(parts),
                       sum(p.triangles for p in parts)), fontsize=12)
    figure.subplots_adjust(left=0.01, right=0.99, bottom=0.01, top=0.88)
    if out:
        figure.savefig(out, dpi=110, bbox_inches="tight", pad_inches=0.1)
        print("wrote %s" % out)
    if show:
        plt.show()
    plt.close(figure)


# ------------------------------------------------------- writing a model

LOD_TAGS = ("high", "medium", "small", "tiny")


def _wheel_name(group):
    """The game's name for a wheel group, or None if the group is not one.

    The game finds a car's wheels by the names its own models use -- "high left
    front wheel" and its three siblings -- and steers and rolls those parts, so
    a group called "wheel_FL_32" has to come out as one of them or the wheel is
    a body panel that never turns."""
    low = " " + group.lower().replace("_", " ").replace("-", " ") + " "
    if not any(word in low for word in (" wheel ", " tyre ", " tire ")):
        return None
    side = ("left" if any(word in low for word in (" left ", " lf ", " fl ", " l "))
            else "right" if any(word in low for word in (" right ", " rf ", " fr ", " r ")) else None)
    end = "front" if " front " in low else "rear" if (" rear " in low or " back " in low) else None
    return "%s %s wheel" % (side, end) if side and end else None


def part_names(groups):
    """The names the parts get, in order: the first is the body."""
    names = []
    for index, group in enumerate(groups):
        name = _wheel_name(group)
        if not name:
            name = "body" if index == 0 else (group.strip() or ("part %d" % index))
        name = name.replace("_", " ").strip()[:63]
        if not name.lower().startswith(LOD_TAGS):
            name = "high " + name
        names.append(name)
    return names


def read_obj(path):
    """(positions, uvs, normals, groups) from an .obj, its faces fan-cut.

    A group is what `o` or `g` names; a file that names none is one part.  A
    corner is (position, uv, normal) indices into the file's own lists, the
    last two None where the face does not give them."""
    positions, uvs, normals = [], [], []
    groups = []
    current = None
    for number, line in enumerate(Path(path).read_text(errors="replace").splitlines(), start=1):
        line = line.strip()
        if not line or line[0] == "#":
            continue
        head, _, rest = line.partition(" ")
        fields = rest.split()
        try:
            if head == "v":
                positions.append(tuple(float(c) for c in fields[:3]))
            elif head == "vt":
                uvs.append((float(fields[0]), float(fields[1]) if len(fields) > 1 else 0.0))
            elif head == "vn":
                normals.append(tuple(float(c) for c in fields[:3]))
            elif head in ("o", "g"):
                current = {"name": rest.strip(), "triangles": []}
                groups.append(current)
            elif head == "f":
                if current is None:
                    current = {"name": "", "triangles": []}
                    groups.append(current)
                corners = []
                for item in fields:
                    bits = item.split("/")

                    def index(text, count):
                        if not text:
                            return None
                        value = int(text)
                        return value - 1 if value > 0 else count + value

                    corners.append((index(bits[0], len(positions)),
                                    index(bits[1], len(uvs)) if len(bits) > 1 else None,
                                    index(bits[2], len(normals)) if len(bits) > 2 else None))
                for i in range(1, len(corners) - 1):
                    current["triangles"].append((corners[0], corners[i], corners[i + 1]))
        except (ValueError, IndexError):
            raise CarFileError("%s line %d: not an .obj line: %s" % (path, number, line))
    groups = [group for group in groups if group["triangles"]]
    if not groups:
        raise CarFileError("%s: no faces in it" % path)
    return positions, uvs, normals, groups


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(vector):
    length = math.sqrt(sum(c * c for c in vector))
    return tuple(c / length for c in vector) if length else (0.0, 0.0, 1.0)


def fce_from_obj(positions, uvs, normals, groups, header, up="y", scale=1.0, lods="dup",
                 flat_uv=(0.5, 0.5)):
    """The bytes of a car.fce made of the groups, over `header`\'s own fields.

    `header` is a model\'s first 0x1f04 bytes: what a mesh cannot say -- the
    paint palettes, the headlight and brake light positions, the areas nothing
    has documented -- is kept from it, and everything the mesh does decide is
    written over it."""
    # The game's own axes are Y up and +Z forward (the retail cars' headlights
    # are at +Z).  Blender's .obj export is Y up with the model facing -Z, and
    # what `export` writes is Z up, so both are turned on the way in.
    turn = (lambda x, y, z: (x, y, -z)) if up == "y" else (lambda x, y, z: (x, z, -y))

    def point(position):
        return turn(*(c * scale for c in position))

    model_vertices, model_normals, model_triangles = [], [], []
    parts = []
    names = part_names([group["name"] for group in groups])
    for name, group in zip(names, groups):
        corners = [point(positions[corner[0]]) for triangle in group["triangles"] for corner in triangle]
        low = tuple(min(c[i] for c in corners) for i in range(3))
        high = tuple(max(c[i] for c in corners) for i in range(3))
        origin = tuple((low[i] + high[i]) / 2.0 for i in range(3))   # a wheel turns about this

        local = {}
        first_vertex, first_triangle = len(model_vertices), len(model_triangles)
        for triangle in group["triangles"]:
            indices, corner_uvs = [], []
            for position_index, uv_index, normal_index in triangle:
                # an FCE's (u, v) belong to the corner, so a vertex is split only
                # where one position carries two normals
                key = (position_index, normal_index)
                if key not in local:
                    local[key] = len(model_vertices)
                    position = point(positions[position_index])
                    model_vertices.append(tuple(position[i] - origin[i] for i in range(3)))
                    if normal_index is not None and normal_index < len(normals):
                        model_normals.append(_unit(turn(*normals[normal_index])))
                    else:
                        model_normals.append(None)      # the face's, below
                indices.append(local[key])
                corner_uvs.append(uvs[uv_index] if uv_index is not None and uv_index < len(uvs)
                                  else flat_uv)
            a, b, c = (model_vertices[i] for i in indices)
            face = _unit(_cross(tuple(b[i] - a[i] for i in range(3)),
                                tuple(c[i] - a[i] for i in range(3))))
            for slot in indices:
                if model_normals[slot] is None:
                    model_normals[slot] = face
            model_triangles.append((indices, corner_uvs))
        parts.append((name, origin, first_vertex, len(model_vertices) - first_vertex,
                      first_triangle, len(model_triangles) - first_triangle))

    def copy_part(source, name):
        """The same geometry again under another name, for a level of detail the
        mesh does not carry of its own."""
        _n, origin, first, count, first_triangle, triangle_count = parts[source]
        new_first, new_triangle = len(model_vertices), len(model_triangles)
        model_vertices.extend(model_vertices[first:first + count])
        model_normals.extend(model_normals[first:first + count])
        for index in range(first_triangle, first_triangle + triangle_count):
            indices, corners = model_triangles[index]
            model_triangles.append(([i - first + new_first for i in indices], list(corners)))
        parts.append((name, origin, new_first, count, new_triangle, triangle_count))

    if lods == "dup":
        # Every car the game ships has four levels of detail, and the game picks
        # one by distance; a mesh with only the near one is a car that can be
        # drawn at one distance only.  Rather than lose it at the others, the
        # missing levels are the near one again, which costs triangles but never
        # shows the wrong thing.  A mesh that brings its own levels keeps them.
        have = {part[0].split(" ")[0] for part in parts}
        wheels = [index for index, part in enumerate(parts) if "wheel" in part[0].split(" ", 1)[-1]]
        for tag in ("medium", "small", "tiny"):
            if tag in have:
                continue
            copy_part(0, tag + " " + parts[0][0].split(" ", 1)[-1])
            if tag == "medium":
                for index in wheels:
                    copy_part(index, tag + " " + parts[index][0].split(" ", 1)[-1])
    return _pack_fce(header, model_vertices, model_normals, model_triangles, parts)


def blank_header(dummies=(), colours=(), secondary=()):
    """A header with nothing in it, for a car that has no model of its own."""
    header = bytearray(HEADER_SIZE)
    for index, pair in enumerate(dummies[:8]):
        struct.pack_into("<6f", header, 0x38 + index * 24, *pair)
    struct.pack_into("<I", header, 0x34, len(dummies[:8]))
    struct.pack_into("<I", header, 0x0F8, 0)
    for count_at, at, entries in ((0x7FC, 0x800, colours), (0x900, 0x904, secondary)):
        struct.pack_into("<I", header, count_at, min(len(entries), 16))
        for slot, entry in enumerate(entries[:16]):
            struct.pack_into("<4I", header, at + slot * 16, *entry)
    return bytes(header)


def _pack_fce(header, vertices, normals, triangles, parts):
    """The file: the header, then the tables in the order the header points at.

    Vertices are 12 bytes (part-local: the part's origin puts them back),
    normals 12, triangles 0x38 -- texture page, three indices local to the
    part, the flags, then the three (u, v) pairs -- and the three reserved
    areas the format leaves undocumented, 32 bytes a vertex and then 12 and 12,
    which the renderer works in and writes over: zero here.

    What a mesh cannot say is kept from `header` -- either the model the car has
    now or blank_header(): the paint palettes, the headlight positions, and
    whatever the format's unknown corners hold in a file the game already
    draws."""
    if header is None or len(header) < HEADER_SIZE:
        raise CarFileError("no 0x1f04-byte header to build on")
    if len(parts) > MAX_PARTS:
        raise CarFileError("%d parts, and a model holds %d" % (len(parts), MAX_PARTS))

    header = bytearray(header)
    data = bytearray()
    offsets = {}

    def place(table, table_bytes):
        offsets[table] = len(data)
        data.extend(table_bytes)

    place("vertices", b"".join(struct.pack("<3f", *vertex) for vertex in vertices))
    place("normals", b"".join(struct.pack("<3f", *normal) for normal in normals))
    triangle_bytes = bytearray()
    for _name, _origin, first, _count, first_triangle, triangle_count in parts:
        for index in range(first_triangle, first_triangle + triangle_count):
            indices, corners = triangles[index]
            # indices are the part's own, not the model's: the game adds the
            # part's first vertex to them
            triangle_bytes.extend(struct.pack("<4I", 0, indices[0] - first, indices[1] - first,
                                              indices[2] - first))
            # 12 bytes the format leaves unexplained, 0xff00ff00 a corner in
            # every retail triangle but some of the bonus car's, which are grey
            triangle_bytes.extend(b"\x00\xff" * 6)
            triangle_bytes.extend(struct.pack("<I", 0))                 # flags
            triangle_bytes.extend(struct.pack("<6f", corners[0][0], corners[1][0], corners[2][0],
                                             corners[0][1], corners[1][1], corners[2][1]))
    place("triangles", bytes(triangle_bytes))
    place("reserved #1", bytes(len(vertices) * 0x20))
    place("reserved #2", bytes(len(vertices) * 12))
    place("reserved #3", bytes(len(vertices) * 12))

    low = [math.inf] * 3
    high = [-math.inf] * 3
    for _name, origin, first, count, _first_triangle, _count in parts:
        for index in range(first, first + count):
            for axis in range(3):
                value = vertices[index][axis] + origin[axis]
                low[axis] = min(low[axis], value)
                high[axis] = max(high[axis], value)

    struct.pack_into("<I", header, 0x04, len(triangles))
    struct.pack_into("<I", header, 0x08, len(vertices))
    struct.pack_into("<I", header, 0x0C, 1)                              # one texture page
    struct.pack_into("<I", header, 0x10, offsets["vertices"])
    struct.pack_into("<I", header, 0x14, offsets["normals"])
    struct.pack_into("<I", header, 0x18, offsets["triangles"])
    struct.pack_into("<I", header, 0x1C, offsets["reserved #1"])
    struct.pack_into("<I", header, 0x20, offsets["reserved #2"])
    struct.pack_into("<I", header, 0x24, offsets["reserved #3"])
    struct.pack_into("<3f", header, 0x28, *[(high[axis] - low[axis]) / 2.0 if vertices else 0.0
                                           for axis in range(3)])
    struct.pack_into("<I", header, 0x0F8, len(parts))

    # the part tables and the names are the model's; the palettes between stay
    for at, size in ((0x0FC, 0x7FC - 0x0FC), (0x0E04, HEADER_SIZE - 0x0E04)):
        header[at:at + size] = bytes(size)
    for index, (name, origin, first, count, first_triangle, triangle_count) in enumerate(parts):
        struct.pack_into("<3f", header, 0x0FC + index * 12, *origin)
        struct.pack_into("<I", header, 0x3FC + index * 4, first)
        struct.pack_into("<I", header, 0x4FC + index * 4, count)
        struct.pack_into("<I", header, 0x5FC + index * 4, first_triangle)
        struct.pack_into("<I", header, 0x6FC + index * 4, triangle_count)
        raw = name.encode("latin-1", "replace")[:64]
        header[0x0E04 + index * 64:0x0E04 + index * 64 + len(raw)] = raw
    return bytes(header) + bytes(data)




# --------------------------------------------------------------------- cli

def _print_info(car, model, reserved):
    low, high = model.bounds()
    print("%s  (%s)" % (car.display_name() or car.key, car.key))
    print("  model %s: %d triangles, %d vertices, %d texture page%s, %d parts"
          % (model.source, len(model.triangles), len(model.vertices), model.pages,
             "" if model.pages == 1 else "s", len(model.parts)))
    print("  size  %.3f x %.3f x %.3f m (the header says half %.3f %.3f %.3f)"
          % (high[0] - low[0], high[1] - low[1], high[2] - low[2],
             model.half_size[0], model.half_size[1], model.half_size[2]))
    print("  bounds x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f"
          % (low[0], high[0], low[1], high[1], low[2], high[2]))
    if model.colours:
        swatches = []
        for entry in model.colours[:6]:
            red, green, blue = _hsb_to_rgb(entry[0], entry[1], entry[2])
            swatches.append("#%02x%02x%02x" % (int(red * 255), int(green * 255), int(blue * 255)))
        print("  paint  %d primary colours: %s" % (len(model.colours), " ".join(swatches)))
    print("  parts:")
    for part in model.parts:
        print("    %2d  %-22s %-6s origin %6.2f %6.2f %6.2f   %4d verts %4d tris"
              % (part.index, part.name or "-", part.lod, part.origin[0], part.origin[1],
                 part.origin[2], part.vertices, part.triangles))
    pages = {}
    for page, *_rest in model.triangles:
        pages[page] = pages.get(page, 0) + 1
    print("  triangles by texture page: %s"
          % ", ".join("page %d: %d" % (page, count) for page, count in sorted(pages.items())))
    if reserved:
        for name, offset, data in model.reserved:
            shown = data[:24].hex(" ")
            nonzero = sum(1 for byte in data if byte)
            print("  %s at 0x%x: %d bytes, %d non-zero, first: %s"
                  % (name, offset, len(data), nonzero, shown))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Inspect a car's model, export it as OBJ, build one from an OBJ, "
                    "or give a car a different body.")
    parser.add_argument("--root", default=str(CARMODEL),
                        help="the carmodel folder (default: the repository's, wherever this is run)")
    sub = parser.add_subparsers(dest="command", required=True)

    info = sub.add_parser("info", help="the mesh, its parts, its paint")
    info.add_argument("car")
    info.add_argument("--reserved", action="store_true",
                      help="also show the three areas the specification leaves undocumented")

    export = sub.add_parser("export", help="write an OBJ (and MTL) to look at in Blender")
    export.add_argument("car")
    export.add_argument("--out", default=".")
    export.add_argument("--lod", default="all",
                        help="high, medium, small, tiny or all (default: all)")
    export.add_argument("--up", default="z", choices=("z", "game"),
                        help="z for Blender (default), game for the file's own axes")

    draw = sub.add_parser("preview", help="draw the mesh as a picture, no textures")
    draw.add_argument("car")
    draw.add_argument("--out")
    draw.add_argument("--lod", default="high")
    draw.add_argument("--no-window", action="store_true")

    load = sub.add_parser("import", help="build a car.fce from an .obj and give it to a car")
    load.add_argument("car")
    load.add_argument("obj")
    load.add_argument("--base", help="the car whose header is kept (paint, lights, the unknown "
                                     "corners); default: the car's own model")
    load.add_argument("--up", default="y", choices=("y", "z"),
                      help="the .obj's up axis: y for Blender's default (default), z if the mesh "
                           "is already Z up")
    load.add_argument("--scale", type=float, default=1.0, help="metres per mesh unit (default 1)")
    load.add_argument("--uv", default="0.5,0.5",
                      help="the (u, v) a corner gets when the .obj has none (default 0.5,0.5, "
                           "which is one texel of the car's own texture)")
    load.add_argument("--lods", default="dup", choices=("dup", "single"),
                      help="dup (default) gives the mesh the medium, small and tiny levels it "
                           "does not carry, so the game can draw it at any distance; single keeps "
                           "only the parts the .obj has")
    load.add_argument("--dry-run", action="store_true")

    swap = sub.add_parser("swap", help="take another car's body, textures or dashboard")
    swap.add_argument("car")
    swap.add_argument("--from", dest="donor", required=True)
    swap.add_argument("--textures", action="store_true", help="fedata.fsh too (the paint)")
    swap.add_argument("--dashboard", action="store_true", help="dash.qfs too")
    swap.add_argument("--showcase", action="store_true", help="car00.tga too")
    swap.add_argument("--dry-run", action="store_true")

    marks = sub.add_parser("exhausts", help="a window to mark where the car's exhausts are, "
                                            "kept as exhausts.json in its folder")
    marks.add_argument("car", nargs="?", help="the car to open on (default: the first)")

    args = parser.parse_args(argv)
    if args.command == "exhausts":
        import car_exhausts
        car_exhausts.run(args.root, args.car)
        return 0
    try:
        if args.command == "info":
            car = find_car(args.car, args.root)
            model, from_archive = load_model(car)
            _print_info(car, model, args.reserved)
            print("  (read from %s)" % ("car.viv" if from_archive else "the loose car.fce"))
            return 0

        if args.command == "export":
            car = find_car(args.car, args.root)
            model, _ = load_model(car)
            parts = lod_parts(model, args.lod)
            if not parts:
                raise CarFileError("%s has no %s parts" % (car.key, args.lod))
            obj, mtl = export_obj(car, model, parts, args.out, args.up)
            print("wrote %s and %s (%d parts, %d triangles)"
                  % (obj, mtl, len(parts), sum(p.triangles for p in parts)))
            return 0

        if args.command == "preview":
            car = find_car(args.car, args.root)
            model, _ = load_model(car)
            parts = lod_parts(model, args.lod)
            if not parts:
                raise CarFileError("%s has no %s parts" % (car.key, args.lod))
            preview(car, model, parts, out=args.out,
                    show=not args.no_window and not args.out)
            return 0

        if args.command == "import":
            car = find_car(args.car, args.root)
            source = find_car(args.base, args.root) if args.base else car
            try:
                model, _ = load_model(source)
                header = model_bytes(source.folder)[0][:HEADER_SIZE]
            except CarFileError:
                if args.base:
                    raise
                model = None
                header = blank_header()
            positions, uvs, normals, groups = read_obj(args.obj)
            try:
                flat_uv = tuple(float(c) for c in args.uv.split(","))
            except ValueError:
                raise CarFileError("--uv takes two numbers, u,v")
            built = fce_from_obj(positions, uvs, normals, groups, header, up=args.up,
                                 scale=args.scale, lods=args.lods, flat_uv=flat_uv)
            again = parse_fce(built, args.obj)
            print("%s <- %s" % (car.key, args.obj))
            print("  %d vertices, %d triangles, %d parts, %.3f x %.3f x %.3f m"
                  % (len(again.vertices), len(again.triangles), len(again.parts),
                     again.half_size[0] * 2, again.half_size[1] * 2, again.half_size[2] * 2))
            for part in again.parts:
                print("    %2d  %-24s origin %6.2f %6.2f %6.2f   %4d verts %4d tris"
                      % (part.index, part.name, part.origin[0], part.origin[1], part.origin[2],
                         part.vertices, part.triangles))
            if args.dry_run:
                print("  (nothing written)")
                return 0
            mode = replace_archive_entry(car_archive(car), "car.fce", built)
            (car.folder / "car.fce").write_bytes(built)
            print("  wrote car.fce (%d bytes) into car.viv, %s" % (len(built), mode))
            return 0

        if args.command == "swap":
            car = find_car(args.car, args.root)
            donor = find_car(args.donor, args.root)
            what = ["car.fce"]
            if args.textures:
                what.append("fedata.fsh")
            if args.dashboard:
                what.append("dash.qfs")
            if args.showcase:
                what.append("car00.tga")
            if args.dry_run:
                print("%s <- %s: %s" % (car.key, donor.key, ", ".join(what)))
                print("  (nothing written)")
                return 0
            done = swap_files(car, donor, what)
            for name, mode, size in done:
                print("  %-12s %7d bytes, %s" % (name, size, mode))
            print("%s now has %s's body" % (car.key, donor.key))
            if not args.textures:
                print("  (its own paint stayed -- pass --textures for the donor's)")
            return 0
    except CarFileError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
