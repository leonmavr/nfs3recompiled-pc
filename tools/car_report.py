#!/usr/bin/env python3
"""Performance report sheets for NFS III's cars, from each car's own data.

    python3 tools/car_perf.py report [CAR ...] [--out DIR]      # through car_perf.py
    python3 tools/car_perf.py edit [CAR]                        # the editor window
    python3 tools/car_report.py [CARMODEL_DIR] [-o OUT_DIR] [--cars diab,f355,...]
    python3 tools/car_report.py [CARMODEL_DIR] --edit [CAR]

The editor: every car in a list on the left (a page at a time), the charts in
the middle, the specs on the right as boxes -- change one and press Enter and
the charts follow at once; Save writes the car (through car_perf.py, car.viv
and the loose carp.txt, the gearbox's rpm per m/s kept in step), Cancel puts
it back.  It needs a window (Tk or Qt for matplotlib).

CARMODEL_DIR defaults to gamedata/carmodel.  Each car folder's performance
data is its carp.txt -- loose in the folder, or packed in its car.viv (a BIGF
archive) -- and its name comes from fedata.eng.  For each car OUT_DIR
(default car_reports/) gets <folder>.png:

  - torque and power against engine speed, the redline marked;
  - the gearbox: engine speed against road speed in each gear, the shift at
    the redline drawn from one gear into the next;
  - the push at the wheels in each gear against road speed (torque times the
    gear's ratio, the final drive and its efficiency, over the wheel's radius),
    the best of them drawn over all, where each gear hands over to the next;
  - the game's own acceleration table by speed (the one its AI drives by),
    with 0-100 and 0-200 km/h times worked out of it;
  - the specs: mass, gears, final drive, redline, top speed, brakes, grip.

and compare.png sets every car side by side (top speed, 0-100 km/h, 0-200 km/h,
power to weight), with summary.csv holding the numbers.

carp.txt's fields are named in the file itself ("torque curve (size 41) in
256rpm increments(10)", ...); the ones read here are keyed by their number.
"""

import argparse
import csv
import math
import os
import struct
import sys

import matplotlib

if not any(a in ("--edit", "edit") for a in sys.argv[1:]):
    matplotlib.use("Agg")   # pictures only; the editor needs a window
import matplotlib.pyplot as plt  # noqa: E402

# The chart's colours: a light surface, ink for text, and the series in a
# fixed order (validated for colour-vision deficiency; see the dataviz palette).
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
MARK = "#d03b3b"   # the redline


# --- reading -----------------------------------------------------------------

def viv_files(path):
    """The files in a BIGF archive, name -> bytes."""
    data = open(path, "rb").read()
    if data[:4] != b"BIGF":
        return {}
    count, = struct.unpack_from(">I", data, 8)
    files, at = {}, 16
    for _ in range(count):
        offset, size = struct.unpack_from(">II", data, at)
        at += 8
        end = data.index(b"\0", at)
        name = data[at:end].decode("latin-1")
        at = end + 1
        files[name.lower()] = data[offset:offset + size]
    return files


def carp_text(folder):
    loose = os.path.join(folder, "carp.txt")
    if os.path.exists(loose):
        return open(loose, "rb").read().decode("latin-1")
    viv = os.path.join(folder, "car.viv")
    if os.path.exists(viv):
        packed = viv_files(viv).get("carp.txt")
        if packed:
            return packed.decode("latin-1")
    return None


def parse_carp(text):
    """carp.txt: a field's name ending in (n), its value on the next line."""
    lines = [line.strip() for line in text.replace("\r", "").split("\n")]
    fields = {}
    for i, line in enumerate(lines[:-1]):
        if line.endswith(")") and "(" in line:
            key = line[line.rindex("(") + 1:-1]
            if key.isdigit():
                value = lines[i + 1]
                fields[int(key)] = [float(v) for v in value.split(",") if v.strip() != ""] if value else []
    return fields


def car_name(folder, fallback):
    """The full name in fedata.eng: the third of its strings."""
    path = os.path.join(folder, "fedata.eng")
    try:
        data = open(path, "rb").read()
        offsets = []
        at = 0x2f
        while at + 4 <= len(data):
            offset, = struct.unpack_from("<I", data, at)
            if not (0 < offset < len(data)):
                break
            offsets.append(offset)
            at += 4
        strings = [data[o:data.index(b"\0", o)].decode("latin-1") for o in offsets[:3]]
        if len(strings) >= 3 and strings[2].strip():
            return strings[2].strip()
    except (OSError, ValueError, struct.error):
        pass
    return fallback


# --- working out -------------------------------------------------------------

class Car:
    def __init__(self, folder, fields):
        self.folder = os.path.basename(folder.rstrip("/"))
        self.name = car_name(folder, self.folder)
        f = lambda k, d=None: fields.get(k, d)
        one = lambda k, d=0.0: (fields.get(k) or [d])[0]
        self.mass = one(2)
        self.gear_count = int(one(3))            # reverse, neutral and the forward gears
        self.rpm_per_speed = f(7, [])            # each gear's rpm for each m/s
        self.ratios = f(8, [])
        self.efficiency = f(9, [])
        self.torque = f(10, [])                  # Nm, every 256 rpm from 0
        self.final = one(11, 1.0)
        self.min_rpm = one(12)
        self.redline = one(13)
        self.top_speed = one(14)                 # m/s
        self.speed_cap = one(15)
        self.front_drive = one(16)
        self.abs = bool(one(17))
        self.brake = one(18)                     # m/s^2
        self.front_brake = one(19)
        self.wheelbase = one(24)
        self.front_grip = one(25)
        self.lateral_grip = one(30)
        self.downforce = one(31)
        self.tyres_front = f(35, [])
        self.tyres_rear = f(36, [])
        self.ai_accel = sum((fields.get(k, []) for k in range(67, 75)), [])   # m/s^2 at each m/s
        self.forward = [g for g in range(2, min(self.gear_count, len(self.ratios), len(self.rpm_per_speed)))
                        if self.ratios[g] > 0 and self.rpm_per_speed[g] > 0]

    def torque_at(self, rpm):
        if not self.torque:
            return 0.0
        x = rpm / 256.0
        i = int(math.floor(x))
        if i >= len(self.torque) - 1:
            return self.torque[-1]
        if i < 0:
            return self.torque[0]
        t = x - i
        return self.torque[i] * (1 - t) + self.torque[i + 1] * t

    def wheel_radius(self, gear):
        """From the game's own rpm per m/s: rpm = v * ratio * final * 60 / (2 pi r)."""
        return self.ratios[gear] * self.final * 60.0 / (2 * math.pi * self.rpm_per_speed[gear])

    def push(self, gear, speed):
        """Newtons at the wheels in a gear at a speed (m/s); 0 past the redline."""
        rpm = max(speed * self.rpm_per_speed[gear], self.min_rpm)
        if rpm > self.redline:
            return 0.0
        eff = self.efficiency[gear] if gear < len(self.efficiency) else 1.0
        return self.torque_at(rpm) * self.ratios[gear] * self.final * eff / self.wheel_radius(gear)

    def peak_power(self):
        best = (0.0, 0.0)
        rpm = 256.0
        while rpm <= self.redline:
            kw = self.torque_at(rpm) * rpm * 2 * math.pi / 60 / 1000
            best = max(best, (kw, rpm))
            rpm += 32
        return best

    def peak_torque(self):
        top = max(range(len(self.torque)), key=lambda i: self.torque[i] if i * 256 <= self.redline else -1)
        return self.torque[top], top * 256

    def time_to(self, kmh):
        """Seconds from rest to a speed by the game's acceleration table."""
        target = kmh / 3.6
        if not self.ai_accel:
            return None
        t, v, dv = 0.0, 0.0, 0.05
        while v < target:
            i = int(v)
            if i + 1 >= len(self.ai_accel):
                return None
            a = self.ai_accel[i] + (self.ai_accel[i + 1] - self.ai_accel[i]) * (v - i)
            if a <= 0.01:
                return None
            t += dv / a
            v += dv
        return t


# --- drawing -------------------------------------------------------------------

def style(ax, title, xlabel, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, loc="left", fontsize=11, color=INK, fontweight="bold", pad=8)
    ax.set_xlabel(xlabel, fontsize=9, color=INK_2)
    ax.set_ylabel(ylabel, fontsize=9, color=INK_2)
    ax.tick_params(colors=INK_2, labelsize=8, length=0)
    ax.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)


def gear_label(car, g):
    return "%d" % (g - 1)


def draw_torque(ax, car):
    style(ax, "Torque", "engine speed (rpm)", "torque (Nm)")
    rpms = car_rpms(car)
    ax.plot(rpms, [car.torque_at(r) for r in rpms], color=SERIES[0], linewidth=2)
    peak_t, at_t = car.peak_torque()
    ax.plot([at_t], [peak_t], "o", color=SERIES[0], markersize=7, markeredgecolor=SURFACE, markeredgewidth=2)
    ax.annotate("%.0f Nm at %d rpm" % (peak_t, at_t), (at_t, peak_t), xytext=(6, 6),
                textcoords="offset points", fontsize=8, color=INK)
    ax.axvline(car.redline, color=MARK, linewidth=1.2, linestyle="--")
    ax.annotate("redline %d" % car.redline, (car.redline, ax.get_ylim()[0]), xytext=(-4, 6),
                textcoords="offset points", fontsize=8, color=INK_2, ha="right")


def draw_power(ax, car):
    style(ax, "Power", "engine speed (rpm)", "power (kW)")
    rpms = car_rpms(car)
    ax.plot(rpms, [car.torque_at(r) * r * 2 * math.pi / 60 / 1000 for r in rpms], color=SERIES[0], linewidth=2)
    peak_p, at_p = car.peak_power()
    ax.plot([at_p], [peak_p], "o", color=SERIES[0], markersize=7, markeredgecolor=SURFACE, markeredgewidth=2)
    ax.annotate("%.0f kW (%.0f hp) at %d rpm" % (peak_p, peak_p * 1.341, at_p), (at_p, peak_p), xytext=(-6, 8),
                textcoords="offset points", fontsize=8, color=INK, ha="right")
    ax.axvline(car.redline, color=MARK, linewidth=1.2, linestyle="--")


def top_kmh(car):
    return max(car.top_speed, car.speed_cap, 1.0) * 3.6


def draw_gearbox(ax, car):
    """Engine rpm against road speed in each gear, the shift at the redline."""
    style(ax, "Gearbox: engine speed in each gear", "road speed (km/h)", "engine speed (rpm)")
    top = top_kmh(car)
    for n, g in enumerate(car.forward):
        cut = car.redline / car.rpm_per_speed[g]
        v = [0, min(cut, top / 3.6)]
        ax.plot([x * 3.6 for x in v], [x * car.rpm_per_speed[g] for x in v], color=SERIES[n % len(SERIES)],
                linewidth=2, label="gear %s" % gear_label(car, g))
        if n + 1 < len(car.forward) and cut < top / 3.6:
            nxt = car.forward[n + 1]
            ax.plot([cut * 3.6, cut * 3.6], [car.redline, cut * car.rpm_per_speed[nxt]], color=INK_2,
                    linewidth=1, linestyle=":")
        end = min(cut, top / 3.6)
        ax.annotate(gear_label(car, g), (end * 3.6, end * car.rpm_per_speed[g]), xytext=(3, 2),
                    textcoords="offset points", fontsize=8, color=INK)
    ax.axhline(car.redline, color=MARK, linewidth=1.2, linestyle="--")
    ax.set_xlim(0, top * 1.02)
    ax.legend(fontsize=8, frameon=False, labelcolor=INK, loc="lower right")


def draw_push(ax, car):
    """The push at the wheels in each gear, the best gear over all."""
    style(ax, "Push at the wheels in each gear", "road speed (km/h)", "force (kN)")
    top = top_kmh(car)
    speeds = [s * 0.25 for s in range(0, int(top / 3.6 * 4) + 2)]
    best = [0.0] * len(speeds)
    for n, g in enumerate(car.forward):
        f = [car.push(g, s) / 1000 for s in speeds]
        best = [max(a, b) for a, b in zip(best, f)]
        pts = [(s * 3.6, x) for s, x in zip(speeds, f) if x > 0]
        if pts:
            ax.plot([p[0] for p in pts], [p[1] for p in pts], color=SERIES[n % len(SERIES)], linewidth=2,
                    label="gear %s" % gear_label(car, g))
    ax.plot([s * 3.6 for s in speeds], best, color=INK, linewidth=1, linestyle="--", label="best gear")
    ax.set_xlim(0, top * 1.02)
    ax.set_ylim(bottom=0)
    ax.legend(fontsize=8, frameon=False, labelcolor=INK, loc="upper right")


def draw_accel(ax, car):
    """The game's acceleration table by speed, with the times out of it."""
    style(ax, "The game's acceleration by speed", "road speed (km/h)", "acceleration (m/s²)")
    ax.plot([i * 3.6 for i in range(len(car.ai_accel))], car.ai_accel, color=SERIES[0], linewidth=2)
    ax.set_xlim(0, max(top_kmh(car) * 1.02, 10))
    ax.set_ylim(bottom=0)
    times = [(k, car.time_to(k)) for k in (100, 160, 200)]
    note = "   ".join("0-%d: %s" % (k, "%.1f s" % t if t else "-") for k, t in times)
    ax.text(0.98, 0.95, note, transform=ax.transAxes, ha="right", va="top", fontsize=9, color=INK)


def car_rpms(car):
    return [r for r in range(0, int(car.redline) + 257, 64) if r <= max(car.redline, 256)]


def spec_rows(car):
    peak_p, at_p = car.peak_power()
    peak_t, at_t = car.peak_torque()
    tyre = lambda t: "%d/%dR%d" % tuple(int(x) for x in t[:3]) if len(t) >= 3 else "-"
    drive = "rear" if car.front_drive <= 0.01 else "front" if car.front_drive >= 0.99 else \
        "all (%.0f%% front)" % (car.front_drive * 100)
    return [
        ("Mass", "%.0f kg" % car.mass),
        ("Power to weight", "%.0f kW/t" % (peak_p / car.mass * 1000) if car.mass else "-"),
        ("Peak torque", "%.0f Nm at %d rpm" % (peak_t, at_t)),
        ("Peak power", "%.0f kW (%.0f hp) at %d rpm" % (peak_p, peak_p * 1.341, at_p)),
        ("Redline / idle", "%d / %d rpm" % (car.redline, car.min_rpm)),
        ("Gears", "%d forward" % len(car.forward)),
        ("Ratios", " ".join("%.2f" % car.ratios[g] for g in car.forward)),
        ("Final drive", "%.3f" % car.final),
        ("Top speed", "%.0f km/h" % (car.top_speed * 3.6)),
        ("Drive", drive),
        ("Brakes", "%.1f m/s² (%.0f%% front)%s" % (car.brake, car.front_brake * 100, ", ABS" if car.abs else "")),
        ("Grip", "lateral %.2f, front bias %.2f" % (car.lateral_grip, car.front_grip)),
        ("Downforce", "%.5f" % car.downforce),
        ("Wheelbase", "%.2f m" % car.wheelbase),
        ("Tyres", "%s front, %s rear" % (tyre(car.tyres_front), tyre(car.tyres_rear))),
    ]


def car_sheet(car, out):
    fig, axes = plt.subplots(2, 3, figsize=(16, 9.2), facecolor=SURFACE)
    fig.suptitle(car.name, x=0.012, y=0.985, ha="left", fontsize=16, color=INK, fontweight="bold")
    fig.text(0.012, 0.952, "%s  -  from carp.txt" % car.folder, fontsize=9, color=INK_2)
    draw_torque(axes[0][0], car)
    draw_power(axes[0][1], car)
    draw_gearbox(axes[0][2], car)
    draw_push(axes[1][0], car)
    draw_accel(axes[1][1], car)
    ax = axes[1][2]
    ax.axis("off")
    ax.set_title("Specs", loc="left", fontsize=11, color=INK, fontweight="bold", pad=8)
    for i, (k, v) in enumerate(spec_rows(car)):
        y = 0.96 - i * 0.064
        ax.text(0.0, y, k, transform=ax.transAxes, fontsize=9.5, color=INK_2, va="top")
        ax.text(0.38, y, v, transform=ax.transAxes, fontsize=9.5, color=INK, va="top")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = os.path.join(out, car.folder.replace("/", "_") + ".png")
    fig.savefig(path, dpi=110, facecolor=SURFACE)
    plt.close(fig)
    return path


def compare_sheet(cars, out):
    names = [c.name for c in cars]
    for c in cars:
        c.label = "%s (%s)" % (c.name, c.folder) if names.count(c.name) > 1 else c.name
    stats = []
    for c in cars:
        p, _ = c.peak_power()
        stats.append((c, c.top_speed * 3.6, c.time_to(100), c.time_to(200), p / c.mass * 1000 if c.mass else 0))
    panels = [
        ("Top speed (km/h)", 1, False, "%.0f"),
        ("0-100 km/h (s), the game's table", 2, True, "%.1f"),
        ("0-200 km/h (s), the game's table", 3, True, "%.1f"),
        ("Power to weight (kW/t)", 4, False, "%.0f"),
    ]
    height = max(4.0, 0.28 * len(cars) + 1.6)
    fig, axes = plt.subplots(1, 4, figsize=(18, height), facecolor=SURFACE)
    fig.suptitle("Every car side by side", x=0.01, y=0.995, ha="left", fontsize=15, color=INK, fontweight="bold")
    for ax, (title, k, lower_better, fmt) in zip(axes, panels):
        rows = [(s[0].label, s[k]) for s in stats if s[k]]
        rows.sort(key=lambda r: r[1], reverse=not lower_better)
        rows.reverse()   # best at the top
        style(ax, title + ("  (lower is better)" if lower_better else ""), "", "")
        ax.grid(True, axis="x", color=GRID)
        ax.grid(False, axis="y")
        y = range(len(rows))
        ax.barh(list(y), [r[1] for r in rows], color=SERIES[0], height=0.62)
        ax.set_yticks(list(y))
        ax.set_yticklabels([r[0] for r in rows], fontsize=8, color=INK)
        for i, r in enumerate(rows):
            ax.text(r[1], i, " " + fmt % r[1], va="center", fontsize=7.5, color=INK_2)
        ax.set_xlim(0, max(r[1] for r in rows) * 1.15 if rows else 1)
    fig.tight_layout(rect=(0, 0, 1, 0.985))
    path = os.path.join(out, "compare.png")
    fig.savefig(path, dpi=110, facecolor=SURFACE)
    plt.close(fig)

    with open(os.path.join(out, "summary.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["folder", "name", "mass_kg", "peak_power_kw", "peak_torque_nm", "redline_rpm", "gears",
                    "final_drive", "top_speed_kmh", "zero_100_s", "zero_200_s", "kw_per_tonne"])
        for c, top, t100, t200, pw in stats:
            p, _ = c.peak_power()
            t, _ = c.peak_torque()
            w.writerow([c.folder, c.name, "%.0f" % c.mass, "%.1f" % p, "%.0f" % t, "%d" % c.redline,
                        len(c.forward), "%.3f" % c.final, "%.0f" % top, "%.2f" % t100 if t100 else "",
                        "%.2f" % t200 if t200 else "", "%.0f" % pw])
    return path


# --- the editor -------------------------------------------------------------

class Fields:
    """The editor's value boxes, its own rather than matplotlib's TextBox,
    which on some setups took no keys at all and redrew the whole window for
    every key, every click and every pointer move.  A click on a box makes it
    the one being typed in (the canvas given the keyboard), the first key typed
    replaces its value, Backspace takes a character off, Enter or Tab or a click
    elsewhere hands the value on, Escape puts it back.  Only the box is drawn
    again as you type (blitting)."""

    TYPED = "0123456789.,-+eE "

    def __init__(self, fig, on_submit):
        import matplotlib as mpl
        for key in list(mpl.rcParams):
            if key.startswith("keymap."):
                mpl.rcParams[key] = []   # no key of the window's own (s saves, q quits, l, g, ...)
        self.fig = fig
        self.on_submit = on_submit
        self.boxes = []          # [ax, text artist, value]
        self.active = None
        self.buffer = ""
        self.fresh = False
        fig.canvas.mpl_connect("button_press_event", self._press)
        fig.canvas.mpl_connect("key_press_event", self._key)

    def add(self, rect, label):
        ax = self.fig.add_axes(rect)
        ax.set_xticks([])
        ax.set_yticks([])
        ax.set_facecolor(SURFACE)
        for spine in ax.spines.values():
            spine.set_color(INK_2)
        ax.text(-0.04, 0.5, label, transform=ax.transAxes, ha="right", va="center", fontsize=9, color=INK_2)
        text = ax.text(0.04, 0.5, "", transform=ax.transAxes, ha="left", va="center", fontsize=9, color=INK)
        self.boxes.append([ax, text, ""])
        return len(self.boxes) - 1

    def set(self, n, value):
        self.boxes[n][2] = value
        if n != self.active:
            self.boxes[n][1].set_text(value)

    def _show(self, n):
        ax, text, value = self.boxes[n]
        typing = n == self.active
        text.set_text((self.buffer + "|") if typing else value)
        ax.set_facecolor("#f0efec" if typing else SURFACE)
        renderer = self.fig.canvas.get_renderer() if hasattr(self.fig.canvas, "get_renderer") else None
        try:
            ax.draw(renderer)
            self.fig.canvas.blit(ax.bbox)
        except Exception:   # no renderer yet, or a backend that cannot blit
            self.fig.canvas.draw_idle()

    def _focus(self):
        canvas = self.fig.canvas
        try:
            if hasattr(canvas, "get_tk_widget"):
                canvas.get_tk_widget().focus_set()
            elif hasattr(canvas, "setFocus"):
                canvas.setFocus()
        except Exception:
            pass

    def _finish(self, submit):
        n = self.active
        if n is None:
            return
        self.active = None
        typed = self.buffer.strip()
        self._show(n)
        if submit and typed and typed != self.boxes[n][2]:
            self.on_submit(n, typed)

    def _press(self, event):
        for n, (ax, _text, value) in enumerate(self.boxes):
            if event.inaxes is ax:
                if self.active != n:
                    self._finish(True)
                    self.active = n
                    self.buffer = value
                    self.fresh = True
                    self._focus()
                    self._show(n)
                return
        self._finish(True)

    def _key(self, event):
        n = self.active
        if n is None or not event.key:
            return
        key = event.key
        if key in ("enter", "return", "tab", "kp_enter"):
            self._finish(True)
            return
        if key == "escape":
            self._finish(False)
            return
        if key in ("backspace", "delete"):
            self.buffer = "" if self.fresh else self.buffer[:-1]
        elif len(key) == 1 and key in self.TYPED:
            self.buffer = key if self.fresh else self.buffer + key
        elif key.startswith("shift+") and len(key) == 7 and key[-1] in self.TYPED:
            self.buffer = key[-1] if self.fresh else self.buffer + key[-1]
        else:
            return
        self.fresh = False
        self._show(n)



# What the editor lets you change: its label, carp.txt's field number (or
# "torque x", the whole torque curve scaled), how it is shown from the field's
# value and read back.
EDITABLE = [
    ("Mass (kg)", 2, None),
    ("Torque x", "torque", None),
    ("Redline (rpm)", 13, None),
    ("Idle (rpm)", 12, None),
    ("Gear ratios", 8, "ratios"),
    ("Final drive", 11, None),
    ("Top speed (km/h)", 14, "kmh"),
    ("Speed cap (km/h)", 15, "kmh"),
    ("Front drive (0-1)", 16, None),
    ("Brakes (m/s²)", 18, None),
    ("Brake bias front", 19, None),
    ("Lateral grip", 30, None),
    ("Front grip bias", 25, None),
    ("Downforce", 31, None),
]
SHORT = {2: "mass", 8: "ratios", 10: "torque", 11: "final", 12: "min rpm", 13: "redline", 14: "vmax",
         15: "speed cap", 16: "front drive", 18: "brake decel", 19: "brake bias", 25: "front grip",
         30: "grip", 31: "downforce", 77: "ratios auto", 79: "final auto"}


def rolling_of(fields):
    """The wheel's rolling circumference from the gearing as it stands (car_perf.py's)."""
    ratios, v2r, final = fields.get(8, []), fields.get(7, []), (fields.get(11) or [0])[0]
    for ratio, speed_ratio in zip(ratios, v2r):
        if ratio > 0 and speed_ratio > 0 and final > 0:
            return ratio * final * 60.0 / speed_ratio
    return None


def edit(carmodel, start=None):
    """A window: every car in a list on the left, its charts in the middle and
    its specs on the right as boxes to change -- each change (Enter, or a click
    elsewhere) drawn at once; Save writes the car's carp.txt (car.viv and the
    loose copy, through car_perf.py), Cancel puts it back as it was."""
    import importlib
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    car_perf = importlib.import_module("car_perf")
    from matplotlib.widgets import Button, RadioButtons

    # Every car's folder; each one's archive is read only when it is shown (reading all
    # 75 car.viv to check them first held the window up for seconds as it opened).
    folders = car_perf.car_folders(carmodel)
    if not folders:
        raise SystemExit("no cars with a carp.txt under %s" % carmodel)
    state = {
        "index": folders.index(start) if start in folders else 0,
        "page": 0,
        "original": {},     # folder -> fields as on disk
        "fields": {},       # folder -> fields as edited
        "torque": {},       # folder -> the torque factor
        "radio": None,
    }
    per_page = 22
    state["page"] = state["index"] // per_page

    def load(folder):
        if folder not in state["original"]:
            fields = parse_carp(carp_text(os.path.join(carmodel, folder)) or "")
            state["original"][folder] = fields
            state["fields"][folder] = {k: list(v) for k, v in fields.items()}
            state["torque"][folder] = 1.0
        return state["fields"][folder]

    def current():
        return folders[state["index"]]

    def changed(folder):
        if folder not in state["original"]:
            return False
        a, b = state["original"][folder], state["fields"][folder]
        return any(a.get(k) != b.get(k) for k in SHORT) or state["torque"][folder] != 1.0

    def car_now():
        folder = current()
        fields = dict(load(folder))
        factor = state["torque"][folder]
        if factor != 1.0:
            fields[10] = [v * factor for v in fields[10]]
        car = Car(os.path.join(carmodel, folder), fields)
        car.folder = folder
        if car.name == os.path.basename(folder):
            car.name = folder
        return car

    plt.rcParams["toolbar"] = "None"
    fig = plt.figure(figsize=(16, 9), facecolor=SURFACE)
    try:
        fig.canvas.manager.set_window_title("NFS III cars")
    except AttributeError:
        pass
    chart_axes = {
        "torque": fig.add_axes((0.215, 0.56, 0.17, 0.33)),
        "power": fig.add_axes((0.425, 0.56, 0.17, 0.33)),
        "gearbox": fig.add_axes((0.635, 0.56, 0.17, 0.33)),
        "push": fig.add_axes((0.215, 0.08, 0.17, 0.33)),
        "accel": fig.add_axes((0.425, 0.08, 0.17, 0.33)),
    }
    readout = fig.add_axes((0.635, 0.08, 0.17, 0.36))
    title = fig.text(0.215, 0.965, "", fontsize=16, color=INK, fontweight="bold", va="top")
    subtitle = fig.text(0.215, 0.93, "", fontsize=9, color=INK_2, va="top")
    status = fig.text(0.825, 0.025, "", fontsize=8.5, color=INK_2)

    # The list of cars, a page at a time.
    fig.text(0.008, 0.965, "Cars", fontsize=13, color=INK, fontweight="bold", va="top")
    list_box = [0.005, 0.11, 0.16, 0.78]
    page_text = fig.text(0.08, 0.075, "", fontsize=8.5, color=INK_2, ha="center")

    def show_list():
        if state["radio"] is not None:
            state["radio"].ax.remove()
        first = state["page"] * per_page
        names = folders[first:first + per_page]
        ax = fig.add_axes(list_box)
        ax.set_facecolor(SURFACE)
        for side in ax.spines.values():
            side.set_visible(False)
        labels = []
        for f in names:
            name = car_name(os.path.join(carmodel, f), "")
            labels.append("%s%s%s" % ("* " if changed(f) else "", f, ("  " + name[:22]) if name else ""))
        active = state["index"] - first if first <= state["index"] < first + per_page else 0
        radio = RadioButtons(ax, labels, active=active, activecolor=SERIES[0])
        for label in radio.labels:
            label.set_fontsize(8.5)
            label.set_color(INK)
        radio.on_clicked(lambda label: pick(first + labels.index(label)))
        state["radio"] = radio
        pages = (len(folders) + per_page - 1) // per_page
        page_text.set_text("page %d of %d" % (state["page"] + 1, pages))

    def page(step):
        pages = (len(folders) + per_page - 1) // per_page
        state["page"] = (state["page"] + step) % pages
        show_list()
        fig.canvas.draw_idle()

    up = Button(fig.add_axes((0.008, 0.9, 0.07, 0.03)), "^ up", color=GRID, hovercolor="#d4d3cf")
    down = Button(fig.add_axes((0.083, 0.9, 0.07, 0.03)), "v down", color=GRID, hovercolor="#d4d3cf")
    up.on_clicked(lambda _: page(-1))
    down.on_clicked(lambda _: page(1))

    # The specs, each a box.
    fig.text(0.825, 0.965, "Specs", fontsize=13, color=INK, fontweight="bold", va="top")
    fig.text(0.825, 0.935, "click a value, type, Enter (Esc: as it was)", fontsize=8.5, color=INK_2, va="top")

    def shown(field, kind, fields, folder):
        if field == "torque":
            return "%.3f" % state["torque"][folder]
        values = fields.get(field, [])
        if kind == "ratios":
            car = car_now()
            return ", ".join("%.3f" % values[g] for g in car.forward)
        if not values:
            return ""
        if kind == "kmh":
            return "%.1f" % (values[0] * 3.6)
        v = values[0]
        return ("%d" % v) if float(v).is_integer() and abs(v) >= 10 else ("%.5g" % v)

    def apply(n, text):
        folder = current()
        label, field, kind = EDITABLE[n]
        fields = load(folder)
        if text.strip() == shown(field, kind, fields, folder):
            return   # left as it was (a click away from the box): nothing to draw again
        try:
            if field == "torque":
                value = float(text)
                if not (0.1 <= value <= 5):
                    raise ValueError
                state["torque"][folder] = value
            elif kind == "ratios":
                values = [float(v) for v in text.replace(";", ",").split(",") if v.strip()]
                forward = Car(os.path.join(carmodel, folder), fields).forward
                if len(values) != len(forward) or any(v <= 0 for v in values):
                    raise ValueError
                rolling = rolling_of(state["original"][folder])
                ratios = list(fields[8])
                for g, v in zip(forward, values):
                    ratios[g] = v
                fields[8] = ratios
                resync(fields, rolling)
            else:
                value = float(text) / (3.6 if kind == "kmh" else 1.0)
                if value < 0:
                    raise ValueError
                fields[field] = [value]
                if field == 11:
                    resync(fields, rolling_of(state["original"][folder]))
            status.set_text("%s: %s (not saved)" % (label, text.strip()))
        except (ValueError, KeyError, IndexError):
            status.set_text("%s: %r is not a value it takes" % (label, text))
        redraw()

    def resync(fields, rolling):
        """The game's rpm per m/s out of the gearing, as car_perf.py keeps them in step."""
        if not rolling:
            return
        final = (fields.get(11) or [0])[0]
        v2r = list(fields.get(7, []))
        for g, ratio in enumerate(fields.get(8, [])):
            if g < len(v2r) and ratio > 0 and v2r[g] != 0:
                v2r[g] = math.copysign(ratio * final * 60.0 / rolling, v2r[g])
        fields[7] = v2r

    fields_ui = Fields(fig, lambda n, text: apply(n, text))
    for n, (label, field, kind) in enumerate(EDITABLE):
        y = 0.885 - n * 0.054
        fields_ui.add((0.895, y, 0.1, 0.036), label)

    def save(_):
        folder = current()
        if not changed(folder):
            status.set_text("%s: nothing to save" % folder)
            fig.canvas.draw_idle()
            return
        original, fields = state["original"][folder], state["fields"][folder]
        perf_car = car_perf.Car(car_perf.Path(os.path.join(carmodel, folder)))
        carp = perf_car.carp
        rolling = car_perf.rolling_circumference(carp)
        changes = []
        for field, short in SHORT.items():
            if field in (77, 79) or field not in fields:
                continue
            if fields[field] != original.get(field):
                changes.append((short, fields[field]))
                # The automatic gearbox the same, where it was the same before.
                mirror = {8: 77, 11: 79}.get(field)
                if mirror and original.get(mirror) == original.get(field):
                    changes.append((SHORT[mirror], fields[field]))
        if state["torque"][folder] != 1.0:
            changes.append(("torque", [round(v * state["torque"][folder], 6) for v in original[10]]))
        try:
            touched = car_perf.apply_changes(carp, changes, quiet=True)
            car_perf.sync_v2r(carp, touched, rolling)
            written = carp.save()
        except Exception as error:   # the file's own complaint, shown, nothing lost
            status.set_text("not saved: %s" % error)
            fig.canvas.draw_idle()
            return
        state["original"].pop(folder)
        state["fields"].pop(folder)
        state["torque"].pop(folder)
        status.set_text("%s saved (%s)" % (folder, ", ".join(os.path.basename(w) for w in written)))
        show_list()
        redraw()

    def cancel(_):
        folder = current()
        state["original"].pop(folder, None)
        state["fields"].pop(folder, None)
        state["torque"].pop(folder, None)
        status.set_text("%s: changes dropped" % folder)
        show_list()
        redraw()

    save_button = Button(fig.add_axes((0.825, 0.06, 0.08, 0.04)), "Save", color=SERIES[0], hovercolor="#1c5cab")
    save_button.label.set_color("white")
    cancel_button = Button(fig.add_axes((0.912, 0.06, 0.08, 0.04)), "Cancel", color=GRID, hovercolor="#d4d3cf")
    save_button.on_clicked(save)
    cancel_button.on_clicked(cancel)
    # The exhausts of the car shown, marked in a window of their own (car_exhausts.py).
    exhaust_button = Button(fig.add_axes((0.825, 0.11, 0.167, 0.035)), "Mark exhausts...", color=GRID,
                            hovercolor="#d4d3cf")

    def exhausts(_):
        import car_exhausts
        state["exhausts"] = car_exhausts.run(carmodel, current(), show=False)

    exhaust_button.on_clicked(exhausts)

    def redraw():
        folder = current()
        car = car_now()
        for name, ax in chart_axes.items():
            ax.clear()
            {"torque": draw_torque, "power": draw_power, "gearbox": draw_gearbox, "push": draw_push,
             "accel": draw_accel}[name](ax, car)
        readout.clear()
        readout.axis("off")
        readout.set_title("Worked out", loc="left", fontsize=11, color=INK, fontweight="bold", pad=8)
        rows = spec_rows(car)
        times = [(k, car.time_to(k)) for k in (100, 200)]
        rows = rows[:5] + [("0-100 / 0-200", " / ".join("%.1f s" % t if t else "-" for _, t in times))] + rows[5:10]
        for i, (k, v) in enumerate(rows):
            y = 0.97 - i * 0.085
            readout.text(0.0, y, k, transform=readout.transAxes, fontsize=9, color=INK_2, va="top")
            readout.text(0.42, y, v, transform=readout.transAxes, fontsize=9, color=INK, va="top")
        title.set_text(car.name + ("  (changed, not saved)" if changed(folder) else ""))
        subtitle.set_text("%s  -  carp.txt  -  the acceleration table is the game's own, unchanged by "
                          "these edits (car_perf.py derive rebuilds it)" % folder)
        fields = load(folder)
        for n, (label, field, kind) in enumerate(EDITABLE):
            fields_ui.set(n, shown(field, kind, fields, folder))
        fig.canvas.draw_idle()

    def pick(index):
        state["index"] = index
        redraw()

    show_list()
    redraw()
    state["fig"], state["pick"], state["apply"], state["save"], state["cancel"] = fig, pick, apply, save, cancel
    if matplotlib.get_backend().lower() != "agg":
        plt.show()
    return state


def run(carmodel, out, wanted=()):
    """Sheets for the cars under carmodel (all, or the folders in wanted) into
    out, and a comparison when there are several.  Returns how many."""
    if not os.path.isdir(carmodel):
        raise SystemExit("no such folder: %s" % carmodel)
    os.makedirs(out, exist_ok=True)
    wanted = {c.strip().lower() for c in wanted if c.strip()}
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import car_perf
    cars = []
    for entry in car_perf.car_folders(carmodel):
        folder = os.path.join(carmodel, entry)
        if wanted and entry.lower() not in wanted and entry.split("/")[-1].lower() not in wanted:
            continue
        text = carp_text(folder)
        if not text:
            continue
        car = Car(folder, parse_carp(text))
        car.folder = entry
        if car.name == os.path.basename(entry):
            car.name = entry   # no name of its own (a traffic or police car): its folder
        if not car.forward or not car.torque:
            print("skipped %s: no gears or torque curve" % entry)
            continue
        cars.append(car)
        print("%-6s %s" % (entry, car_sheet(car, out)))
    if len(cars) > 1:
        print("compare", compare_sheet(cars, out))
    elif not cars:
        raise SystemExit("no cars with a carp.txt under %s" % carmodel)
    return len(cars)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("carmodel", nargs="?", default=None,
                        help="the cars' folder (default: the repository's gamedata/carmodel, from anywhere)")
    parser.add_argument("-o", "--out", default="car_reports")
    parser.add_argument("--cars", default="", help="folders to draw, comma separated (default: all)")
    parser.add_argument("--edit", nargs="?", const="", metavar="CAR",
                        help="open the editor window (on this car)")
    args = parser.parse_args()
    if args.carmodel is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import car_perf
        args.carmodel = str(car_perf.CARMODEL)
    if args.edit is not None:
        edit(args.carmodel, args.edit or None)
        return
    run(args.carmodel, args.out, args.cars.split(","))


if __name__ == "__main__":
    main()
