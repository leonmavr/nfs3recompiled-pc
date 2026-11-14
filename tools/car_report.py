#!/usr/bin/env python3
"""Performance report sheets for NFS III's cars, from each car's own data.

    python3 tools/car_perf.py report [CAR ...] [--out DIR]      # through car_perf.py
    python3 tools/car_report.py [CARMODEL_DIR] [-o OUT_DIR] [--cars diab,f355,...]

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

matplotlib.use("Agg")
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


def car_sheet(car, out):
    fig, axes = plt.subplots(2, 3, figsize=(16, 9.2), facecolor=SURFACE)
    fig.suptitle(car.name, x=0.012, y=0.985, ha="left", fontsize=16, color=INK, fontweight="bold")
    fig.text(0.012, 0.952, "%s  -  from carp.txt" % car.folder, fontsize=9, color=INK_2)

    rpms = [r for r in range(0, int(car.redline) + 257, 64) if r <= max(car.redline, 256)]
    # Torque.
    ax = axes[0][0]
    style(ax, "Torque", "engine speed (rpm)", "torque (Nm)")
    ax.plot(rpms, [car.torque_at(r) for r in rpms], color=SERIES[0], linewidth=2)
    peak_t, at_t = car.peak_torque()
    ax.plot([at_t], [peak_t], "o", color=SERIES[0], markersize=7, markeredgecolor=SURFACE, markeredgewidth=2)
    ax.annotate("%.0f Nm at %d rpm" % (peak_t, at_t), (at_t, peak_t), xytext=(6, 6),
                textcoords="offset points", fontsize=8, color=INK)
    ax.axvline(car.redline, color=MARK, linewidth=1.2, linestyle="--")
    ax.annotate("redline %d" % car.redline, (car.redline, ax.get_ylim()[0]), xytext=(-4, 6),
                textcoords="offset points", fontsize=8, color=INK_2, ha="right")
    # Power.
    ax = axes[0][1]
    style(ax, "Power", "engine speed (rpm)", "power (kW)")
    kw = [car.torque_at(r) * r * 2 * math.pi / 60 / 1000 for r in rpms]
    ax.plot(rpms, kw, color=SERIES[0], linewidth=2)
    peak_p, at_p = car.peak_power()
    ax.plot([at_p], [peak_p], "o", color=SERIES[0], markersize=7, markeredgecolor=SURFACE, markeredgewidth=2)
    ax.annotate("%.0f kW (%.0f hp) at %d rpm" % (peak_p, peak_p * 1.341, at_p), (at_p, peak_p), xytext=(-6, 8),
                textcoords="offset points", fontsize=8, color=INK, ha="right")
    ax.axvline(car.redline, color=MARK, linewidth=1.2, linestyle="--")

    # The gearbox: rpm against speed in each gear, shifting at the redline.
    ax = axes[0][2]
    style(ax, "Gearbox: engine speed in each gear", "road speed (km/h)", "engine speed (rpm)")
    top_kmh = max(car.top_speed, car.speed_cap) * 3.6
    for n, g in enumerate(car.forward):
        top = car.redline / car.rpm_per_speed[g]
        v = [0, min(top, top_kmh / 3.6)]
        ax.plot([x * 3.6 for x in v], [x * car.rpm_per_speed[g] for x in v], color=SERIES[n % len(SERIES)],
                linewidth=2, label="gear %s" % gear_label(car, g))
        if n + 1 < len(car.forward) and top < top_kmh / 3.6:
            nxt = car.forward[n + 1]
            ax.plot([top * 3.6, top * 3.6], [car.redline, top * car.rpm_per_speed[nxt]], color=INK_2,
                    linewidth=1, linestyle=":")
        ax.annotate("%s" % gear_label(car, g), (min(top, top_kmh / 3.6) * 3.6, min(top, top_kmh / 3.6) *
                    car.rpm_per_speed[g]), xytext=(3, 2), textcoords="offset points", fontsize=8, color=INK)
    ax.axhline(car.redline, color=MARK, linewidth=1.2, linestyle="--")
    ax.set_xlim(0, top_kmh * 1.02)
    ax.legend(fontsize=8, frameon=False, labelcolor=INK, loc="lower right")

    # Push at the wheels.
    ax = axes[1][0]
    style(ax, "Push at the wheels in each gear", "road speed (km/h)", "force (kN)")
    speeds = [s * 0.25 for s in range(0, int(top_kmh / 3.6 * 4) + 2)]
    best = [0.0] * len(speeds)
    for n, g in enumerate(car.forward):
        f = [car.push(g, s) / 1000 for s in speeds]
        best = [max(a, b) for a, b in zip(best, f)]
        pts = [(s * 3.6, x) for s, x in zip(speeds, f) if x > 0]
        if pts:
            ax.plot([p[0] for p in pts], [p[1] for p in pts], color=SERIES[n % len(SERIES)], linewidth=2,
                    label="gear %s" % gear_label(car, g))
    ax.plot([s * 3.6 for s in speeds], best, color=INK, linewidth=1, linestyle="--", label="best gear")
    ax.set_xlim(0, top_kmh * 1.02)
    ax.set_ylim(bottom=0)
    ax.legend(fontsize=8, frameon=False, labelcolor=INK, loc="upper right")

    # The game's acceleration table.
    ax = axes[1][1]
    style(ax, "The game's acceleration by speed", "road speed (km/h)", "acceleration (m/s²)")
    n = len(car.ai_accel)
    ax.plot([i * 3.6 for i in range(n)], car.ai_accel, color=SERIES[0], linewidth=2)
    ax.set_xlim(0, max(top_kmh * 1.02, 10))
    ax.set_ylim(bottom=0)
    times = [(k, car.time_to(k)) for k in (100, 160, 200)]
    note = "   ".join("0-%d: %s" % (k, "%.1f s" % t if t else "-") for k, t in times)
    ax.text(0.98, 0.95, note, transform=ax.transAxes, ha="right", va="top", fontsize=9, color=INK)

    # Specs.
    ax = axes[1][2]
    ax.axis("off")
    ax.set_title("Specs", loc="left", fontsize=11, color=INK, fontweight="bold", pad=8)
    tyre = lambda t: "%d/%dR%d" % tuple(int(x) for x in t[:3]) if len(t) >= 3 else "-"
    drive = "rear" if car.front_drive <= 0.01 else "front" if car.front_drive >= 0.99 else \
        "all (%.0f%% front)" % (car.front_drive * 100)
    rows = [
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
    for i, (k, v) in enumerate(rows):
        y = 0.96 - i * 0.064
        ax.text(0.0, y, k, transform=ax.transAxes, fontsize=9.5, color=INK_2, va="top")
        ax.text(0.38, y, v, transform=ax.transAxes, fontsize=9.5, color=INK, va="top")

    fig.tight_layout(rect=(0, 0, 1, 0.94))
    path = os.path.join(out, car.folder + ".png")
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


def run(carmodel, out, wanted=()):
    """Sheets for the cars under carmodel (all, or the folders in wanted) into
    out, and a comparison when there are several.  Returns how many."""
    if not os.path.isdir(carmodel):
        raise SystemExit("no such folder: %s" % carmodel)
    os.makedirs(out, exist_ok=True)
    wanted = {c.strip().lower() for c in wanted if c.strip()}
    cars = []
    for entry in sorted(os.listdir(carmodel)):
        folder = os.path.join(carmodel, entry)
        if not os.path.isdir(folder) or (wanted and entry.lower() not in wanted):
            continue
        text = carp_text(folder)
        if not text:
            continue
        car = Car(folder, parse_carp(text))
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
    parser.add_argument("carmodel", nargs="?", default=os.path.join("gamedata", "carmodel"))
    parser.add_argument("-o", "--out", default="car_reports")
    parser.add_argument("--cars", default="", help="folders to draw, comma separated (default: all)")
    args = parser.parse_args()
    run(args.carmodel, args.out, args.cars.split(","))


if __name__ == "__main__":
    main()
