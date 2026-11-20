#!/usr/bin/env python3
"""Generate the minimap's sprites: the atlas the game loads.

Two sprites, each a TILE-pixel square of assets/minimap/minimap-atlas.png
(the game's side is native_minimap.cpp, whose `kTile`, `kPrismTile`,
`kCornerTile` and the prism's corners `kPrismTip`/`kPrismBase` must match the
values here):

- the prism (tile 0): a pointed triangle, tip up -- the way the car faces --
  shaded in greys, which the game tints by the driver's colour (the overlay's
  Map kind: 0.5 is the colour itself, darker below it, towards white above
  it).  It is drawn as a low three-sided pyramid seen from above: its apex
  the joint of its skeleton -- an inverted Y with its tail at the tip -- the
  Y its three ridges, the triangle its base.  Each facet is lit by its own
  slope from the upper left (a glint on the lit ones), the outer edges are
  bevelled, the ridges and the outline are a darker line, each facet grows a
  little lighter towards its middle, and a soft shadow falls below and right;
- the corner (tile 1): the top-left piece of a metal frame -- a thin L,
  chamfered, silver, semi-translucent, darker along its outlines with a slight
  shine over its inner part.  The game turns it for the other three corners
  (flipping its uv) and draws it untinted (colour 0.5).

Everything is drawn SUPER times larger and box-filtered down, so the edges are
smooth; the pixels are straight (not premultiplied) RGBA.  `--preview` also
writes both, tinted as the game would, enlarged over the HUD's navy.

    python3 tools/generate_minimap_art.py [--preview sheet.png]
"""

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


TILE = 128           # a sprite's square, as the game reads it
TILES = 2            # across the atlas; one row
SUPER = 4            # drawn this many times larger, then filtered down

# The prism, in the tile's pixels: the tip, the base's two corners, and the
# apex -- where the Y's tail meets its two arms, raised above the base.
PRISM_TIP = (64.0, 4.0)
PRISM_LEFT = (25.0, 118.0)
PRISM_RIGHT = (103.0, 118.0)
PRISM_JOINT = (64.0, 80.0)
PRISM_HEIGHT = 30.0          # the apex above the base, pixels: how steep the facets
PRISM_LIGHT = (-0.55, -0.70, 0.62)   # towards the light: up-left and above (x right, y down, z up)
PRISM_AMBIENT = 0.27
PRISM_DIFFUSE = 0.42
PRISM_SPECULAR = 0.55        # the glint, whitening (shades over 0.5 go towards white)
PRISM_SHININESS = 18.0
PRISM_BEVEL = 6.0            # the outer edges rolled over this many pixels
PRISM_BEVEL_TILT = 1.6       # how far the bevel leans out
PRISM_LINE = 0.18            # the ridges' and the outline's shade
PRISM_LINE_WIDTH = 2.6
PRISM_GLOW = 0.10            # each facet lighter towards its middle, by this much
PRISM_SHADOW = (3.0, 5.0)    # the drop shadow's offset, pixels
PRISM_SHADOW_BLUR = 3.5
PRISM_SHADOW_ALPHA = 0.55

# The corner: a thin L along the tile's top and left edges.
CORNER_ARM = 124.0           # each arm's length
CORNER_THICK = 15.0          # its thickness
CORNER_CHAMFER = 9.0         # the outer corner cut off at 45 degrees
CORNER_SILVER = np.array([0.80, 0.82, 0.86])
CORNER_OUTLINE = 0.38        # the outline's shade (times the silver)
CORNER_OUTLINE_WIDTH = 2.2
CORNER_SHINE = 0.35          # how much lighter its inner part's shine
CORNER_ALPHA = 0.82          # semi-translucent

NAVY = (41, 33, 77)          # the HUD's panel colour, for the preview


def grid(size):
    """Pixel centres of a tile drawn SUPER times larger, in the tile's own pixels."""
    n = size * SUPER
    c = (np.arange(n) + 0.5) / SUPER
    return np.meshgrid(c, c)


def segment_distance(x, y, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = np.clip(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0)
    return np.hypot(x - (ax + t * dx), y - (ay + t * dy))


def side(x, y, a, b):
    """Which side of the line a->b each point is on (the sign of the cross product)."""
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])


def inside_triangle(x, y, a, b, c):
    s1, s2, s3 = side(x, y, a, b), side(x, y, b, c), side(x, y, c, a)
    return ((s1 >= 0) & (s2 >= 0) & (s3 >= 0)) | ((s1 <= 0) & (s2 <= 0) & (s3 <= 0))


def down(image):
    """Box-filter a SUPER-sized RGBA (straight alpha) down to the tile, averaging
    premultiplied, so no dark fringe comes in from the transparent outside."""
    h, w, _ = image.shape
    pre = image.copy()
    pre[..., :3] *= pre[..., 3:4]
    pre = pre.reshape(h // SUPER, SUPER, w // SUPER, SUPER, 4).mean(axis=(1, 3))
    a = pre[..., 3:4]
    pre[..., :3] = np.where(a > 0, pre[..., :3] / np.maximum(a, 1e-6), 0.0)
    return pre


def normalize(v):
    n = np.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def blur(mask, radius):
    """A soft-edged copy of a mask: a separable box blur, three times over (near a Gaussian)."""
    r = max(1, int(round(radius * SUPER)))
    out = mask.astype(float)
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (r, r)
            padded = np.pad(out, pad, mode="constant")
            c = np.cumsum(padded, axis=axis)
            c = np.insert(c, 0, 0.0, axis=axis)
            n = out.shape[axis]
            lo = np.take(c, np.arange(n), axis=axis)
            hi = np.take(c, np.arange(n) + 2 * r + 1, axis=axis)
            out = (hi - lo) / (2 * r + 1)
    return out


def prism():
    """A low three-sided pyramid seen from above: its apex the Y's joint, its ridges
    the Y, its base the triangle.  Each facet lit by its own normal (diffuse and a
    glint), the outer edges bevelled, the ridges and the outline drawn dark, each
    facet a little lighter towards its middle, and a soft shadow below and right."""
    x, y = grid(TILE)
    tip, left, right, joint = PRISM_TIP, PRISM_LEFT, PRISM_RIGHT, PRISM_JOINT
    inside = inside_triangle(x, y, tip, left, right)
    light = np.array(normalize(PRISM_LIGHT))
    half = np.array(normalize([light[0], light[1], light[2] + 1.0]))   # towards the eye, straight above

    # the outline's distance, and which edge is nearest (its outward normal, for the bevel)
    edges = [(tip, left), (left, right), (right, tip)]
    centroid = ((tip[0] + left[0] + right[0]) / 3.0, (tip[1] + left[1] + right[1]) / 3.0)
    edge_d = np.stack([segment_distance(x, y, a, b) for a, b in edges])
    nearest = np.argmin(edge_d, axis=0)
    d_edge = edge_d.min(axis=0)
    outward = []
    for a, b in edges:
        nx, ny = b[1] - a[1], -(b[0] - a[0])
        mx, my = (a[0] + b[0]) / 2.0 - centroid[0], (a[1] + b[1]) / 2.0 - centroid[1]
        if nx * mx + ny * my < 0:
            nx, ny = -nx, -ny
        n = np.hypot(nx, ny)
        outward.append((nx / n, ny / n))

    facets = [(tip, joint, left), (tip, joint, right), (joint, left, right)]
    ridges = [(tip, joint), (joint, left), (joint, right)]
    d_ridge = np.min([segment_distance(x, y, a, b) for a, b in ridges], axis=0)
    d_line = np.minimum(d_ridge, d_edge)
    shade = np.zeros_like(x)
    for a, b, c in facets:
        where = inside_triangle(x, y, a, b, c)
        if not where.any():
            continue
        # the facet's plane: the joint raised, the triangle's corners on the base
        pts = [np.array([p[0], p[1], PRISM_HEIGHT if p == joint else 0.0]) for p in (a, b, c)]
        n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
        n = n / np.linalg.norm(n)
        if n[2] < 0:
            n = -n
        # the bevel: near the outline the surface leans out over the edge
        roll = np.clip(1.0 - d_edge / PRISM_BEVEL, 0.0, 1.0) ** 1.5 * PRISM_BEVEL_TILT
        ox = np.choose(nearest, [o[0] for o in outward])
        oy = np.choose(nearest, [o[1] for o in outward])
        nx, ny, nz = n[0] + ox * roll, n[1] + oy * roll, n[2] + 0.0 * roll
        length = np.sqrt(nx * nx + ny * ny + nz * nz)
        nx, ny, nz = nx / length, ny / length, nz / length
        diffuse = np.clip(nx * light[0] + ny * light[1] + nz * light[2], 0.0, 1.0)
        glint = np.clip(nx * half[0] + ny * half[1] + nz * half[2], 0.0, 1.0) ** PRISM_SHININESS
        reach = d_line[where].max()
        middle = np.clip(d_line / reach, 0.0, 1.0)
        value = PRISM_AMBIENT + PRISM_DIFFUSE * diffuse + PRISM_GLOW * middle + PRISM_SPECULAR * glint
        shade = np.where(where, value, shade)
    line = np.clip((PRISM_LINE_WIDTH * 0.5 - d_line) / 1.0 + 0.5, 0.0, 1.0)
    shade = shade * (1.0 - line) + PRISM_LINE * line

    # the shadow: the triangle's mask moved down and right, softened; under the prism
    sx, sy = PRISM_SHADOW
    shadow_mask = inside_triangle(x - sx, y - sy, tip, left, right)
    shadow = blur(shadow_mask, PRISM_SHADOW_BLUR) * PRISM_SHADOW_ALPHA
    body = inside.astype(float)
    alpha = body + shadow * (1.0 - body)
    out = np.zeros(x.shape + (4,))
    grey = np.where(body > 0, np.clip(shade, 0.0, 1.0), 0.0)
    for k in range(3):
        out[..., k] = grey
    out[..., 3] = alpha
    return down(out)


def corner():
    x, y = grid(TILE)
    arm, thick, cham = CORNER_ARM, CORNER_THICK, CORNER_CHAMFER
    across = (y <= thick) & (x <= arm - y)          # the top arm, its end cut at 45 degrees
    along = (x <= thick) & (y <= arm - x)           # the left arm
    inside = (across | along) & (x + y >= cham)
    # distance to the outline, by the shape's edges
    edges = np.min([
        np.abs(y - 0.0) + np.where(x + y < cham, 1e3, 0.0),
        np.abs(x - 0.0) + np.where(x + y < cham, 1e3, 0.0),
        np.where(x > thick, np.abs(y - thick), 1e3),
        np.where(y > thick, np.abs(x - thick), 1e3),
        np.abs(x + y - cham) / np.sqrt(2.0),
        np.where(y <= thick, np.abs(x + y - arm) / np.sqrt(2.0), 1e3),
        np.where(x <= thick, np.abs(x + y - arm) / np.sqrt(2.0), 1e3),
    ], axis=0)
    outline = np.clip((CORNER_OUTLINE_WIDTH - edges) / 1.0 + 0.5, 0.0, 1.0)
    # brushed metal: a soft gradient along the arms, a shine over the inner part
    along_arm = np.clip(np.where(y <= thick, x, y) / arm, 0.0, 1.0)
    centre = np.where(y <= thick, np.abs(y - thick * 0.5), np.abs(x - thick * 0.5)) / (thick * 0.5)
    shine = np.exp(-(centre / 0.45) ** 2) * (1.0 - along_arm) ** 0.6 * CORNER_SHINE
    base = 0.78 - 0.18 * along_arm + shine
    shade = base * (1.0 - outline) + CORNER_OUTLINE * outline
    out = np.zeros(x.shape + (4,))
    for k in range(3):
        out[..., k] = np.clip(CORNER_SILVER[k] * shade / 0.80, 0.0, 1.0)
    out[..., 3] = inside * CORNER_ALPHA
    return down(out)


def atlas():
    sheet = np.zeros((TILE, TILE * TILES, 4))
    sheet[:, 0:TILE] = prism()
    sheet[:, TILE:2 * TILE] = corner()
    return Image.fromarray(np.round(np.clip(sheet, 0.0, 1.0) * 255.0).astype(np.uint8), "RGBA")


def preview(sheet, path):
    """The prism tinted green (the player's), red, blue and yellow, and the four
    corners round a panel, as the game draws them, enlarged, over navy."""
    pixels = np.asarray(sheet).astype(float) / 255.0
    tiles = [pixels[:, k * TILE:(k + 1) * TILE] for k in range(TILES)]
    scale = 3
    out = Image.new("RGB", (TILE * 4 * scale, TILE * 2 * scale), NAVY)

    def put(tile, tint, at, flip=(False, False)):
        t = tile.copy()
        if flip[0]:
            t = t[:, ::-1]
        if flip[1]:
            t = t[::-1]
        shade, col = t[..., :3], np.array(tint)
        rgb = np.where(shade <= 0.5, col * 2.0 * shade, col + (1.0 - col) * (2.0 * shade - 1.0))
        img = Image.fromarray(np.round(np.dstack([rgb, t[..., 3:]]) * 255).astype(np.uint8), "RGBA")
        img = img.resize((TILE * scale, TILE * scale), Image.Resampling.LANCZOS)
        out.paste(img, at, img)

    put(tiles[0], (0.15, 0.75, 0.25), (0, 0))
    put(tiles[0], (0.85, 0.12, 0.12), (TILE * scale, 0))
    put(tiles[0], (0.15, 0.30, 0.90), (0, TILE * scale))
    put(tiles[0], (0.95, 0.80, 0.15), (TILE * scale, TILE * scale))
    grey = (0.5, 0.5, 0.5)
    w = TILE * scale
    put(tiles[1], grey, (2 * w, 0))
    put(tiles[1], grey, (3 * w, 0), (True, False))
    put(tiles[1], grey, (2 * w, w), (False, True))
    put(tiles[1], grey, (3 * w, w), (True, True))
    out.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument("--asset-output", type=Path, default=root / "assets" / "minimap")
    parser.add_argument("--preview", type=Path, help="also write the sprites tinted and enlarged here")
    args = parser.parse_args()
    args.asset_output.mkdir(parents=True, exist_ok=True)
    sheet = atlas()
    sheet.save(args.asset_output / "minimap-atlas.png", optimize=True)
    print("wrote", args.asset_output / "minimap-atlas.png", sheet.size)
    if args.preview:
        preview(sheet, args.preview)
        print("wrote", args.preview)


if __name__ == "__main__":
    main()
