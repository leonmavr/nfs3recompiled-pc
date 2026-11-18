"""Port a Need For Speed: High Stakes track folder into this game's own.

    python3 tools/port_track.py --list
    python3 tools/port_track.py --status
    python3 tools/port_track.py --reset [SLOT ...] [--dry-run]
    python3 tools/port_track.py --self-test
    python3 tools/port_track.py NAME [--slot N] [--out DIR] [--borrow N] [--force] [--dry-run]

NAME is a folder of the High Stakes installation's data/TRACKS (HOMETOWN,
AQUATICA, ...), and the result is a track folder of gamedata/tracks -- trk009
unless --slot says another.  The nine tracks both games have have the same slot
in each (trk000 is HOMETOWN, trk006 AQUATICA, ...; --list says which), so
--slot 6 --force puts High Stakes' Aquatica where this game's was and the menus
find it as they always did.  The ten tracks this game never had -- COASTAL,
SNOWY, HILLS, PARK, FRANCE, GERMANY, UK, GT1, GT2, GT3 -- go in a slot past the
ninth, which the game never asks for: its location menu lists nine, and only
the nine have art and places in its tables (0x559620, 0x5596d4, 0x5597f4).

The two games are the same engine a version apart, so most of a High Stakes
track is already what this one reads.  What comes across, and how:

- **TR.FRD** (the road, its polygons and its objects) is the same data in
  another order, and is converted here.  High Stakes writes the file as a list
  of road vectors, then every block's header, then every block's data; this
  game interleaves each block's header with its data.  The header is the same
  fields in another order with a few extra (the eleven chunks' polygon counts,
  the block's min/max corners), a polygon is a byte shorter (no trailing
  unknown), and a polygon carries its own road vector instead of an index into
  its block's: those are collected per block here.  High Stakes' four polygon
  chunks beyond the seven this game's blocks hold -- its extra scenery ones --
  have nowhere to go and are dropped, and so are its special objects (the ones
  the player drives into), which this game has no form for.
- **TR00.CAN ... TR05.CAN** (the objects along the road) are byte for byte this
  game's tr0000.can ... and are copied, renamed to the neighbour they belong to.
- **HEIGHTS.SIM** (the ground's heights) is this game's simhts.dat -- four bytes
  per road vector, and the nine shared tracks have exactly as many vectors in
  each game -- and is copied.
- **TR0.QFS** (the road's textures) is the same FSH container this game loads
  as tr000.qfs, and is copied under all four names the game asks for
  (tr000.qfs, tr000_8.qfs, tr001.qfs, tr001_8.qfs), High Stakes having one
  where this game keeps four.
- **SKY.QFS** is the same container as sky.fsh, but this game's horizon loader
  expects its own sky -- the number of textures in it decides how it is laid
  out, and High Stakes' differ (Park's 43, Aquatica's 12) -- so this game's own
  sky.fsh is kept unless --sky asks for High Stakes'.
Every coordinate, block centre and ground height is scaled by 1.3 on the way
in: High Stakes' world is this game's divided by that, and a ported track left
at High Stakes' scale sits 23% small under this game's cars and its own
(borrowed) collision.  On the shared tracks the scaled result reproduces this
game's own: High Stakes' heights become its `simhts.dat` exactly.

- **TR.INI, TRN.INI, TRW.INI, TRNW.INI** are this game's .hrz files in another
  syntax -- the same horizon, clouds, fog regions, weather and ambient light,
  key by key -- and are translated: the nine shared tracks' fog comes out equal
  to the ones this game ships.
- **TR.CAM** (High Stakes' chase camera) has no equivalent and is dropped.

What High Stakes does not have at all is collision: this game loads trNN.col
and trNN.ccm beside the road, and a ported track has neither.  --borrow N lends
another track's, right only for the nine both games have (the default for
them); a High Stakes track this game never had gets none, is drivable but not
solid until someone writes a .col, and its corners will not stop a car.  The
same --borrow lends speedsf.bin and speedsr.bin, whose High Stakes layout is
not this game's.

**Undoing it.** The first time a port writes into `gamedata/tracks`, the tool
takes `gamedata/tracks-default.tar.gz` -- the tracks as they were -- and
`--reset` puts them back from it: every file of every slot byte for byte, and
it takes away any track that is not one of the game's (a slot 9 port made).
`--reset 6`, `--reset trk006` or `--reset Aquatica` puts back just that slot;
`--dry-run` says what it would do, and `--status` says how each slot stands
against the snapshot. `--snapshot` takes one by hand (`--force` to replace it),
which is worth doing once before anything else touches the tracks.

When a port does not get as far as driving, `--road-only` writes just the road
and leaves the slot's own objects, textures, sky, heights and `.hrz` in place,
and `--skip objects,textures,sky,heights,hrz` leaves out just those, which is
how to find out which part the game chokes on.

Every conversion is checked before it is written: read back with this game's
layout, every polygon's vertices and road vectors in range of its block.

---
"""

import argparse
import shutil
import struct
import sys
import tarfile
from pathlib import Path

# ---------------------------------------------------------------------------
# The nine tracks both games have, in this game's slot order: gamedata/tracks/
# trkNNN is the art name at 0x559620 -- home, redr, atla, rock, coun, lost,
# aqua, summ, empi -- and every one of them has exactly the same number of road
# vectors in the two games (simhts.dat holds one 4-byte entry each), which is
# how each of the nine is matched to its folder here.
# ---------------------------------------------------------------------------

SHARED = ["HOMETOWN", "REDROCK", "ATLANTIC", "ROCKYPAS", "COUNTRY", "LOSTCANY", "AQUATICA", "SUMMIT",
          "EMPIRE"]

# High Stakes' world is this game's divided by 1.3 -- the same tracks, the same
# road vectors in the same order, but every coordinate, every block centre and
# every ground height is 1/1.3 of this game's (multiply High Stakes' Hometown
# heights by 1.3 and this game's simhts.dat comes out digit for digit).  A
# ported track is scaled back up by this on the way in, or it would sit 23%
# small under this game's cars, its own collision and its borrowed files.
SCALE = 1.3

# The polygon chunks a block holds: this game's seven, High Stakes' eleven.
LEVELS = 7
HS_LEVELS = 11

# The four object lists a block has, and the track's own after every block's.
OBJECT_LISTS = 4


class TrackError(Exception):
    pass


class Cursor:
    """A read position in a file's bytes, failing rather than short-reading."""

    def __init__(self, data, at=0):
        self.data = data
        self.at = at

    def take(self, count):
        if count < 0 or self.at + count > len(self.data):
            raise TrackError(f"read of {count} bytes at {self.at:#x} past the file's {len(self.data):#x}")
        out = self.data[self.at:self.at + count]
        self.at += count
        return out

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u8(self):
        return self.take(1)[0]

    def floats(self, count):
        return struct.unpack("<%df" % count, self.take(4 * count))

    def skip(self, count):
        self.take(count)


# ---------------------------------------------------------------------------
# NFS3's FRD: the layout the game reads, block by block (see
# src/nfs3hp/native_loading.cpp, sub_419c20, which reads the same fields in the
# same order).
# ---------------------------------------------------------------------------


def read_nfs3(data):
    """One track's FRD, this game's layout, as a dict of the file's parts."""

    c = Cursor(data)
    track = {"header": c.take(28), "blocks": [], "objects": [], "lights": []}
    last = c.u32()
    count = last + 1
    if not 1 <= count <= 500:
        raise TrackError(f"{count} blocks")
    for _ in range(count):
        b = {}
        b["centre"] = c.floats(3)
        b["bounding"] = [c.floats(3) for _ in range(4)]
        (b["vertices"], b["hires"], b["lores"], b["medres"], b["dup"], b["object_vertices"]) = (
            c.u32() for _ in range(6)
        )
        b["vertex_data"] = c.take(12 * b["vertices"])
        b["shading"] = c.take(4 * b["vertices"])
        b["neighbours"] = c.take(1200)
        (b["start_positions"], b["positions"], b["polygons"], b["road_vectors"], b["xobj"],
         b["polyobj"], b["sounds"], b["lights"]) = (c.u32() for _ in range(8))
        b["position_data"] = c.take(8 * b["positions"])
        b["polygon_road"] = c.take(8 * b["polygons"])
        b["road_data"] = c.take(12 * b["road_vectors"])
        b["xobj_data"] = c.take(20 * b["xobj"])
        b["polyobj_data"] = c.take(20 * b["polyobj"])
        b["sound_data"] = c.take(16 * b["sounds"])
        b["light_data"] = c.take(16 * b["lights"])
        track["blocks"].append(b)
    for b in track["blocks"]:
        b["chunks"] = [read_nfs3_chunk(c) for _ in range(LEVELS)]
        b["polyobj_chunks"] = [read_nfs3_polyobj_chunk(c) for _ in range(OBJECT_LISTS)]
    for b in track["blocks"]:
        b["object_chunks"] = [read_nfs3_object_chunk(c) for _ in range(OBJECT_LISTS)]
    track["objects"].append(read_nfs3_object_chunk(c))
    count = c.u32()
    for _ in range(count):
        track["lights"].append(c.take(47))
    if c.at != len(data):
        raise TrackError(f"ended at {c.at:#x} of the file's {len(data):#x}")
    return track


def read_nfs3_chunk(c):
    """A block's polygons at one level: the room for them, how many there are, them.

    The two counts differ -- 28 of room for 18 polygons, say -- since the first
    is what the game sets aside and the second what it reads (sub_419c20)."""

    room = c.u32()
    if room == 0:
        return None
    count = c.u32()
    return {"room": room, "data": c.take(14 * count)}


def read_nfs3_polyobj_chunk(c):
    """The polygons of the objects a block repeats: how many, then each one's."""

    polygons = c.u32()
    if polygons == 0:
        return None
    types = c.u32()
    objects = []
    for _ in range(types):
        kind = c.u32()
        if kind == 1:
            count = c.u32()
            objects.append((kind, c.take(14 * count)))
        else:
            objects.append((kind, None))
    return {"polygons": polygons, "objects": objects}


def read_nfs3_object_chunk(c):
    """A block's objects at one distance: the count, then each one's data.

    The loader's own order, which is the order the file is in (sub_4198e0): the
    object's type, its number and a word, then twelve bytes of its own (a place
    in the world, for an object that stands still), then the length of an extra
    block of bytes and the block -- an animated object's keyframes -- then its
    vertices, their colours and its polygons.  Anything that does not follow
    that order reads as an object of 196608 vertices and overruns the arena
    (2026-11-18)."""

    count = c.u32()
    objects = []
    for _ in range(count):
        o = {"cross_type": c.u32(), "cross_no": c.u32(), "unknown": c.u32(), "head": c.take(12)}
        extra = c.u32()
        o["extra"] = c.take(extra) if extra else b""
        o["vertices"] = c.u32()
        o["vertex_data"] = c.take(12 * o["vertices"])
        o["shading"] = c.take(4 * o["vertices"])
        o["polygons"] = c.u32()
        o["poly_data"] = c.take(14 * o["polygons"])
        objects.append(o)
    return objects


def write_nfs3(track):
    """The bytes of a track dict in this game's layout."""

    out = bytearray(track["header"])
    blocks = track["blocks"]
    out += struct.pack("<I", len(blocks) - 1)
    for b in blocks:
        out += struct.pack("<3f", *b["centre"])
        for corner in b["bounding"]:
            out += struct.pack("<3f", *corner)
        out += struct.pack("<6I", b["vertices"], b["hires"], b["lores"], b["medres"], b["dup"],
                           b["object_vertices"])
        out += b["vertex_data"] + b["shading"] + b["neighbours"]
        out += struct.pack("<8I", b["start_positions"], b["positions"], b["polygons"], b["road_vectors"],
                           b["xobj"], b["polyobj"], b["sounds"], b["lights"])
        out += b["position_data"] + b["polygon_road"] + b["road_data"] + b["xobj_data"]
        out += b["polyobj_data"] + b["sound_data"] + b["light_data"]
    for b in blocks:
        for chunk in b["chunks"]:
            if chunk is None:
                out += struct.pack("<I", 0)
            else:
                out += struct.pack("<2I", chunk["room"], len(chunk["data"]) // 14) + chunk["data"]
        for chunk in b["polyobj_chunks"]:
            if chunk is None:
                out += struct.pack("<I", 0)
                continue
            out += struct.pack("<2I", chunk["polygons"], len(chunk["objects"]))
            for kind, data in chunk["objects"]:
                out += struct.pack("<I", kind)
                if kind == 1:
                    out += struct.pack("<I", len(data) // 14) + data
    for b in blocks:
        for chunk in b["object_chunks"]:
            out += write_nfs3_object_chunk(chunk)
    for chunk in track["objects"]:
        out += write_nfs3_object_chunk(chunk)
    out += struct.pack("<I", len(track["lights"]))
    out += b"".join(track["lights"])
    return bytes(out)


def write_nfs3_object_chunk(objects):
    out = bytearray(struct.pack("<I", len(objects)))
    for o in objects:
        out += struct.pack("<3I", o["cross_type"], o["cross_no"], o["unknown"])
        out += o["head"]
        out += struct.pack("<I", len(o["extra"])) + o["extra"]
        out += struct.pack("<I", o["vertices"]) + o["vertex_data"] + o["shading"]
        out += struct.pack("<I", o["polygons"]) + o["poly_data"]
    return bytes(out)


def loader_reads(data):
    """Read a track's bytes the way the game's loader does, and return where it
    stopped: the file when the reading was whole.

    read_nfs3 and write_nfs3 are each other's mirror, so they agree about a
    layout even when the game does not -- an object written with its keyframes
    inline read as one of 196608 vertices, because the loader takes the length
    of that block from the file and reads exactly that many bytes after it
    (2026-11-18).  This walks the loader's own order -- sub_419c20's reads from
    front to back, sub_4198e0's objects, sub_419ab0's lights -- so a ported file
    is checked against the reading that matters.  Raises TrackError when the
    reading runs off the end."""

    at = 0

    def take(count, what):
        nonlocal at
        if at + count > len(data):
            raise TrackError(f"the game would read past the end of the file: {count} bytes for "
                             f"{what} at {at:#x} of {len(data):#x}")
        at += count

    def u32(what):
        nonlocal at
        take(4, what)
        return struct.unpack_from("<I", data, at - 4)[0]

    take(28, "the header")
    blocks = u32("the last block's number") + 1
    for block in range(blocks):
        take(0x3c, f"block {block}'s points")
        vertices = u32(f"block {block}'s vertices")
        take(0x14, f"block {block}'s counts")
        take(vertices * 12, f"block {block}'s vertex data")
        take(vertices * 4, f"block {block}'s colours")
        take(0x4b0, f"block {block}'s neighbours")
        u32("count")   # where the block's first position is in the list
        positions, polyroad, road = u32("count"), u32("count"), u32("count")
        xobj, polyobj, sounds, lights = u32("count"), u32("count"), u32("count"), u32("count")
        take(positions * 8, f"block {block}'s positions")
        take(polyroad * 8, f"block {block}'s road polygons")
        take(road * 12, f"block {block}'s road vectors")
        take(xobj * 20, f"block {block}'s objects")
        take(polyobj * 20, f"block {block}'s object groups")
        take(sounds * 16, f"block {block}'s sounds")
        take(lights * 16, f"block {block}'s lamps")
    for block in range(blocks):
        for level in range(0xb):
            room = u32(f"block {block}'s level {level}")
            if room == 0:
                continue
            if level < 7:
                take(u32("count") * 14, f"block {block}'s level {level} polygons")
            else:
                for piece in range(u32("pieces")):
                    if u32("kind") != 1:
                        continue
                    take(u32("count") * 14, f"block {block}'s level {level} piece {piece}")

    def objects(what):
        for _ in range(u32(what + "'s count")):
            take(12, "an object's type")
            take(12, "an object's place")
            take(u32("an object's extra length"), "an object's extra block")
            take(u32("an object's vertices") * 16, "an object's vertices and colours")
            take(u32("an object's polygons") * 14, "an object's polygons")

    for block in range(blocks):
        for group in range(4):
            objects(f"block {block}'s objects {group}")
    objects("the track's objects")
    take(u32("the lights' count") * 0x2f, "the lights")
    return at


# ---------------------------------------------------------------------------
# High Stakes' FRD: road vectors, then every block's header, then every block's
# data, then the two lists of objects that span blocks.
# ---------------------------------------------------------------------------

HS_HEADER = 0x5E8   # a block's header: the sizes, the corners, 300 neighbours
HS_VROAD = 84       # a road vector: a reference point, its three axes, its walls
HS_POLYGON = 13     # vertices, texture, texture flags, animation


def read_hs(data):
    c = Cursor(data)
    track = {"header": c.take(28), "road": [], "blocks": [], "objects": []}
    count = c.u32() + 1
    if not 1 <= count <= 500:
        raise TrackError(f"{count} blocks")
    vectors = c.u32()
    if (vectors + 7) // 8 != count:
        raise TrackError(f"{vectors} road vectors for {count} blocks")
    track["road"] = [read_hs_road_vector(c) for _ in range(vectors)]
    headers = [read_hs_block_header(c) for _ in range(count)]
    for h in headers:
        track["blocks"].append(read_hs_block(c, h))
    for _ in range(2):
        objects = c.u32()
        track["objects"].append(read_hs_objects(c, objects))
    if c.at != len(data):
        raise TrackError(f"ended at {c.at:#x} of the file's {len(data):#x}")
    return track


def read_hs_road_vector(c):
    """A road vector: where it is, its three axes, its two walls, and unknown."""

    vector = {"ref": c.floats(3), "normal": c.floats(3), "forward": c.floats(3),
              "right": c.floats(3), "walls": c.floats(2), "unknown": c.take(8 + 20)}
    return vector


def read_hs_block_header(c):
    h = {"sizes": [c.u32() for _ in range(HS_LEVELS)],
         "sizes_dup": [c.u32() for _ in range(HS_LEVELS)],
         "vertices": c.u32()}
    (h["hires"], h["lores"], h["medres"]) = (c.u32() for _ in range(3))
    h["dup"] = c.u32()
    h["object_vertices"] = c.u32()
    h["unknown2"] = c.take(8)
    h["centre"] = c.floats(3)
    h["bounding"] = [c.floats(3) for _ in range(4)]
    h["neighbours"] = c.take(1200)
    h["object_counts"] = [c.u32() for _ in range(OBJECT_LISTS * 2)][::2]
    h["polygons"] = c.u32()
    h["min"] = c.floats(3)
    h["max"] = c.floats(3)
    h["unknown3"] = c.u32()
    h["positions"] = c.u32()
    h["xobj"] = c.u32()
    c.skip(4)
    h["polyobj"] = c.u32()
    c.skip(4)
    h["sounds"] = c.u32()
    c.skip(4)
    h["lights"] = c.u32()
    c.skip(4)
    h["neighbours2"] = c.take(32)
    return h


def read_hs_block(c, h):
    """A block's data: vertices, its polygons' road vectors, then the polygons."""

    b = {"header": h}
    b["vertex_data"] = c.take(12 * h["vertices"])
    b["shading"] = c.take(4 * h["vertices"])
    b["polygon_road"] = [read_hs_polygon_road(c) for _ in range(h["polygons"])]
    b["xobj_data"] = c.take(20 * h["xobj"])
    b["polyobj_data"] = c.take(20 * h["polyobj"])
    b["sound_data"] = c.take(16 * h["sounds"])
    b["light_data"] = c.take(16 * h["lights"])
    b["chunks"] = [c.take(HS_POLYGON * h["sizes"][level]) for level in range(HS_LEVELS)]
    b["object_chunks"] = [read_hs_objects(c, h["object_counts"][i]) for i in range(OBJECT_LISTS)]
    return b


def read_hs_polygon_road(c):
    """A polygon's road link: two ranges of slices, flags, and its own vector."""

    return {"range": c.take(4), "flags": c.take(5), "unknown": c.u8(), "vector": c.u16(),
            "normal": c.take(6), "forward": c.take(6)}


def read_hs_objects(c, count):
    """Objects that belong to one place: their headers, then each one's data."""

    objects = []
    for _ in range(count):
        o = {"type": c.u32(), "index": c.u32(), "unknown1": c.u32(), "ref": c.floats(3),
             "size": c.u32(), "unknown2": c.u32(), "vertices": c.u32(), "unknown3": c.take(8),
             "polygons": c.u32(), "unknown4": c.u32()}
        objects.append(o)
    for o in objects:
        if o["type"] == 3:
            o["anim"] = c.take(8)
            (o["keyframe_count"],) = struct.unpack("<H", o["anim"][4:6])
            o["keyframes"] = c.take(20 * o["keyframe_count"])
        elif o["type"] == 6:
            o["special"] = c.take(72)
        elif o["type"] not in (1, 2, 4):
            raise TrackError(f"an object of type {o['type']} at {c.at:#x}")
        o["vertex_data"] = c.take(12 * o["vertices"])
        o["shading"] = c.take(4 * o["vertices"])
        o["poly_data"] = c.take(HS_POLYGON * o["polygons"])
    return objects


# ---------------------------------------------------------------------------
# High Stakes to this game's blocks
# ---------------------------------------------------------------------------


def scaled_floats(data, stride=12):
    """A run of records whose first three floats are a point, scaled."""

    out = bytearray(data)
    for at in range(0, len(data) - stride + 12, stride):
        x, y, z = struct.unpack_from("<3f", out, at)
        struct.pack_into("<3f", out, at, x * SCALE, y * SCALE, z * SCALE)
    return bytes(out)


def scaled_points(data, stride=20):
    """A run of records whose first three words are a 16.16 point, scaled."""

    out = bytearray(data)
    for at in range(0, len(data) - stride + 12, stride):
        x, y, z = struct.unpack_from("<3i", out, at)
        struct.pack_into("<3i", out, at, round(x * SCALE), round(y * SCALE), round(z * SCALE))
    return bytes(out)


def road_vectors_of(block):
    """The block's polygons' road vectors, deduplicated: one per road vector used.

    This game keeps a block's road vectors in a list a polygon indexes into,
    where High Stakes repeats the vector in the polygon itself."""

    order = []
    seen = {}
    for polygon in block["polygon_road"]:
        key = (polygon["vector"], polygon["normal"], polygon["forward"])
        if key not in seen:
            seen[key] = len(order)
            order.append(polygon)
    return order, seen


def convert_polygons(data, extra_unknown=b"\xf9"):
    """A run of High Stakes polygons as this game's: one byte longer each."""

    out = bytearray()
    for at in range(0, len(data), HS_POLYGON):
        out += data[at:at + HS_POLYGON] + extra_unknown
    return bytes(out)


# The file's first two words: every one of this game's tracks opens 0x0135 then
# 0xbeef, every High Stakes one 0x0058 then 0xbeef, so the first is the reader's
# version and a ported file has to carry this game's -- with High Stakes' 88 in
# it the game loads nothing at all and the race comes up empty (2026-11-18).
NFS3_HEADER_OPENING = struct.pack("<HH", 0x0135, 0xBEEF)


POSITION_RECORDS = 8   # the runs a block's road polygons are listed in


def position_records(block, index):
    """A block's eight "positions", and where they start in the track's list.

    Every block this game ships carries eight records of eight bytes -- its road
    polygons split into eight runs for the renderer to cull by, one record a-run
    (its first polygon, 0x400 | how many, -1, -1) -- and the list is the track's
    whole, a block's starting at eight times its number (Start Positions).  A
    block with none of them loads and then faults while drawing, in sub_41e6a0,
    which resolves a polygon's corners through them (2026-11-18).  The split is
    the block's polygon count divided as evenly as it goes, which is what this
    game's own files carry: 104 polygons as 8 runs of 13, 106 as five of 13 and
    two of 14."""

    total = block["polygons"]
    records = []
    at = 0
    for run in range(POSITION_RECORDS):
        count = -(-(total - at) // (POSITION_RECORDS - run))
        records.append(struct.pack("<4H", at, 0x400 | min(count, 0x3ff), 0xffff, 0xffff))
        at += count
    return index * POSITION_RECORDS, b"".join(records)


def convert(hs, lights, repeated_objects=False):
    """A High Stakes track as this game's track dict (see write_nfs3)."""

    header = NFS3_HEADER_OPENING + hs["header"][4:]
    track = {"header": header, "blocks": [], "objects": [], "lights": lights}
    for index, source in enumerate(hs["blocks"]):
        h = source["header"]
        vectors, index_of = road_vectors_of(source)
        b = {
            "centre": [v * SCALE for v in h["centre"]],
            "bounding": [[v * SCALE for v in corner] for corner in h["bounding"]],
            "vertices": h["vertices"], "hires": h["hires"], "lores": h["lores"],
            "medres": h["medres"], "dup": h["dup"], "object_vertices": h["object_vertices"],
            "vertex_data": scaled_floats(source["vertex_data"]), "shading": source["shading"],
            "neighbours": h["neighbours"],
            "start_positions": index * POSITION_RECORDS, "positions": POSITION_RECORDS,
            "polygons": h["polygons"], "road_vectors": len(vectors),
            "xobj": h["xobj"],
            # High Stakes' repeated objects (its polyobj list) index polygon
            # banks this converter does not rebuild yet, and an index into a
            # bank that is not there is what the game crashes on: they are left
            # out unless asked for.
            "polyobj": h["polyobj"] if repeated_objects else 0,
            "sounds": h["sounds"], "lights": h["lights"],
            "position_data": b"", "polygon_road": b"", "road_data": b"",   # filled below
            "xobj_data": scaled_points(source["xobj_data"]),
            "polyobj_data": source["polyobj_data"] if repeated_objects else b"",
            "sound_data": source["sound_data"], "light_data": source["light_data"],
        }
        # A polygon's link this game's way: its block's road vector, its flags,
        # and the six bytes of slice range and flags High Stakes splits around them.
        b["polygon_road"] = b"".join(
            bytes([index_of[(p["vector"], p["normal"], p["forward"])] & 0xFF, p["flags"][0]])
            + p["range"] + p["flags"][1:3]
            for p in source["polygon_road"]
        )
        b["road_data"] = b"".join(p["normal"] + p["forward"] for p in vectors)
        b["start_positions"], b["position_data"] = position_records(b, index)
        b["chunks"] = [{"room": len(c), "data": convert_polygons(c)} if c else None
                       for c in source["chunks"][:LEVELS]]
        b["polyobj_chunks"] = [None] * OBJECT_LISTS
        b["object_chunks"] = [convert_objects(chunk) for chunk in source["object_chunks"]]
        track["blocks"].append(b)
    # The objects that span blocks are one list here, two there.
    globals_ = [convert_objects(chunk) for chunk in hs["objects"]]
    track["objects"] = [sum(globals_, [])]
    return track


def convert_objects(objects):
    """A place's objects as this game's: a basic object is a cross-typed copy.

    High Stakes' animated objects have this game's animated form; its special
    ones (things to drive into) have none and are left out."""

    out = []
    for o in objects:
        if o["type"] == 3:
            # Left out for now.  An animated object is an object plus a block of
            # keyframes whose length the game reads before them, and a file
            # whose keyframes are not in that block draws the track and then
            # faults in the drawing (2026-11-18).  The keyframes are in High
            # Stakes' file, scaled like everything else, and can be put in the
            # block the game wants; until then a ported track has its animated
            # objects missing rather than the whole track crashing.
            continue
        elif o["type"] in (1, 2, 4):
            out.append({"cross_type": 4, "cross_no": o["index"], "unknown": 0,
                        "head": struct.pack("<3f", *(v * SCALE for v in o["ref"])), "extra": b"",
                        "vertices": o["vertices"],
                        "vertex_data": scaled_floats(o["vertex_data"]), "shading": o["shading"],
                        "polygons": o["polygons"], "poly_data": convert_polygons(o["poly_data"])})
    return out


# ---------------------------------------------------------------------------
# Textures: the table a track's polygons' texture numbers index
# ---------------------------------------------------------------------------

# The game reads a track into an arena of this many bytes (sub_419c20's
# 0x552dfc), and a track bigger than it loads part of itself: the retail ones
# take 34 to 65% of it.
ARENA = 0x310000


def check(track):
    """A ported track's polygons and objects, their vertices and vectors in range.

    A conversion that leaves an index past its block's vertices would draw
    another block's, or read past the arena, so each one is checked here."""

    for index, b in enumerate(track["blocks"]):
        for level, chunk in enumerate(b["chunks"]):
            if not chunk:
                continue
            for at in range(0, len(chunk["data"]), 14):
                vertices = struct.unpack_from("<4H", chunk["data"], at)
                if any(v >= b["vertices"] for v in vertices):
                    raise TrackError(f"block {index} level {level}: vertices {vertices} of {b['vertices']}")
        for at in range(0, len(b["polygon_road"]), 8):
            if b["polygon_road"][at] >= b["road_vectors"]:
                raise TrackError(f"block {index}: road vector {b['polygon_road'][at]} of {b['road_vectors']}")
        for chunk in b["object_chunks"]:
            for o in chunk:
                for at in range(0, len(o["poly_data"]), 14):
                    vertices = struct.unpack_from("<4H", o["poly_data"], at)
                    if any(v >= o["vertices"] for v in vertices):
                        raise TrackError(f"block {index}: an object's vertices {vertices} of {o['vertices']}")
    # The arena check is fit_arena's: what a file asks for is what the loader
    # allocates, so a track that does not fit is refused there.


def arena(track):
    """How much of the game's track arena reading this track would take.

    The loader allocates a block's vertices and colours, its position, road
    vector, object, sound and light data, every polygon chunk, and every
    object's vertices and polygons, all from one 0x310000-byte arena
    (sub_419c20's 0x552dfc) which is never given back: past the end the
    allocation returns 0 and the read that follows copies the whole of what it
    wanted to address 0 -- so the track loads a stub of itself, the race starts
    with no track in it, and the frontend faults on the way back to the menu
    (trace of 2026-11-18: "[FRD] the arena is full: 10920 wanted with 3200428
    used of 0x310000", then a read into address 0).  So a track has to fit."""

    total = 0
    for b in track["blocks"]:
        total += b["vertices"] * 16 + b["road_vectors"] * 12 + b["positions"] * 8
        total += b["xobj"] * 20 + b["polyobj"] * 20 + b["sounds"] * 16 + b["lights"] * 16
        for chunk in b["chunks"]:
            if chunk:
                total += chunk["room"] * 14
        for chunk in b["polyobj_chunks"]:
            if chunk:
                for kind, data in chunk["objects"]:
                    if kind == 1:
                        total += len(data)
        for chunk in b["object_chunks"]:
            total += len(chunk) * 0x34
            for o in chunk:
                total += o["vertices"] * 16 + o["polygons"] * 14 + len(o["extra"])
    for chunk in track["objects"]:
        total += len(chunk) * 0x34
        for o in chunk:
            total += o["vertices"] * 16 + o["polygons"] * 14 + len(o["extra"])
    return total


# What of the arena a ported track may fill; the rest is headroom.
ARENA_BUDGET = 0.85

# A block keeps its road at eleven levels of detail, but this game's own nine
# tracks fill only four of them -- 0 the coarsest, 2, 4 and 6 -- so the other
# seven are dead weight in a ported file and go first, for nothing.
UNUSED_LEVELS = (1, 3, 5, 7, 8, 9, 10)

# Then the road sets themselves, dearest and least missed first: High Stakes
# carries about ten times this game's road polygons (Park's medium set alone is
# 800 a block against Rocky Pass's 107), so a ported track has to lose whole
# sets.  The high resolution one goes first -- the medium set is already ten
# times what this game draws near -- then the coarsest, then the lanes.
DROP_ORDER = (4, 0, 6)


def fit_arena(track):
    """Shed road detail until the track fits the arena the loader reads it into.

    Park as it comes out of High Stakes wants 6.3 MB of the 3.2 MB arena, and
    what does not fit does not load (see arena).  Returns what was left out, as
    lines to say to whoever is porting; raises TrackError when even a track of
    bare roads is too big, which no ported track has been."""

    def drop(level):
        for b in track["blocks"]:
            if level < len(b["chunks"]):     # High Stakes stores the seven levels 0..6
                b["chunks"][level] = None

    budget = int(ARENA * ARENA_BUDGET)
    dropped = []
    for level in UNUSED_LEVELS:
        drop(level)
    for level in DROP_ORDER:
        if arena(track) <= budget:
            break
        drop(level)
        dropped.append(f"the level {level} road left out")
    if arena(track) > budget:
        # The road is what cannot go: the scenery does, an empty track still
        # being drivable where an unloadable one is a segfault.
        for b in track["blocks"]:
            b["object_chunks"] = [[] for _ in range(OBJECT_LISTS)]
            b["xobj"] = 0
            b["xobj_data"] = b""
            b["polyobj"] = 0
            b["polyobj_data"] = b""
        track["objects"] = [[]]
        dropped.append("the scenery left out")
    if arena(track) > budget:
        raise TrackError(f"this track wants {arena(track)} bytes of the game's {ARENA}-byte track arena "
                         "even with one road set: it cannot be ported")
    return dropped


# ---------------------------------------------------------------------------
# A track's other files
# ---------------------------------------------------------------------------

def read_ini(path):
    """An INI as {section: {key: value}}, the sections and keys as written."""

    sections = {}
    section = ""
    for line in Path(path).read_text(errors="replace").splitlines():
        line = line.split("//")[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            sections.setdefault(section, {})
        elif "=" in line:
            key, _, value = line.partition("=")
            sections.setdefault(section, {})[key.strip().lower()] = value.strip()
    return sections


def colour(text, default=(128, 128, 128)):
    """`[r,g,b]` as three numbers."""

    try:
        values = [int(round(float(v))) for v in text.strip("[] ").split(",")]
    except (AttributeError, ValueError):
        return default
    return tuple((values + list(default))[:3])


def number(section, key, default=0.0):
    try:
        return float(section[key])
    except (KeyError, ValueError):
        return default


def count(section, key, default=0.0):
    """A number that cannot be negative: a slice, a tick count, a density.

    High Stakes' own .INI files carry rubbish in some of these -- Park's
    `stayTimeOff` is -22080 and its `fadeTimeOn` -63360, left over from its
    track editor -- and a negative in the .hrz is what the game's reader dies
    on (2026-11-18)."""
    return max(0.0, number(section, key, default))


def hrz(ini):
    """A High Stakes track's .hrz from its INI: the same horizon and fog, reworded.

    The keys are this game's lines: the INI names what each number is, and the
    order below is the order its own tracks write them in (3tr00.hrz)."""

    strip = ini.get("strip", {})
    clouds = ini.get("clouds", {})
    fog = ini.get("fog", {})
    weather = ini.get("weather", {})
    light = ini.get("light", {})
    lightning = ini.get("lightning", {})
    names = sorted((key for key in ini if key.startswith("fog region ")),
                   key=lambda key: int(key.split()[-1]))
    lines = []

    def add(note, text):
        lines.append(f"/*{note}*/")
        lines.append(f"{text},")

    def colours(section, key, default):
        return ",".join(str(v) for v in colour(section.get(key), default))

    r, g, b = colour(fog.get("fogcolor"))
    add("Fog Color (r,g,b) and Density (%), (%)effect on pixmap",
        f"{r},{g},{b},\t{number(fog, 'fogdensity'):g},\t{number(fog, 'foghorizon') * 100:g}")
    add("Number of Dynamic Fog Regions", f"{len(names)}")
    for index, name in enumerate(names, 1):
        region = ini[name]
        add(f"Fog Region {index}  (starting)r,g,b,density,  (apex)r,g,b,density, "
            "(ending)r,g,b,density,\tbeforeslice,centerslice,afterslice",
            f"{colours(region, 's_color', (128, 128, 128))},\t{count(region, 's_density'):g}")
        lines.append(f"{colours(region, 'c_color', (128, 128, 128))},\t{count(region, 'c_density'):g},")
        lines.append(f"{colours(region, 'e_color', (128, 128, 128))},\t{count(region, 'e_density'):g},")
        lines.append(f"{count(region, 'startslice'):g},")
        lines.append(f"{count(region, 'centerslice'):g},")
        lines.append(f"{count(region, 'endslice'):g},")
        lines.append("")
    add("Cloud Type 0, 1 additive, 2 blend", f"{number(clouds, 'cloudtype', 1):g}")
    add("Fog Clouds?", f"{number(clouds, 'cloudfog'):g}")
    add("Has Lightning?, (%chance per # ticks)",
        f"{number(lightning, 'lightningchance'):g},{number(lightning, 'lightningoffticks'):g}")
    add("Visual Height 100-1000", f"{number(clouds, 'cloudvisualheight', 500):g}")
    add("Cloud dome offset", f"{number(clouds, 'cloudyoffset', -100):g},\t250")
    add("Cloud bright (0-255), Cloud Variance (< bright), Nimbus Effect(%)",
        f"{number(clouds, 'cloudbright', 235):g},{number(clouds, 'cloudvariance', 208):g},"
        f"{number(clouds, 'cloudnimbus', 0.8) * 100:g}")
    add("WindSpeed (clouds) mph, dir", f"{number(clouds, 'windspeed', 10):g},{number(clouds, 'winddir'):g}")
    add(" Black Horizon  (1=yes & 0=no) ", f"{number(strip, 'hrzshoulddrawblack'):g}")
    add(" Mirror Flag ", f"{number(strip, 'hrzmirror', 1):g}")
    add(" The radius of the horizon ", f"{number(strip, 'hrzfradius', 1500):g}")
    add(" The angle of rotation for the horizon in degrees. ", f"{number(strip, 'hrzangle'):g}")
    add(" Has a pixmap ", f"{number(strip, 'hrzhaspmx', 1):g}")
    add(" How high in the air the base of the Gourad shaded area is (world offset). ",
        f"{number(strip, 'hrzbottomyoff', -1200):g}")
    add(" Height of the Gourad shaded area (offset from base) ",
        f"{number(strip, 'hrzgouraudheight', 1800):g}")
    add(" Midpoint of Gourad shaded area (offset from base) ",
        f"{number(strip, 'hrzgouraudmiddle', 1200):g}")
    add(" Top of horizon pixmap (offset from base) ", f"{number(strip, 'hrzpmxtop', 1350):g}")
    add(" Bottom of horizon pixmap (offset from base) ", f"{number(strip, 'hrzpmxbottom', 1150):g}")
    add(" r,g,b value at top of Gourad shaded EARTH area ", colours(strip, "hrzearthtopcolor", (117, 106, 15)))
    add(" r,g,b value at base of Gourad shaded EARTH area ",
        colours(strip, "hrzearthbotcolor", (207, 153, 98)))
    add(" r,g,b value at top of Gourad shaded SKY area ", colours(strip, "hrzskytopcolor", (120, 182, 239)))
    add(" r,g,b values for base of Gourad shaded SKY area ",
        colours(strip, "hrzsuncolor", (223, 210, 151)))
    lines.append(f"{colours(strip, 'hrzoppositesuncolor', (120, 171, 204))},")
    lines.append("")
    lines.append("/* weather parameters for track! */")
    lines.append("")
    for note, key in (("type", "type"), ("startSlice", "startslice"), ("endSlice", "endslice"),
                      ("fade", "fade"), ("stayTimeOn  Ticks...", "staytimeon"),
                      ("fadeTimeOff Ticks...", "fadetimeoff"), ("stayTimeOff Ticks...", "staytimeoff"),
                      ("fadeTimeOn Ticks...", "fadetimeon")):
        add(note, f"{count(weather, key):g}")
    add(" Ambient Light Percentages (R,G,B) 0-100",
        f"{number(light, 'ambientred', 100):g},{number(light, 'ambientgreen', 100):g},"
        f"{number(light, 'ambientblue', 100):g}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# The port itself
# ---------------------------------------------------------------------------


def find_tracks(game_dir):
    """Every High Stakes track folder this checkout has, by name."""

    found = {}
    for path in game_dir.glob("**/data/TRACKS/*"):
        if path.is_dir() and (path / "TR.FRD").exists():
            found[path.name.upper()] = path
    return found


def game_root():
    return Path(__file__).resolve().parent.parent


# ---------------------------------------------------------------------------
# The tracks as the game shipped them: a snapshot the tool takes before it
# first writes anything, and puts back with --reset.
# ---------------------------------------------------------------------------

DEFAULT_SNAPSHOT = "tracks-default.tar.gz"   # beside the tracks folder, in gamedata/


def tracks_dir(root):
    return root / "gamedata" / "tracks"


def default_path(root):
    return tracks_dir(root).parent / DEFAULT_SNAPSHOT


def make_default(root, force=False):
    """Snapshot the tracks as they are now, for --reset to put back."""

    path = default_path(root)
    if path.exists() and not force:
        raise TrackError(f"{path} is the snapshot already: --force to take a new one")
    tracks = tracks_dir(root)
    if not tracks.is_dir():
        raise TrackError(f"no {tracks}")
    if not force:
        # A snapshot of a ported track is no use as "the original": say so
        # rather than quietly keeping half High Stakes.
        ported = read_hs_markers(tracks)
        if ported:
            raise TrackError(f"{', '.join(ported)} look ported already; --force to snapshot them anyway")
    temporary = path.with_suffix(".partial")
    with tarfile.open(temporary, "w:gz") as archive:
        for slot in sorted(tracks.iterdir()):
            if not slot.is_dir():
                continue
            for item in sorted(slot.iterdir()):
                if item.is_file():
                    archive.add(item, arcname=f"tracks/{slot.name}/{item.name}")
    temporary.replace(path)
    return path


def read_hs_markers(tracks):
    """The slots whose FRD is not this game's own: a High Stakes header's first
    two words are 0x??58/0xbeef with a 959-style road-vector count after."""

    ported = []
    for slot in sorted(tracks.iterdir()):
        for path in sorted(slot.glob("tr*.frd")):
            data = path.read_bytes()
            if len(data) < 8 or data[2:4] != b"\xef\xbe":
                ported.append(f"{slot.name}/{path.name}")
                break
            vectors = struct.unpack_from("<I", data, 0x20)[0]
            blocks = struct.unpack_from("<I", data, 0x1c)[0] + 1
            if blocks and (vectors + 7) // 8 == blocks:
                ported.append(f"{slot.name}/{path.name}")
                break
    return ported


def read_default(root):
    """The snapshot as {slot: {file name: bytes}}, or {} when there is none."""

    path = default_path(root)
    if not path.exists():
        return {}
    snapshot = {}
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            parts = member.name.split("/")
            if not member.isfile() or len(parts) != 3 or parts[0] != "tracks":
                continue
            slot, name = parts[1], parts[2]
            handle = archive.extractfile(member)
            if handle is not None:
                snapshot.setdefault(slot, {})[name] = handle.read()
    return snapshot


def slot_of(text, snapshot):
    """A slot from what a person types: 6, trk006, or Aquatica."""

    spelling = text.strip()
    lowered = spelling.lower()
    if lowered.startswith("trk"):
        lowered = lowered[3:]
    if lowered.isdigit():
        candidate = f"trk{int(lowered):03d}"
        if snapshot and candidate not in snapshot:
            raise TrackError(f"the snapshot has no {candidate}; it has {', '.join(sorted(snapshot))}")
        return candidate
    for index, name in enumerate(SHARED):
        if name.lower() == lowered:
            return f"trk{index:03d}"
    raise TrackError(f"{text} is not a track: give a slot (6), trk006, or one of {', '.join(SHARED)}")


def reset(root, slots, dry_run=False):
    """Put the snapshot's tracks back, and take away any that are extra.

    Returns what was (or would be) done, a line a file."""

    snapshot = read_default(root)
    if not snapshot:
        raise TrackError(f"no {default_path(root)} to put back: "
                         f"--snapshot takes one from the tracks as they are now")
    live = tracks_dir(root)
    named = bool(slots)
    wanted = sorted(snapshot) if not named else [slot_of(slot, snapshot) for slot in slots]
    done = []
    for slot in sorted(live.iterdir()) if live.is_dir() else []:
        # A track that is not the game's goes when the reset is the whole set;
        # resetting one slot leaves the others alone.
        if not slot.is_dir() or slot.name in snapshot or (named and slot.name not in wanted):
            continue
        done.append(f"remove {slot.name}/ (not one of the game's tracks)")
        if not dry_run:
            shutil.rmtree(slot)
    for slot in wanted:
        folder = live / slot
        keep = snapshot[slot]
        for name, data in sorted(keep.items()):
            path = folder / name
            if path.exists() and path.read_bytes() == data:
                continue
            done.append(f"put back {slot}/{name}")
            if not dry_run:
                folder.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        if folder.is_dir():
            for path in sorted(folder.iterdir()):
                if path.is_file() and path.name not in keep:
                    done.append(f"remove {slot}/{path.name} (not one of the game's)")
                    if not dry_run:
                        path.unlink()
    return done


def status(root):
    """Each of the game's slots, and how it stands against the snapshot."""

    snapshot = read_default(root)
    live = tracks_dir(root)
    lines = []
    for slot in sorted(snapshot) if snapshot else [f"trk{index:03d}" for index in range(len(SHARED))]:
        folder = live / slot
        if not snapshot:
            lines.append((slot, "no snapshot to compare with"))
            continue
        keep = snapshot[slot]
        present = {path.name: path for path in folder.iterdir() if path.is_file()} if folder.is_dir() else {}
        changed = [name for name, data in keep.items() if name in present and present[name].read_bytes() != data]
        missing = [name for name in keep if name not in present]
        extra = [name for name in present if name not in keep]
        if not changed and not missing and not extra:
            lines.append((slot, "as shipped"))
        else:
            parts = []
            if changed:
                parts.append(f"{len(changed)} changed")
            if missing:
                parts.append(f"{len(missing)} missing")
            if extra:
                parts.append(f"{len(extra)} extra")
            lines.append((slot, ", ".join(parts)))
    for slot in sorted(live.iterdir()) if live.is_dir() else []:
        if slot.is_dir() and snapshot and slot.name not in snapshot:
            lines.append((slot.name, "not one of the game's tracks"))
    return lines


ASSET_GROUPS = ("objects", "textures", "sky", "heights", "hrz")


def port(name, game_dir, out, slot, borrow, allow_overwrite=False, dry_run=False,
         skip=(), repeated_objects=False, reduce=False):
    """Write a High Stakes track into a track folder; return what was written."""

    tracks = find_tracks(game_dir)
    key = name.upper()
    if key not in tracks:
        raise TrackError(f"no {name} track; have: {', '.join(sorted(tracks))}")
    source = tracks[key]
    slot = int(slot)
    written = []
    if out.exists() and any(out.iterdir()) and not (allow_overwrite or dry_run):
        raise TrackError(f"{out} already has files: --force to replace that track")

    road = source / "TR.FRD"
    if not road.exists():
        raise TrackError(f"{source} has no TR.FRD")
    hs = read_hs(road.read_bytes())
    borrowed = borrow_track(borrow) if borrow is not None else None
    track = convert(hs, borrowed["lights"] if borrowed else [], repeated_objects)
    dropped = fit_arena(track)
    check(track)
    frd = write_nfs3(track)
    read_nfs3(frd)   # a ported track the game could not read back is a bug here
    end = loader_reads(frd)   # and one the game would read differently is worse
    if end != len(frd):
        raise TrackError(f"the game would stop reading {out.name} at {end} of {len(frd)} bytes")
    if not dry_run:
        out.mkdir(parents=True, exist_ok=True)
        (out / f"tr{slot:02x}.frd").write_bytes(frd)
    written.append((f"tr{slot:02x}.frd", len(frd),
                    "converted from TR.FRD" + (f" ({dropped[0]})" if dropped else "")))

    if "objects" not in skip:
        for source_can in sorted(source.glob("TR*.CAN")):
            stem = source_can.stem.upper()
            if stem.startswith("TRN") or stem.startswith("TRW"):
                continue
            suffix = stem[2:]
            if not suffix or not suffix[0].isdigit():
                continue
            number = int(suffix[:2], 16)
            letter = suffix[2:].lower()
            target = out / f"tr{slot:02x}{number:02x}{letter}.can"
            if not dry_run:
                shutil.copyfile(source_can, target)
            written.append((target.name, source_can.stat().st_size, f"from {source_can.name}"))

    if "heights" not in skip:
        heights = source / "HEIGHTS.SIM"
        if heights.exists():
            # A float for each road vector: High Stakes' at its own scale, this
            # game's 1.3 times them.
            data = heights.read_bytes()
            values = struct.unpack("<%df" % (len(data) // 4), data)
            data = struct.pack("<%df" % len(values), *(value * SCALE for value in values))
            if not dry_run:
                (out / "simhts.dat").write_bytes(data)
            written.append(("simhts.dat", len(data), "from HEIGHTS.SIM, scaled"))

    if "textures" not in skip:
        page = source / "TR0.QFS"
        if page.exists():
            for suffix in ("0.qfs", "0_8.qfs", "1.qfs", "1_8.qfs"):
                if not dry_run:
                    shutil.copyfile(page, out / f"tr{slot:02x}{suffix}")
                written.append((f"tr{slot:02x}{suffix}", page.stat().st_size, "from TR0.QFS"))

    sky = source / "SKY.QFS"
    if "sky" in skip or not sky.exists():
        # High Stakes' sky does not load in this game's horizon loader, and one
        # left in the folder from an earlier port stops the whole track's setup
        # before the road is read at all -- the race then starts with no track
        # in it and the frontend faults on the way back (2026-11-18).  So the
        # game's own goes back rather than the folder keeping whatever was
        # there.
        own = game_own(slot, "sky.fsh")
        if own is not None:
            if not dry_run:
                (out / "sky.fsh").write_bytes(own)
            written.append(("sky.fsh", len(own), "the game's own: High Stakes' sky does not load"))
    else:
        if not dry_run:
            shutil.copyfile(sky, out / "sky.fsh")
        written.append(("sky.fsh", sky.stat().st_size, "from SKY.QFS (may not load)"))

    if "hrz" not in skip:
        for suffix, ini_name in (("", "TR.INI"), ("n", "TRN.INI"), ("w", "TRW.INI"), ("nw", "TRNW.INI")):
            ini = source / ini_name
            if not ini.exists():
                continue
            text = hrz(read_ini(ini))
            if not dry_run:
                (out / f"3tr{slot:02x}{suffix}.hrz").write_text(text)
            written.append((f"3tr{slot:02x}{suffix}.hrz", len(text), f"from {ini.name}"))

    for name in (f"tr{slot:02x}.col", f"tr{slot:02x}.ccm", "speedsf.bin", "speedsr.bin"):
        if not borrowed or name not in borrowed:
            continue
        data = borrowed[name]
        if not dry_run:
            (out / name).write_bytes(data)
        written.append((name, len(data), f"borrowed from trk{borrow:03d}"))
    return written


def game_own(slot, name):
    """A file of a track slot as the game shipped it, from the snapshot.

    None when the snapshot has no such file (or no snapshot yet), in which case
    the port leaves whatever the folder holds alone."""

    path = default_path(game_root())
    if not path.exists():
        return None
    with tarfile.open(path) as archive:
        try:
            member = archive.getmember(f"tracks/trk{slot:03d}/{name}")
        except KeyError:
            return None
        return archive.extractfile(member).read()


def borrow_track(slot):
    """The files of an NFS3 track of the same name, to lend what High Stakes lacks.

    From the snapshot, which holds the game's own: the slot named here may hold a
    ported track already, whose road is not this game's to lend the lights of --
    and after a port from an older layout, one the reader cannot read at all
    (2026-11-18).  The live folder serves only when there is no snapshot."""

    names = (f"tr{slot:02x}.col", f"tr{slot:02x}.ccm", "speedsf.bin", "speedsr.bin")
    frd = game_own(slot, f"tr{slot:02x}.frd")
    if frd is None:
        root = game_root() / "gamedata" / "tracks" / f"trk{slot:03d}"
        if not root.exists():
            raise TrackError(f"no trk{slot:03d} to borrow from")
        path = root / f"tr{slot:02x}.frd"
        if not path.exists():
            raise TrackError(f"trk{slot:03d} has no {path.name}")
        frd = path.read_bytes()
    lent = {"lights": read_nfs3(frd)["lights"]}
    # Not every track has a .ccm: Atlantica ships without one.
    for name in names:
        own = game_own(slot, name)
        if own is not None:
            lent[name] = own
        else:
            path = game_root() / "gamedata" / "tracks" / f"trk{slot:03d}" / name
            if path.exists():
                lent[name] = path.read_bytes()
    return lent


def self_test(root):
    """This game's nine tracks written back as they came, and every High Stakes
    track converted and checked, in memory: what the reader and the conversion
    are worth, without writing anything."""

    failures = 0
    tracks = sorted((root / "gamedata" / "tracks").glob("trk*/tr*.frd"))
    for path in tracks:
        data = path.read_bytes()
        try:
            again = write_nfs3(read_nfs3(data))
        except TrackError as error:
            print(f"FAIL {path}: {error}")
            failures += 1
            continue
        if again != data:
            print(f"FAIL {path}: written back {len(again)} bytes of {len(data)}")
            failures += 1
        elif loader_reads(data) != len(data):
            print(f"FAIL {path}: the game's own reading of it does not end at its end")
            failures += 1
        else:
            print(f"ok   {path.name}: read and written back whole, and read as the game reads it")
    ported = 0
    for name, path in sorted(find_tracks(root / "nfshs").items()):
        try:
            track = convert(read_hs((path / "TR.FRD").read_bytes()), [])
            check(track)
            fit_arena(track)
            data = write_nfs3(track)
            read_nfs3(data)
            if loader_reads(data) != len(data):
                raise TrackError("the game would not read all of it")
            ported += 1
        except TrackError as error:
            print(f"FAIL {name}: {error}")
            failures += 1
    print(f"ok   {ported} High Stakes tracks converted and read back")
    return failures


def main():
    root = game_root()
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("track", nargs="?", help="the High Stakes track's folder name")
    parser.add_argument("--list", action="store_true", help="the tracks there are to port")
    parser.add_argument("--self-test", action="store_true",
                        help="check the reader on this game's tracks and the conversion on "
                             "every High Stakes one, writing nothing")
    parser.add_argument("--snapshot", action="store_true",
                        help=f"write {DEFAULT_SNAPSHOT} from the tracks as they are now, for "
                             "--reset to put back (taken for you before the first port)")
    parser.add_argument("--reset", nargs="*", metavar="SLOT",
                        help="put the game's tracks back from the snapshot: all of them, or the "
                             "slots named (6, trk006, Aquatica) -- and take away any track that "
                             "is not the game's")
    parser.add_argument("--status", action="store_true",
                        help="each of the game's track slots, and how it stands against the snapshot")
    parser.add_argument("--slot", type=int, default=9, help="the trkNNN to write (default 9)")
    parser.add_argument("--out", help="the folder to write (default gamedata/tracks/trkNNN)")
    parser.add_argument("--borrow", type=int,
                        help="an NFS3 track slot to take trNN.col, trNN.ccm and the speed "
                             "files from (needed for collision; default the same track)")
    parser.add_argument("--force", action="store_true", help="replace a track folder that is not empty")
    parser.add_argument("--reduce", action="store_true",
                        help="no longer needed: a track that does not fit the game's "
                             "0x310000-byte track arena is now fitted on the way through "
                             "(a track the loader cannot fit does not load at all). Kept so "
                             "old command lines still work")
    parser.add_argument("--sky", action="store_true",
                        help="take High Stakes' sky (SKY.QFS) as sky.fsh. Off by default: this "
                             "game's horizon loader expects its own sky, and a High Stakes one "
                             "with a different set of textures in it crashes the track load "
                             "(Park's has 43, Aquatica's 12)")
    parser.add_argument("--skip", metavar="GROUP", default="",
                        help="leave out of the port: objects, textures, sky, heights, hrz "
                             "(comma separated); what is left out stays as the slot already has it")
    parser.add_argument("--road-only", action="store_true",
                        help="write only the converted road (trNN.frd) and leave the slot's own "
                             "objects, textures, sky, heights and .hrz as they are: for finding "
                             "out which part of a port the game chokes on")
    parser.add_argument("--repeated-objects", action="store_true",
                        help="keep High Stakes' list of repeated objects (off by default: the "
                             "polygon banks behind it are not rebuilt yet, and an index into a "
                             "bank that is not there crashes the game)")
    parser.add_argument("--dry-run", action="store_true", help="say what would be written")
    args = parser.parse_args()

    game_dir = root / "nfshs"
    tracks = find_tracks(game_dir)
    if args.self_test:
        return 1 if self_test(root) else 0
    if args.snapshot:
        path = make_default(root, args.force)
        print(f"wrote {path}: {DEFAULT_SNAPSHOT} of {', '.join(sorted(read_default(root)))}")
        return 0
    if args.status:
        for slot, standing in status(root):
            print(f"{slot}  {standing}")
        print(f"\nsnapshot: {default_path(root)}" if default_path(root).exists() else
              f"\nno snapshot yet: --snapshot takes one from the tracks as they are now")
        return 0
    if args.reset is not None:
        if not args.reset and not default_path(root).exists():
            raise TrackError(f"no {DEFAULT_SNAPSHOT} to put back; --snapshot takes one from the "
                             f"tracks as they are now")
        done = reset(root, args.reset, args.dry_run)
        print(f"{'would put back' if args.dry_run else 'put back'} the game's tracks:")
        for line in done:
            print(f"  {line}")
        if not done:
            print("  nothing to do: they are as shipped")
        return 0
    if args.list or not args.track:
        for name in sorted(tracks):
            path = tracks[name]
            shared = SHARED.index(name) if name in SHARED else None
            print(f"{name:10} {path}")
            if shared is not None:
                print(f"{'':10} both games have it: this game's trk{shared:03d}")
        return 0
    if not tracks:
        raise TrackError(f"no High Stakes tracks under {game_dir}")

    borrow = args.borrow
    if borrow is None and args.track.upper() in SHARED:
        borrow = SHARED.index(args.track.upper())
    out = Path(args.out) if args.out else tracks_dir(root) / f"trk{args.slot:03d}"
    if not args.dry_run and out.parent == tracks_dir(root) and not default_path(root).exists():
        # What --reset puts back has to be the tracks as they were before this.
        print(f"first write into the game's tracks: taking {DEFAULT_SNAPSHOT} first")
        try:
            make_default(root)
        except TrackError as error:
            print(f"  ({error})")
            make_default(root, force=True)
    skip = {group.strip() for group in args.skip.split(",") if group.strip()}
    if not args.sky:
        skip.add("sky")
    unknown = skip - set(ASSET_GROUPS)
    if unknown:
        raise TrackError(f"cannot skip {', '.join(sorted(unknown))}: the parts are "
                         f"{', '.join(ASSET_GROUPS)}")
    if args.road_only:
        skip = set(ASSET_GROUPS)
    written = port(args.track, game_dir, out, args.slot, borrow, args.force, args.dry_run,
                   skip, args.repeated_objects, args.reduce)

    print(f"{'would write' if args.dry_run else 'wrote'} {out}:")
    for name, size, note in written:
        print(f"  {name:16} {size:9}  {note}")
    missing = [name for name in ("collision",) if borrow is None]
    if missing:
        print(f"\nno collision: {args.track} is not a track this game has, so it has no "
              f"tr{args.slot:02x}.col; --borrow N lends another track's (see the docstring).")
    name = args.track.upper()
    if borrow is not None and (name not in SHARED or SHARED.index(name) != borrow):
        print(f"\nthe collision is another place's: trk{borrow:03d} is {SHARED[borrow]}'s, whose road "
              f"runs through {name} at a different length and in a different place.  "
              f"The road loads -- the track's own blocks, heights, textures and horizon -- and the "
              f"race then faults where the two meet (2026-11-18).  A .col of its own has to be made "
              f"before this track can be driven.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except TrackError as error:
        sys.exit(f"port_track: {error}")
