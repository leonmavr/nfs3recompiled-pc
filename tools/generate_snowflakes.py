#!/usr/bin/env python3
"""Generate the snow-on-the-lens sprites: the atlas the game loads.

Pixel-art clumps of wet snow, each in several shrink stages for the end of its
life, packed into assets/snowflakes/snowflake-atlas.png (flake_atlas, below;
the game's side is snowOnScreen in src/lib/thrashrenderer.cpp, and its
`snow::kTile`, `kStages`, `kBig` and `kSmall` must match TILE, STAGES, BIG and
SMALL here).  `--preview` also writes the sprites enlarged, for a look.

    python3 tools/generate_snowflakes.py [--preview sheet.png]
"""

import argparse
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image


SIZE = 768   # the atlas's side, as the game checks it


# ------------------------------------------------------------------ pixel flakes (the atlas)
#
# The game's sprites: pixel-art clumps of wet snow ("C3" of the review sheets).
# Twelve tones, lightest in the middle and darker and thinner every step out to
# a wet blue-grey edge -- never a ring lighter than the one inside it -- a few
# clumped gaps in the big ones and loose crumbs round them, a glint.  Each is
# baked in STAGES shrink stages: the same outline and the same dark edge, down
# to SHRINK_MOST of its size, for the flake melting at the end of its life.
#
# The atlas: 768x768 of TILE-pixel tiles; column = sprite (BIG first, then
# SMALL), row = shrink stage.  The game reads it texel by texel, unfiltered.

TILE = 32
STAGES = 6
SHRINK_MOST = 0.5
BIG, SMALL = 9, 9
BIG_SIZE, SMALL_SIZE = 22, 14       # the blob's span in pixels (its tile has room for the crumbs)
TONES = 12
ALPHA = (0.84, 0.62, 1.3)           # middle, edge, and the curve between
EDGE_RADIUS = 0.40                  # the edge's mean radius, of the blob's span
GAPS = 0.22                         # how much of the coarse noise is a gap (big ones)
CRUMBS = (0.18, 0.08)               # big, small: how thick they lie round the edge
CRUMB_PAIRS = 0.25                  # of the big ones' crumbs, how many are two pixels wide
JITTER = 0.03                       # each pixel's colour, either way
PALE_WHITE, PALE_GREY, PALE_LIGHT = (246, 248, 250), (196, 204, 210), (222, 228, 232)
PALE_CYAN, WET_EDGE = (200, 232, 242), (150, 172, 184)
FLAKE_SEEDS = (11, 23, 37, 41, 59, 67, 73, 89, 97)


def coarse_noise(seed, cells):
    grid = np.random.default_rng(seed).random((cells, cells))
    image = Image.fromarray((grid * 255).astype(np.uint8)).resize((TILE, TILE), Image.Resampling.BICUBIC)
    return np.asarray(image, np.float32) / 255.0


def flake(seed, span, big, shrink):
    """One TILE x TILE flake: `span` pixels across, shrunk by `shrink` (0 to SHRINK_MOST)."""
    pick = random.Random(seed)
    ys, xs = np.mgrid[0:TILE, 0:TILE] + 0.5
    dx, dy = (xs - TILE / 2) / span, (ys - TILE / 2) / span
    theta = np.arctan2(dy, dx)
    edge = np.ones((TILE, TILE))
    for waves in (2, 3, 4):
        edge += pick.uniform(0.05, 0.12) / waves ** 0.6 * np.sin(waves * theta + pick.uniform(0, math.tau))
    radius = EDGE_RADIUS * pick.uniform(0.9, 1.05)
    v = np.hypot(dx, dy) / (radius * edge) / (1.0 - shrink)   # 0 middle, 1 edge -- the edge drawn in as it melts
    tint = pick.choice((PALE_WHITE, PALE_LIGHT, PALE_CYAN))
    middle = PALE_GREY if tint != PALE_CYAN else PALE_CYAN
    gaps = coarse_noise(seed, 6) * 0.7 + coarse_noise(seed + 1, 11) * 0.3
    wobble = (coarse_noise(seed + 7, 5) - 0.5) * 0.12
    rnd = np.random.default_rng(seed)
    chance, jitter = rnd.random((TILE, TILE)), rnd.uniform(-JITTER, JITTER, (TILE, TILE))
    alpha0, alpha1, curve = ALPHA

    def tone(at):
        q = round(min(max(at, 0.0), 1.0) * TONES) / TONES
        colour = np.array(tint) + (np.array(middle) - np.array(tint)) * min(q / 0.7, 1.0)
        colour = colour + (np.array(WET_EDGE) - colour) * max(0.0, (q - 0.55) / 0.45)
        return colour, alpha0 + (alpha1 - alpha0) * q ** curve

    out = np.zeros((TILE, TILE, 4), np.float32)
    crumbs = (CRUMBS[0] if big else CRUMBS[1]) * (1.0 - shrink / SHRINK_MOST * 0.5)   # melting first
    for y in range(TILE):
        for x in range(TILE):
            at = v[y, x]
            if at > 1.0:
                reach = 1.35 if big else 1.25
                odds = crumbs * (reach - at) / (reach - 1.0) * 1.6 if big else crumbs
                if at < reach and chance[y, x] < odds:
                    colour, _ = tone(0.9)
                    out[y, x] = (*colour, (110 if not big else 90 + 60 * chance[y, x] / max(odds, 1e-6)))
                    if big and x + 1 < TILE and (chance[y, x] * 7.0) % 1.0 < CRUMB_PAIRS and out[y, x + 1, 3] == 0:
                        out[y, x + 1] = (*colour, 100)
                continue
            if big:
                gap = GAPS if 0.42 < at < 0.8 else GAPS * 0.7 if at >= 0.8 else 0.0
                if gaps[y, x] < gap:
                    continue
            colour, a = tone(at + (wobble[y, x] if big else 0.0))
            if not big and at > 0.9:
                a *= 0.6
            out[y, x, :3] = np.clip(colour * (1.0 + jitter[y, x] * big), 0, 255)
            out[y, x, 3] = a * 255
    gx, gy = int(TILE / 2 - span * pick.uniform(0.06, 0.14)), int(TILE / 2 - span * pick.uniform(0.08, 0.16))
    if out[gy, gx, 3] > 0:
        out[gy, gx] = (255, 255, 255, 250)
        if out[gy, gx + 1, 3] > 0 and pick.random() < 0.7:
            out[gy, gx + 1] = (255, 255, 255, 190)
    return Image.fromarray(out.round().clip(0, 255).astype(np.uint8), "RGBA")


def flake_atlas():
    """The game's atlas, and the sprites in it by name."""
    atlas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    named = []
    for column in range(BIG + SMALL):
        big = column < BIG
        seed = FLAKE_SEEDS[column % len(FLAKE_SEEDS)] + (0 if big else 1000)
        for stage in range(STAGES):
            image = flake(seed, BIG_SIZE if big else SMALL_SIZE, big, SHRINK_MOST * stage / (STAGES - 1))
            atlas.paste(image, (column * TILE, stage * TILE))
            if stage == 0:
                named.append((f"{'big' if big else 'small'}-{column % BIG + 1:02d}", image))
    return atlas, named


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--asset-output", type=Path, default=Path("assets/snowflakes"),
                        help="directory for the atlas the game loads")
    parser.add_argument("--preview", type=Path, help="also write the atlas's sprites enlarged over grey here")
    args = parser.parse_args()
    args.asset_output.mkdir(parents=True, exist_ok=True)
    atlas, _ = flake_atlas()
    atlas.save(args.asset_output / "snowflake-atlas.png", optimize=True)
    if args.preview:
        used = atlas.crop((0, 0, (BIG + SMALL) * TILE, STAGES * TILE))
        sheet = Image.new("RGBA", used.size, (55, 62, 70, 255))
        sheet.alpha_composite(used)
        sheet.resize((used.width * 3, used.height * 3), Image.Resampling.NEAREST).convert("RGB").save(args.preview)


if __name__ == "__main__":
    main()
