#!/usr/bin/env python3
"""Mark where a car's exhausts are, on its model, by hand.

    python3 tools/car_model.py exhausts [CAR]        # or
    python3 tools/car_exhausts.py [CAR] [--root DIR]

A window: every car in a list on the left; the car's body seen from behind
(the high level of detail of its car.fce, shaded) and, small on the right,
from the side and from above with the marks on them.

  - left click on the car     a new exhaust there, on the body's surface;
                              hold the button and it grows
  - left click on a mark      hold and it grows
  - right click on a mark     hold and it shrinks
  - middle click on a mark    (or d with the pointer on it) takes it away
  - m                         mirror on/off: a mark made, grown or shrunk on
                              one side is made, grown or shrunk on the other
  - c                         colour on/off: the body in the car's own picture
                              from the game (car00.tga, its texture), lit a
                              face at a time, so the paint, the lights and the
                              exhausts' cut-outs read -- grey otherwise
  - s, or Save                write them; Revert, or r, back to what is saved
  - the strip under the views the colour the flames burn: hue across it,
                              brightness up, a click on it picks one, and a
                              click on the swatch beside it puts back the
                              game's own (#f2380a, the file then naming none)

They are kept in the car's own folder as exhausts.json:

    {"flame": "#f2380a",
     "exhausts": [{"x": 0.42, "y": -0.31, "z": -2.27, "radius": 0.045}, ...]}

in the model's own space, metres -- car.fce's axes: x to the car's right, y up,
z forward (the rear is the smallest z).  A car with no file has no exhausts.
"flame" is left out of most files, the game's own #f2380a then; whatever the
file has is shown in the swatch, and written back as it was unless it is
picked again.
"""
import argparse
import io
import json
import math
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import car_perf  # noqa: E402
from car_perf import CARMODEL, CarFileError, hovering, present  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MARK = "#eb6834"
BODY = (0.62, 0.66, 0.72)
TYRE = (0.13, 0.13, 0.14)
FLAME = "#f2380a"   # the colour the flames burn where the car's file names none
FILE = "exhausts.json"
MIN_RADIUS, MAX_RADIUS, START_RADIUS = 0.008, 0.25, 0.035


def load_mesh(folder):
    """The high level of detail's triangles in the car's space, as numpy arrays:
    their corners, the (u, v) each corner is mapped to in the car's texture, and
    the colour a face falls back to when there is no texture to show -- the
    car's paint out of its palette by the page the triangle names (the game's
    rule, car_model.page_colours), and a tyre's black for the wheels, which
    share the body's page but are not painted with it."""
    import numpy as np
    import car_model
    car = car_perf.Car(Path(folder))
    model, _ = car_model.load_model(car)
    parts = car_model.lod_parts(model, "high") or list(model.parts)
    paint = car_model.page_colours(model)
    corners, uvs, colours = [], [], []
    for part in parts:
        wheel = "wheel" in part.name.lower()
        for index in range(part.first_triangle, min(part.first_triangle + part.triangles, len(model.triangles))):
            page, i0, i1, i2 = model.triangles[index][:4]
            try:
                corners.append([model.vertex(part, i + part.first_vertex) for i in (i0, i1, i2)])
            except IndexError:
                continue
            uvs.append(model.triangles[index][5:8])
            if wheel:
                colours.append(TYRE)
            elif paint:
                colours.append(paint[min(page, len(paint) - 1)])
            else:
                colours.append(BODY)   # a file with no palette: the same grey as ever
    return (car, np.array(corners, dtype=float), np.array(uvs, dtype=float).reshape(-1, 3, 2),
            np.array(colours, dtype=float).reshape(-1, 3))


def load_texture(car):
    """The car's own picture -- `car00.tga` out of its archive, the paint and
    the details its triangles are mapped into -- as RGB 0 to 1, or None where
    there is none to show (PIL's not here, or the file is not)."""
    import numpy as np
    import car_model
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        blob, _archive = car_model.model_bytes(car.folder, "car00.tga")
        picture = Image.open(io.BytesIO(blob)).convert("RGB")
    except (CarFileError, OSError, ValueError):
        return None
    return np.asarray(picture, dtype=float) / 255.0


def flame_rgb(value):
    """The colour a file's "flame" names, as (r, g, b) 0 to 1 -- "#f2380a",
    "f2380a", or three numbers 0 to 1 or 0 to 255, the ways the game's own
    reader takes them (native_exhausts.cpp) -- or None where it names none."""
    if value is None:
        return None
    if isinstance(value, str):
        text = value.strip().lstrip("#")
        if len(text) != 6:
            return None
        try:
            return tuple(int(text[at:at + 2], 16) / 255.0 for at in (0, 2, 4))
        except ValueError:
            return None
    if isinstance(value, (list, tuple)) and len(value) == 3:
        try:
            numbers = [float(c) for c in value]
        except (TypeError, ValueError):
            return None
        if max(numbers) > 1.0:
            numbers = [c / 255.0 for c in numbers]
        return tuple(min(1.0, max(0.0, c)) for c in numbers)
    return None


def flame_hex(rgb):
    """(r, g, b) 0 to 1 as the "#rrggbb" a file is written in."""
    return "#%02x%02x%02x" % tuple(int(round(min(1.0, max(0.0, c)) * 255)) for c in rgb)


def read_marks(folder):
    """The marks in the car's folder, and the colour the file gives its flames
    ("#f2380a" where it gives none -- the game's own default)."""
    path = Path(folder) / FILE
    if not path.exists():
        return [], None
    data = json.loads(path.read_text())
    marks = [dict(x=float(e["x"]), y=float(e["y"]), z=float(e["z"]), radius=float(e["radius"]))
             for e in data.get("exhausts", [])]
    return marks, data.get("flame")


def write_marks(folder, marks, flame=None):
    """The marks written back, with the flame colour the file had (it is the
    game's to read, not this tool's to keep, but losing it would be a surprise)."""
    path = Path(folder) / FILE
    data = {
        "about": "exhaust positions in the car model's own space (car.fce), metres: x right, y up, "
                 "z forward; radius in metres. \"flame\" is the colour the flames burn, \"#f2380a\" "
                 "where it is left out. Written by tools/car_exhausts.py.",
        "exhausts": [{"x": round(m["x"], 4), "y": round(m["y"], 4), "z": round(m["z"], 4),
                      "radius": round(m["radius"], 4)} for m in marks],
    }
    if flame:
        data["flame"] = flame
    path.write_text(json.dumps(data, indent=2) + "\n")
    return path


def pick(tris, u, v):
    """The point on the body's surface under (x=u, y=v) seen from behind: of the
    triangles over it, the one nearest the eye (the smallest z)."""
    import numpy as np
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    d = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
    ok = np.abs(d) > 1e-12
    d = np.where(ok, d, 1.0)
    w0 = ((b[:, 1] - c[:, 1]) * (u - c[:, 0]) + (c[:, 0] - b[:, 0]) * (v - c[:, 1])) / d
    w1 = ((c[:, 1] - a[:, 1]) * (u - c[:, 0]) + (a[:, 0] - c[:, 0]) * (v - c[:, 1])) / d
    w2 = 1.0 - w0 - w1
    inside = ok & (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
    if not inside.any():
        return None
    z = w0 * a[:, 2] + w1 * b[:, 2] + w2 * c[:, 2]
    z = np.where(inside, z, np.inf)
    return float(z.min())


def lit(tris, light, lowest=0.35):
    """How brightly each face is lit, `lowest` to 1: by how squarely it faces
    the light, as the game lights a car's body -- a face at a time."""
    import numpy as np
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    length = np.linalg.norm(n, axis=1)
    length[length == 0] = 1.0
    n /= length[:, None]
    return (lowest + (1.0 - lowest) * np.abs(n @ np.array(light)))[:, None]


def shaded(tris, light, colours=None):
    """A face's colour, lit: the body's grey, or a colour of its own (`colours`,
    a face's each -- the car's paint where there is no texture to show)."""
    import numpy as np
    base = np.array(BODY, dtype=float)[None, :] if colours is None else np.asarray(colours, dtype=float)
    return np.clip(base * lit(tris, light, 0.35 if colours is None else 0.60), 0.0, 1.0)


def textured_view(tris, uvs, texture, light, across, up, down, across_range, up_range, scale):
    """The car as the game draws it: each triangle filled with the part of its
    own texture its three corners are mapped into, its whole area lit as one
    face -- the way the game lights a body.  A picture (RGBA, 0 where the car is
    not) of the view along `up` across `across`: the car's two axes drawn, the
    picture's rows running up `up`, downwards when `down` (the view from above).
    Faces turned away are left out and farther faces go first, so nearer ones
    cover them."""
    import numpy as np
    columns = max(1, int(round((across_range[1] - across_range[0]) * scale)))
    rows = max(1, int(round((up_range[1] - up_range[0]) * scale)))
    image = np.zeros((rows, columns, 4), dtype=float)
    height, width, _ = texture.shape
    top = up_range[0] if down else up_range[1]     # the up the picture's first row is at
    sign = 1.0 if down else -1.0
    shade = lit(tris, light, 0.62)
    depth = 3 - across - up   # the axis the camera looks along
    # Pixel centres, once: a triangle's own box is a slice of these.
    middles = (np.arange(columns) + 0.5) / scale + across_range[0]
    rungs = top + (np.arange(rows) + 0.5) / scale * sign
    normal = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    # Which way a face looks is its winding's business, and a body's parts are
    # not all wound alike: turned to point away from the middle of the car, so
    # that "facing the camera" means the same for every part of it.
    middle = tris.reshape(-1, 3).mean(axis=0)
    outward = np.einsum("ij,ij->i", normal, tris.mean(axis=1) - middle)
    normal *= np.where(outward < 0.0, -1.0, 1.0)[:, None]
    # Farther faces first, and only those facing the camera: it stands at the
    # car's own +x from the side and -x from behind, and above it from above.
    facing = normal[:, depth] * (1.0 if down else -1.0) > 0.0
    order = np.argsort((1.0 if down else -1.0) * tris[:, :, depth].mean(axis=1))
    for index in order[facing[order]]:
        corner = tris[index][:, [across, up]]
        x0, x1 = corner[:, 0].min(), corner[:, 0].max()
        y0, y1 = corner[:, 1].min(), corner[:, 1].max()
        first = max(0, int((x0 - across_range[0]) * scale) - 1)
        last = min(columns, int((x1 - across_range[0]) * scale) + 2)
        if last <= first:
            continue
        row_a = (y0 - top) * scale * sign
        row_b = (y1 - top) * scale * sign
        first_row = max(0, int(min(row_a, row_b)) - 1)
        last_row = min(rows, int(max(row_a, row_b)) + 2)
        if last_row <= first_row or (last - first) * (last_row - first_row) < 1:
            continue   # nothing of it would show
        ax, ay = corner[1, 0] - corner[0, 0], corner[1, 1] - corner[0, 1]
        bx, by = corner[2, 0] - corner[0, 0], corner[2, 1] - corner[0, 1]
        det = ax * by - ay * bx
        if abs(det) < 1e-12:
            continue   # a face seen edge on
        # The test is in the car's own metres (middles and rungs are the places
        # the pixels stand for), so the rows' running down the picture does not
        # come into it.
        px = (middles[first:last] - corner[0, 0])[None, :]
        py = (rungs[first_row:last_row] - corner[0, 1])[:, None]
        l1 = (px * by - py * bx) / det
        l2 = (ax * py - ay * px) / det
        l0 = 1.0 - l1 - l2
        inside = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        if not inside.any():
            continue
        uv = uvs[index]
        u = l0 * uv[0][0] + l1 * uv[1][0] + l2 * uv[2][0]
        v = l0 * uv[0][1] + l1 * uv[1][1] + l2 * uv[2][1]
        column = np.clip((u * width).astype(np.int32), 0, width - 1)
        row = np.clip(((1.0 - v) * height).astype(np.int32), 0, height - 1)   # v runs up
        pixels = image[first_row:last_row, first:last]
        pixels[inside, :3] = texture[row, column][inside] * shade[index]
        pixels[inside, 3] = 1.0
    extent = (across_range[0], across_range[1], up_range[1] if down else up_range[0],
              up_range[0] if down else up_range[1])
    return image, extent


def run(root, start=None, show=True):
    import matplotlib
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    from matplotlib.patches import Circle, Rectangle
    from matplotlib.widgets import Button, RadioButtons
    import numpy as np

    root = str(root)
    folders = car_perf.car_folders(root)
    if not folders:
        raise SystemExit("no cars under %s" % root)
    state = {"index": folders.index(start) if start in folders else 0, "page": 0, "marks": [], "saved": [],
             "flame": None, "tris": None, "uvs": None, "texture": None, "colours": None, "colour": False,
             "car": None, "held": None,
             "button": None, "mirror": True, "radio": None, "list_ax": None, "hover": None, "dirty": False}
    per_page = 24
    state["page"] = state["index"] // per_page

    plt.rcParams["toolbar"] = "None"
    fig = plt.figure(figsize=(17, 9.6), facecolor=SURFACE)
    try:
        fig.canvas.manager.set_window_title("NFS III exhausts")
    except AttributeError:
        pass
    rear = fig.add_axes((0.2, 0.08, 0.5, 0.8))
    side = fig.add_axes((0.73, 0.5, 0.26, 0.36))
    top = fig.add_axes((0.73, 0.08, 0.26, 0.36))
    title = fig.text(0.2, 0.965, "", fontsize=15, color=INK, fontweight="bold", va="top")
    help_text = fig.text(0.2, 0.925, "", fontsize=8.5, color=INK_2, va="top")
    status = fig.text(0.73, 0.025, "", fontsize=8.5, color=INK_2)
    fig.text(0.008, 0.965, "Cars", fontsize=13, color=INK, fontweight="bold", va="top")
    page_text = fig.text(0.09, 0.075, "", fontsize=8.5, color=INK_2, ha="center")
    # The window's own colour behind the lines round the views, drawn under the
    # figure, so they can be drawn again in place -- a shorter title, a new "saved
    # to" -- without the body behind them being drawn whole again (see repaint).
    chrome_texts = (title, help_text, status, page_text)
    chrome_blanks = [Rectangle(rect[:2], rect[2], rect[3], transform=fig.transFigure, facecolor=SURFACE,
                               edgecolor="none", zorder=-1)
                     for rect in ((0.195, 0.936, 0.53, 0.040),      # the title
                                  (0.195, 0.902, 0.53, 0.035),      # the help line
                                  (0.725, 0.014, 0.27, 0.028),      # the sizes, or what was saved
                                  (0.045, 0.062, 0.09, 0.030))]     # page n of n
    for blank in chrome_blanks:
        fig.add_artist(blank)

    def folder_path():
        return os.path.join(root, folders[state["index"]])

    def show_list():
        if state["radio"] is not None:
            state["radio"].ax.remove()
        first = state["page"] * per_page
        names = folders[first:first + per_page]
        ax = fig.add_axes((0.005, 0.11, 0.17, 0.78))
        ax.set_facecolor(SURFACE)
        for spine in ax.spines.values():
            spine.set_visible(False)
        labels = []
        for f in names:
            mark = " (%d)" % len(read_marks(os.path.join(root, f))[0]) if (Path(root) / f / FILE).exists() else ""
            labels.append("%s%s" % (f, mark))
        active = state["index"] - first if first <= state["index"] < first + per_page else 0
        # useblit off: the dots are left out of a draw when it is on, and drawn
        # by the widget afterwards -- the list is drawn on its own here (repaint).
        radio = RadioButtons(ax, labels, active=active, activecolor=MARK, useblit=False)
        radio.drawon = False   # a click draws the car it opens anyway
        for label in radio.labels:
            label.set_fontsize(8.5)
            label.set_color(INK)
        radio.on_clicked(lambda label: open_car(first + labels.index(label)))
        state["radio"] = radio
        state["list_ax"] = ax
        pages = (len(folders) + per_page - 1) // per_page
        page_text.set_text("page %d of %d" % (state["page"] + 1, pages))

    def page(step):
        pages = (len(folders) + per_page - 1) // per_page
        state["page"] = (state["page"] + step) % pages
        show_list()
        repaint(cars=True)

    up = hovering(fig, Button(fig.add_axes((0.008, 0.9, 0.08, 0.03)), "^ up", color="#e4e3df",
                              hovercolor="#d4d3cf"))
    down = hovering(fig, Button(fig.add_axes((0.093, 0.9, 0.08, 0.03)), "v down", color="#e4e3df",
                                hovercolor="#d4d3cf"))
    up.on_clicked(lambda _: page(-1))
    down.on_clicked(lambda _: page(1))

    def faces(order):
        """The colour each triangle is drawn in, in the order it is drawn: the
        car's paint, or none at all for the body's own grey (the colour off)."""
        colours = state["colours"] if state["colour"] else None
        return None if colours is None else colours[order]

    # The three views: the axes drawn across and up, whether up runs down the
    # picture (from above looks along -y, so the car's right goes down), and the
    # light each is lit by.
    VIEWS = ((rear, 0, 1, False, (0.3, 0.6, -0.75)),
             (side, 2, 1, False, (-0.8, 0.5, 0.2)),
             (top, 2, 0, True, (0.2, 0.9, 0.3)))

    def draw_mesh():
        tris = state["tris"]
        for ax in (rear, side, top):
            ax.clear()
            ax.set_facecolor(SURFACE)
            ax.set_aspect("equal")
            ax.tick_params(colors=INK_2, labelsize=7, length=0)
            for spine in ax.spines.values():
                spine.set_color("#e4e3df")
        if tris is None or not len(tris):
            rear.text(0.5, 0.5, "no model", transform=rear.transAxes, ha="center", color=INK_2)
            return
        lo, hi = tris.reshape(-1, 3).min(axis=0), tris.reshape(-1, 3).max(axis=0)
        pad = 0.08
        texture = state["texture"] if state["colour"] else None
        for ax, across, up, down, light in VIEWS:
            across_range = (lo[across] - pad, hi[across] + pad)
            up_range = (lo[up] - pad, hi[up] + pad)
            ax.set_xlim(*across_range)
            ax.set_ylim(*(up_range[::-1] if down else up_range))
            if texture is not None:
                # The car in its own colours, the texture drawn into every face.
                # A picture's worth: the rear view is the big one on the screen,
                # the two beside it a third of that.
                largest = max(across_range[1] - across_range[0], up_range[1] - up_range[0])
                scale = min(340.0 if ax is rear else 190.0, 1400.0 / largest)
                picture, extent = textured_view(tris, state["uvs"], texture, light, across, up, down,
                                                across_range, up_range, scale)
                ax.imshow(picture, extent=extent, origin="upper", interpolation="bilinear", zorder=0)
                continue
            # Otherwise the body shaded grey (or its paint, where a file has no
            # texture), far faces first.
            depth = 3 - across - up
            order = np.argsort((1.0 if down else -1.0) * tris[:, :, depth].mean(axis=1))
            ax.add_collection(PolyCollection(tris[order][:, :, [across, up]],
                                             facecolors=shaded(tris[order], light, faces(order)),
                                             edgecolors=(0, 0, 0, 0.12) if ax is rear else "none",
                                             linewidths=0.2))
        rear.set_title("from behind: click to mark, hold to size", loc="left", fontsize=10, color=INK)
        rear.set_xlabel("x (m, the car's right)", fontsize=8, color=INK_2)
        rear.set_ylabel("y (m, up)", fontsize=8, color=INK_2)
        side.set_title("side (front to the right)", loc="left", fontsize=9, color=INK)
        top.set_title("from above (front to the right)", loc="left", fontsize=9, color=INK)

    marks_artists = []

    def repaint(cars=False, texts=True):
        """The marks and the lines round them drawn again over the body as it was
        last drawn, and put on the screen: a mark grown while the button is held,
        the one under the pointer, a save -- none of them the body itself, whose
        thousands of triangles are the slow part of a draw."""
        canvas = fig.canvas
        if state.get("backgrounds") is None:
            canvas.draw_idle()   # nothing to draw over yet: the one whole draw of it
            return
        renderer = canvas.get_renderer()
        for ax, background in zip((rear, side, top), state["backgrounds"]):
            canvas.restore_region(background)
        for artist in marks_artists:
            artist.axes.draw_artist(artist)
        if texts:
            for blank in chrome_blanks:   # the lines' own old selves wiped
                blank.draw(renderer)
            for text in chrome_texts:
                text.draw(renderer)
        if cars:
            state["list_ax"].draw(renderer)   # which cars carry how many marks
        present(canvas)

    def on_draw(_event):
        # The views without the marks (animated: left out of a full draw), kept to
        # draw the marks over again instead of drawing the body (see repaint).
        state["backgrounds"] = [fig.canvas.copy_from_bbox(ax.bbox) for ax in (rear, side, top)]
        for artist in marks_artists:
            artist.axes.draw_artist(artist)

    def draw_marks(cars=False, mark_only=False, saved_to=None):
        for artist in marks_artists:
            artist.remove()
        marks_artists.clear()
        for n, m in enumerate(state["marks"]):
            edge = INK if n == state["hover"] else MARK
            marks_artists.append(rear.add_patch(Circle((m["x"], m["y"]), m["radius"], facecolor=(0.92, 0.41, 0.2, 0.35),
                                                       edgecolor=edge, linewidth=1.6, zorder=5, animated=True)))
            marks_artists.append(side.add_patch(Circle((m["z"], m["y"]), m["radius"], facecolor=MARK, edgecolor="none",
                                                       zorder=5, animated=True)))
            marks_artists.append(top.add_patch(Circle((m["z"], m["x"]), m["radius"], facecolor=MARK, edgecolor="none",
                                                      zorder=5, animated=True)))
        if mark_only:   # grown under the held button: the sizes come after the release
            repaint(texts=False)
            return
        changed = state["marks"] != state["saved"]
        title.set_text("%s  -  %s%s" % (state["car"].display_name() or folders[state["index"]], folders[state["index"]],
                                         "  (changed, not saved)" if changed else ""))
        help_text.set_text("left click: a new exhaust / hold to grow   right click on one: hold to shrink   "
                           "middle click or d: remove   m: mirror %s   c: colour %s   s: save   r: revert   "
                           "the strip: the flames' colour (the swatch: the game's own)"
                           % ("ON" if state["mirror"] else "off", "ON" if state["colour"] else "off"))
        status.set_text(saved_to or "%d exhaust%s%s" % (
            len(state["marks"]), "" if len(state["marks"]) == 1 else "s",
            "" if not state["marks"] else ": " + ", ".join("r %.0f mm" % (m["radius"] * 1000) for m in state["marks"])))
        repaint(cars=cars)

    def open_car(index):
        state["index"] = index
        try:
            car, tris, uvs, colours = load_mesh(folder_path())
        except (CarFileError, OSError, ValueError) as error:
            car, tris, uvs, colours = None, None, None, None
            status.set_text("no model: %s" % error)
        if car is None:
            class Nameless:
                def display_name(self):
                    return ""
            car = Nameless()
        state["car"], state["tris"], state["colours"] = car, tris, colours
        state["uvs"], state["texture"] = uvs, load_texture(car) if car is not None else None
        state["saved"], state["flame"] = read_marks(folder_path())
        state["marks"] = [dict(m) for m in state["saved"]]
        state["hover"] = None
        state["backgrounds"] = None
        marks_artists.clear()   # the views are cleared and drawn again
        draw_mesh()
        draw_marks()
        draw_flame()

    def mark_at(x, y):
        best, best_d = None, None
        for n, m in enumerate(state["marks"]):
            d = math.hypot(m["x"] - x, m["y"] - y)
            if d <= max(m["radius"], 0.02) and (best_d is None or d < best_d):
                best, best_d = n, d
        return best

    def twin(n):
        """The mark on the other side of this one (mirror), if there is one."""
        m = state["marks"][n]
        for k, o in enumerate(state["marks"]):
            if k != n and abs(o["x"] + m["x"]) < 0.02 and abs(o["y"] - m["y"]) < 0.02 and abs(o["z"] - m["z"]) < 0.03:
                return k
        return None

    def on_press(event):
        if event.inaxes is not rear or event.xdata is None or state["tris"] is None:
            return
        x, y = event.xdata, event.ydata
        n = mark_at(x, y)
        if event.button == 2:
            if n is not None:
                remove(n)
            return
        if event.button == 1 and n is None:
            z = pick(state["tris"], x, y)
            if z is None:
                status.set_text("not on the car")
                fig.canvas.draw_idle()
                return
            state["marks"].append(dict(x=x, y=y, z=z, radius=START_RADIUS))
            n = len(state["marks"]) - 1
            if state["mirror"] and abs(x) > 0.03:
                z2 = pick(state["tris"], -x, y)
                if z2 is not None:
                    state["marks"].append(dict(x=-x, y=y, z=z2, radius=START_RADIUS))
        if n is None:
            return
        state["held"], state["button"] = n, event.button
        timer.start()
        draw_marks()

    def on_release(event):
        held = state["held"]
        state["held"] = None
        timer.stop()
        if held is not None:
            draw_marks()   # the title and the sizes below brought up to date

    def remove(n):
        k = twin(n) if state["mirror"] else None
        for index in sorted({n} | ({k} if k is not None else set()), reverse=True):
            state["marks"].pop(index)
        state["hover"] = None
        draw_marks()

    def tick():
        n = state["held"]
        if n is None or n >= len(state["marks"]):
            return
        step = 1.06 if state["button"] == 1 else 1 / 1.06
        targets = [n]
        k = twin(n) if state["mirror"] else None
        if k is not None:
            targets.append(k)
        for t in targets:
            m = state["marks"][t]
            m["radius"] = min(MAX_RADIUS, max(MIN_RADIUS, m["radius"] * step))
        draw_marks(mark_only=True)

    def on_move(event):
        hover = mark_at(event.xdata, event.ydata) if event.inaxes is rear and event.xdata is not None else None
        if hover != state["hover"]:
            state["hover"] = hover
            draw_marks(mark_only=True)

    def save(_=None):
        path = write_marks(folder_path(), state["marks"], state["flame"])
        state["saved"] = [dict(m) for m in state["marks"]]
        show_list()   # the counts in the list of cars
        draw_marks(cars=True, saved_to="saved %d to %s" % (len(state["marks"]), os.path.relpath(path)))

    def revert(_=None):
        state["marks"] = [dict(m) for m in state["saved"]]
        draw_marks()

    def on_key(event):
        if event.key == "m":
            state["mirror"] = not state["mirror"]
            draw_marks()
        elif event.key == "c":
            state["colour"] = not state["colour"]
            state["backgrounds"] = None   # the body is drawn again, so what to draw the marks over is not
            marks_artists.clear()         # the views are cleared and drawn again, as a new car's are
            draw_mesh()
            draw_marks()
        elif event.key == "s":
            save()
        elif event.key == "r":
            revert()
        elif event.key in ("d", "delete", "backspace") and state["hover"] is not None:
            remove(state["hover"])

    save_button = hovering(fig, Button(fig.add_axes((0.2, 0.015, 0.07, 0.04)), "Save", color=MARK,
                                       hovercolor="#d95926"))
    save_button.label.set_color("white")
    revert_button = hovering(fig, Button(fig.add_axes((0.28, 0.015, 0.07, 0.04)), "Revert", color="#e4e3df",
                                         hovercolor="#d4d3cf"))
    save_button.on_clicked(save)
    revert_button.on_clicked(revert)

    # The colour the car's flames burn, which its file may name: a strip of them
    # -- hue across, brightness up, full saturation, the way a flame's own is --
    # a click on it picks one, the swatch beside it says which and a click there
    # puts back the game's own (#f2380a, the file then naming none).
    import matplotlib.colors as mcolors
    flame_ax = fig.add_axes((0.397, 0.012, 0.235, 0.048))
    hue = np.linspace(0.0, 1.0, 160)[None, :]
    value = np.linspace(1.0, 0.22, 36)[:, None] * np.ones_like(hue)
    strip = mcolors.hsv_to_rgb(np.dstack([hue * np.ones_like(value), np.ones_like(hue) * np.ones_like(value), value]))
    flame_ax.imshow(strip, extent=(0.0, 1.0, 0.22, 1.0), origin="upper", aspect="auto", interpolation="bilinear")
    flame_ax.set_xticks([])
    flame_ax.set_yticks([])
    for spine in flame_ax.spines.values():
        spine.set_color("#e4e3df")
    # The ring that says where the colour stands on the strip, two of them so
    # that one shows on the bright half of it and the other on the dark.
    flame_ring = flame_ax.plot([0.5], [1.0], marker="o", markersize=7.5, markerfacecolor="none",
                               markeredgecolor=INK, markeredgewidth=1.1)[0]
    flame_marker = flame_ax.plot([0.5], [1.0], marker="o", markersize=5.4, markerfacecolor="none",
                                 markeredgecolor="#ffffff", markeredgewidth=1.1)[0]
    fig.text(0.392, 0.021, "flame", fontsize=7.5, color=INK_2, ha="right")
    swatch = fig.add_axes((0.638, 0.012, 0.052, 0.048))
    swatch.set_xticks([])
    swatch.set_yticks([])
    swatch_text = swatch.text(0.5, 0.5, "", fontsize=7, ha="center", va="center")

    def draw_flame():
        """The swatch, what it says, and the ring on the strip where the
        colour stands, from the state -- the strip itself left as it is."""
        named = flame_rgb(state["flame"])
        rgb = named or flame_rgb(FLAME)
        swatch.set_facecolor(rgb)
        hexed = flame_hex(rgb)
        swatch_text.set_text(hexed)
        dark = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
        swatch_text.set_color(INK if dark > 0.55 else "#fcfcfb")
        h, s, v = mcolors.rgb_to_hsv(rgb)
        flame_marker.set_data([h], [max(v, 0.22)])
        flame_ring.set_data([h], [max(v, 0.22)])
        renderer = fig.canvas.get_renderer()
        for ax in (flame_ax, swatch):
            ax.draw(renderer)
        present(fig.canvas)

    def pick_flame(event):
        """A click on the strip: the colour under it; on the swatch: the game's
        own back, the file naming none."""
        if event.inaxes is flame_ax and event.button == 1 and event.xdata is not None:
            value_here = max(0.22, min(1.0, event.ydata))
            state["flame"] = flame_hex(mcolors.hsv_to_rgb((max(0.0, min(1.0, event.xdata)), 1.0, value_here)))
            draw_flame()
        elif event.inaxes is swatch and event.button == 1:
            state["flame"] = None
            draw_flame()

    timer = fig.canvas.new_timer(interval=40)
    timer.add_callback(tick)
    fig.canvas.mpl_connect("button_press_event", pick_flame)
    fig.canvas.mpl_connect("button_press_event", on_press)
    fig.canvas.mpl_connect("button_release_event", on_release)
    fig.canvas.mpl_connect("motion_notify_event", on_move)
    fig.canvas.mpl_connect("key_press_event", on_key)
    fig.canvas.mpl_connect("draw_event", on_draw)

    show_list()
    open_car(state["index"])
    state.update(fig=fig, press=on_press, release=on_release, tick=tick, save=save, open_car=open_car, key=on_key,
                 pick=pick_flame)
    if matplotlib.get_backend().lower() != "agg":
        if show:
            plt.show()
        else:
            fig.show()   # from another window: its loop runs this one too
    return state


def main(argv=None):
    parser = argparse.ArgumentParser(description="Mark a car's exhausts on its model.")
    parser.add_argument("car", nargs="?", help="the car to open on (a folder, e.g. diab or traffic/pursuit/0005)")
    parser.add_argument("--root", default=str(CARMODEL),
                        help="the cars' folder (default: the repository's, wherever this is run)")
    args = parser.parse_args(argv)
    run(args.root, args.car)
    return 0


if __name__ == "__main__":
    sys.exit(main())
