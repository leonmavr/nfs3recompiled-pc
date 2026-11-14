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
  - s, or Save                write them; Revert, or r, back to what is saved

They are kept in the car's own folder as exhausts.json:

    {"exhausts": [{"x": 0.42, "y": -0.31, "z": -2.27, "radius": 0.045}, ...]}

in the model's own space, metres -- car.fce's axes: x to the car's right, y up,
z forward (the rear is the smallest z).  A car with no file has no exhausts.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import car_perf  # noqa: E402
from car_perf import CARMODEL, CarFileError  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MARK = "#eb6834"
BODY = (0.62, 0.66, 0.72)
FILE = "exhausts.json"
MIN_RADIUS, MAX_RADIUS, START_RADIUS = 0.008, 0.25, 0.035


def load_mesh(folder):
    """The high level of detail's triangles in the car's space, as numpy arrays."""
    import numpy as np
    import car_model
    car = car_perf.Car(Path(folder))
    model, _ = car_model.load_model(car)
    parts = car_model.lod_parts(model, "high") or list(model.parts)
    corners = []
    for part in parts:
        for index in range(part.first_triangle, min(part.first_triangle + part.triangles, len(model.triangles))):
            _page, i0, i1, i2 = model.triangles[index][:4]
            try:
                corners.append([model.vertex(part, i + part.first_vertex) for i in (i0, i1, i2)])
            except IndexError:
                continue
    return car, np.array(corners, dtype=float)


def read_marks(folder):
    path = Path(folder) / FILE
    if not path.exists():
        return []
    data = json.loads(path.read_text())
    return [dict(x=float(e["x"]), y=float(e["y"]), z=float(e["z"]), radius=float(e["radius"]))
            for e in data.get("exhausts", [])]


def write_marks(folder, marks):
    path = Path(folder) / FILE
    data = {
        "about": "exhaust positions in the car model's own space (car.fce), metres: x right, y up, "
                 "z forward; radius in metres. Written by tools/car_exhausts.py.",
        "exhausts": [{"x": round(m["x"], 4), "y": round(m["y"], 4), "z": round(m["z"], 4),
                      "radius": round(m["radius"], 4)} for m in marks],
    }
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


def shaded(tris, light):
    import numpy as np
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    length = np.linalg.norm(n, axis=1)
    length[length == 0] = 1.0
    n /= length[:, None]
    shade = 0.35 + 0.65 * np.abs(n @ np.array(light))
    return [(BODY[0] * s, BODY[1] * s, BODY[2] * s) for s in shade]


def run(root, start=None, show=True):
    import matplotlib
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    from matplotlib.patches import Circle
    from matplotlib.widgets import Button, RadioButtons
    import numpy as np

    root = str(root)
    folders = car_perf.car_folders(root)
    if not folders:
        raise SystemExit("no cars under %s" % root)
    state = {"index": folders.index(start) if start in folders else 0, "page": 0, "marks": [], "saved": [],
             "tris": None, "car": None, "held": None, "button": None, "mirror": True, "radio": None,
             "hover": None, "dirty": False}
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
            mark = " (%d)" % len(read_marks(os.path.join(root, f))) if (Path(root) / f / FILE).exists() else ""
            labels.append("%s%s" % (f, mark))
        active = state["index"] - first if first <= state["index"] < first + per_page else 0
        radio = RadioButtons(ax, labels, active=active, activecolor=MARK)
        for label in radio.labels:
            label.set_fontsize(8.5)
            label.set_color(INK)
        radio.on_clicked(lambda label: open_car(first + labels.index(label)))
        state["radio"] = radio
        pages = (len(folders) + per_page - 1) // per_page
        page_text.set_text("page %d of %d" % (state["page"] + 1, pages))

    def page(step):
        pages = (len(folders) + per_page - 1) // per_page
        state["page"] = (state["page"] + step) % pages
        show_list()
        fig.canvas.draw_idle()

    up = Button(fig.add_axes((0.008, 0.9, 0.08, 0.03)), "^ up", color="#e4e3df", hovercolor="#d4d3cf")
    down = Button(fig.add_axes((0.093, 0.9, 0.08, 0.03)), "v down", color="#e4e3df", hovercolor="#d4d3cf")
    up.on_clicked(lambda _: page(-1))
    down.on_clicked(lambda _: page(1))

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
        # From behind: x across, y up, nearest (smallest z) drawn last.
        order = np.argsort(-tris[:, :, 2].mean(axis=1))
        rear.add_collection(PolyCollection(tris[order][:, :, [0, 1]], facecolors=shaded(tris[order], (0.3, 0.6, -0.75)),
                                           edgecolors=(0, 0, 0, 0.12), linewidths=0.2))
        rear.set_title("from behind: click to mark, hold to size", loc="left", fontsize=10, color=INK)
        rear.set_xlabel("x (m, the car's right)", fontsize=8, color=INK_2)
        rear.set_ylabel("y (m, up)", fontsize=8, color=INK_2)
        # The side (z across, front to the right) and from above (z across, x down).
        order = np.argsort(tris[:, :, 0].mean(axis=1))
        side.add_collection(PolyCollection(tris[order][:, :, [2, 1]], facecolors=shaded(tris[order], (-0.8, 0.5, 0.2)),
                                           edgecolors="none"))
        side.set_title("side (front to the right)", loc="left", fontsize=9, color=INK)
        order = np.argsort(tris[:, :, 1].mean(axis=1))
        top.add_collection(PolyCollection(tris[order][:, :, [2, 0]], facecolors=shaded(tris[order], (0.2, 0.9, 0.3)),
                                          edgecolors="none"))
        top.set_title("from above (front to the right)", loc="left", fontsize=9, color=INK)
        lo, hi = tris.reshape(-1, 3).min(axis=0), tris.reshape(-1, 3).max(axis=0)
        pad = 0.08
        rear.set_xlim(lo[0] - pad, hi[0] + pad)
        rear.set_ylim(lo[1] - pad, hi[1] + pad)
        side.set_xlim(lo[2] - pad, hi[2] + pad)
        side.set_ylim(lo[1] - pad, hi[1] + pad)
        top.set_xlim(lo[2] - pad, hi[2] + pad)
        top.set_ylim(hi[0] + pad, lo[0] - pad)

    marks_artists = []

    def blit_marks():
        """Only the marks drawn again, over the views as they were last drawn
        (blitting): a mark grown while the button is held, or the one under the
        pointer, without the whole window -- the body's thousand triangles --
        being drawn again every 40 ms."""
        canvas = fig.canvas
        if state.get("backgrounds") is None:
            canvas.draw_idle()
            return
        for ax, background in zip((rear, side, top), state["backgrounds"]):
            canvas.restore_region(background)
        for artist in marks_artists:
            artist.axes.draw_artist(artist)
        for ax in (rear, side, top):
            canvas.blit(ax.bbox)

    def on_draw(_event):
        # The views without the marks (animated: left out of a full draw), kept to blit over.
        state["backgrounds"] = [fig.canvas.copy_from_bbox(ax.bbox) for ax in (rear, side, top)]
        for artist in marks_artists:
            artist.axes.draw_artist(artist)

    def draw_marks(full=True):
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
        if not full:
            blit_marks()
            return
        changed = state["marks"] != state["saved"]
        title.set_text("%s  -  %s%s" % (state["car"].display_name() or folders[state["index"]], folders[state["index"]],
                                         "  (changed, not saved)" if changed else ""))
        help_text.set_text("left click: a new exhaust / hold to grow   right click on one: hold to shrink   "
                           "middle click or d: remove   m: mirror %s   s: save   r: revert"
                           % ("ON" if state["mirror"] else "off"))
        status.set_text("%d exhaust%s%s" % (len(state["marks"]), "" if len(state["marks"]) == 1 else "s",
                                            "" if not state["marks"] else ": " + ", ".join(
                                                "r %.0f mm" % (m["radius"] * 1000) for m in state["marks"])))
        fig.canvas.draw_idle()

    def open_car(index):
        state["index"] = index
        try:
            car, tris = load_mesh(folder_path())
        except (CarFileError, OSError, ValueError) as error:
            car, tris = None, None
            status.set_text("no model: %s" % error)
        if car is None:
            class Nameless:
                def display_name(self):
                    return ""
            car = Nameless()
        state["car"], state["tris"] = car, tris
        state["saved"] = read_marks(folder_path())
        state["marks"] = [dict(m) for m in state["saved"]]
        state["hover"] = None
        state["backgrounds"] = None
        marks_artists.clear()   # the views are cleared and drawn again
        draw_mesh()
        draw_marks()

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
        draw_marks(full=False)

    def on_move(event):
        hover = mark_at(event.xdata, event.ydata) if event.inaxes is rear and event.xdata is not None else None
        if hover != state["hover"]:
            state["hover"] = hover
            draw_marks(full=False)

    def save(_=None):
        path = write_marks(folder_path(), state["marks"])
        state["saved"] = [dict(m) for m in state["marks"]]
        status.set_text("saved %s" % path)
        show_list()
        draw_marks()
        status.set_text("saved %d to %s" % (len(state["marks"]), os.path.relpath(path)))
        fig.canvas.draw_idle()

    def revert(_=None):
        state["marks"] = [dict(m) for m in state["saved"]]
        draw_marks()

    def on_key(event):
        if event.key == "m":
            state["mirror"] = not state["mirror"]
            draw_marks()
        elif event.key == "s":
            save()
        elif event.key == "r":
            revert()
        elif event.key in ("d", "delete", "backspace") and state["hover"] is not None:
            remove(state["hover"])

    save_button = Button(fig.add_axes((0.2, 0.015, 0.07, 0.04)), "Save", color=MARK, hovercolor="#d95926")
    save_button.label.set_color("white")
    revert_button = Button(fig.add_axes((0.28, 0.015, 0.07, 0.04)), "Revert", color="#e4e3df", hovercolor="#d4d3cf")
    save_button.on_clicked(save)
    revert_button.on_clicked(revert)

    timer = fig.canvas.new_timer(interval=40)
    timer.add_callback(tick)
    fig.canvas.mpl_connect("button_press_event", on_press)
    fig.canvas.mpl_connect("button_release_event", on_release)
    fig.canvas.mpl_connect("motion_notify_event", on_move)
    fig.canvas.mpl_connect("key_press_event", on_key)
    fig.canvas.mpl_connect("draw_event", on_draw)

    show_list()
    open_car(state["index"])
    state.update(fig=fig, press=on_press, release=on_release, tick=tick, save=save, open_car=open_car, key=on_key)
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
