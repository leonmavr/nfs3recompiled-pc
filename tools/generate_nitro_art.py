#!/usr/bin/env python3
"""Bake the nitro bottle's atlas from the SVG it is drawn in.

The picture is the one in `assets/nitro/nos_bottle.svg` -- a NOS bottle with a
grey outline, a blue valve and a blue label whose window is cut out -- and the
game needs two things from it, so the atlas holds both:

  the mask    the bottle's silhouette filled solid, which is what clips the
              liquid to the bottle: only what is inside the outline is ever
              painted, and the label's window and the NOS lettering cut out of
              it let the liquid show through, as a real bottle's do;
  the art     the bottle exactly as drawn, drawn over the liquid: its
              semi-transparent label tints what is behind it, its opaque parts
              hide it.

Both tiles are the same size, the mask first, a gutter clear of texels round
each.  The port reads the atlas (native_nitro_gauge.cpp), keeps the mask white
with its own alpha and the art's colours, and tints the mask by the level's
colour.

    python3 tools/generate_nitro_art.py [--preview FILE]

The renderer is cairosvg (pip install cairosvg); the atlas is committed, so
this is only needed when the drawing changes.  --preview writes the atlas
enlarged, the mask tinted, to look at.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "nitro" / "nos_bottle.svg"
ATLAS = ROOT / "assets" / "nitro" / "nitro-atlas.png"

TILE_W, TILE_H = 184, 465        # half the drawing's own size: 368 by 930
GUTTER = 8


def outline_path(text):
    """The `d` of the bottle's outline, the one path the mask is made of."""
    found = re.search(r'<path\s+id="bottle-outline"[^>]*?\sd="([^"]+)"', text, re.S)
    if not found:
        found = re.search(r'<path[^>]*?\sd="([^"]+)"', text, re.S)
    if not found:
        raise SystemExit("no outline path in %s" % SOURCE)
    return " ".join(found.group(1).split())


def mask_svg(text, d):
    """The same drawing with only the outline left, filled solid."""
    view = re.search(r'viewBox="([^"]+)"', text)
    box = view.group(1) if view else "0 0 %d %d" % (TILE_W * 2, TILE_H * 2)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s">'
        '<path d="%s" fill="#ffffff"/></svg>' % (box, d)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", metavar="FILE", help="write the atlas enlarged, to look at")
    parser.add_argument("--scale", type=int, default=4, help="render scale before the tile is reduced (4)")
    args = parser.parse_args()

    try:
        import cairosvg
        from PIL import Image
    except ImportError as missing:
        raise SystemExit("needs cairosvg and Pillow: pip install cairosvg Pillow (%s)" % missing)

    text = SOURCE.read_text()
    d = outline_path(text)

    def render(svg, by_url):
        if by_url:
            data = cairosvg.svg2png(url=str(SOURCE), output_width=TILE_W * args.scale,
                                    output_height=TILE_H * args.scale)
        else:
            data = cairosvg.svg2png(bytestring=svg.encode("utf-8"), output_width=TILE_W * args.scale,
                                    output_height=TILE_H * args.scale)
        from io import BytesIO
        image = Image.open(BytesIO(data)).convert("RGBA")
        return image.resize((TILE_W, TILE_H), Image.LANCZOS)

    art = render(None, True)
    mask = render(mask_svg(text, d), False)

    width = TILE_W * 2 + GUTTER * 3
    height = TILE_H + GUTTER * 2
    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    # The mask: its silhouette white, its alpha the same, so the port can tint it.
    solid = Image.new("RGBA", mask.size, (255, 255, 255, 0))
    solid.putalpha(mask.getchannel("A"))
    atlas.paste(solid, (GUTTER, GUTTER))
    atlas.paste(art, (GUTTER * 2 + TILE_W, GUTTER))
    atlas.save(ATLAS)
    print("wrote %s, %dx%d (mask then art, %dx%d each, gutter %d)"
          % (ATLAS.relative_to(ROOT), width, height, TILE_W, TILE_H, GUTTER))

    if args.preview:
        shots = []
        for level in (1.0, 0.5, 0.15):
            shot = Image.new("RGBA", (TILE_W, TILE_H), (0, 0, 0, 0))
            filled = int(TILE_H * level)
            if filled:
                liquid = Image.new("RGBA", (TILE_W, filled), (71, 224, 0, 255))
                liquid = Image.new("RGBA", (TILE_W, filled), (71, 224, 0, 255))
                shaped = Image.new("RGBA", (TILE_W, filled), (0, 0, 0, 0))
                shaped.paste(liquid, (0, 0), mask.getchannel("A").crop((0, TILE_H - filled, TILE_W, TILE_H)))
                shot.paste(shaped, (0, TILE_H - filled))
            shots.append(Image.alpha_composite(shot, art))
        zoom = 2
        gap = 12
        width = TILE_W * len(shots) + gap * (len(shots) + 1)
        preview = Image.new("RGBA", (width, TILE_H + gap * 2), (28, 28, 38, 255))
        for k, shot in enumerate(shots):
            preview.paste(shot, (gap + k * (TILE_W + gap), gap), shot)
        preview.resize((width * zoom, (TILE_H + gap * 2) * zoom), Image.LANCZOS).save(args.preview)
        print("wrote the preview %s" % args.preview)

if __name__ == "__main__":
    sys.exit(main())
