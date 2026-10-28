"""Read, show and tune the performance of a car: the `carp.txt` in its `car.viv`.

Every car the game loads carries a `carp.txt` -- 80 labelled fields, written by
EA's car editor, and the only place the physics comes from: the game looks each
one up by its label, so editing a value really does change how the car drives
(the labels are in `nfs3.exe` at 0x53e0xx-0x53f0xx and are what its parser
searches for).  Nothing else in a car's folder holds performance: the
`fedata.eng` strings ("255 km/h", "6.59 sec") are the showroom blurb, and are
shown here only as the figures to check an estimate against.

It lives *inside the archive* -- `<car>/car.viv` holds `carp.txt` beside
`car.fce` and `fedata.fsh`, and the game parses that copy; the loose
`<car>/carp.txt` beside it is what tools/extract_viv.py unpacked, and nothing
reads it (delete it and the game still drives the car).  So this reads the
archive and writes both, the archive being the one that counts.

    python3 tools/car_perf.py list
    python3 tools/car_perf.py show hgts
    python3 tools/car_perf.py show hgts --out hgts.png
    python3 tools/car_perf.py compare hgts f355 peln
    python3 tools/car_perf.py set hgts mass=1500 "final gear=4.10"
    python3 tools/car_perf.py clone hgts mycar --id 60 --name "ZZZ 001" mass=1500

`show` draws four panels: the headline numbers as bars against the rest of the
field, the engine's torque and power against rpm, the gear chart (engine rpm
against road speed, one line per gear), and the acceleration the game gives the
car against speed with the 0-100 it works out to.

What is read straight out of the file, and what is worked out:

  given      torque curve (41 points, 256 rpm apart), gear ratios, final gear,
             efficiencies, velocity-to-rpm ratios, redline, mass, maximum
             velocity, the eight AI acceleration tables (a single curve of
             acceleration against speed in 1 m/s steps), brake and grip figures
  derived    power from the torque curve, the gear chart's speed axis, the
             0-100 / 0-200 / 400 m times (integrating the acceleration curve),
             power per tonne

`carp.txt` is the only thing ever written -- the value lines of the fields that
changed, with the file's CRLF endings, trailing commas and trailing NUL left as
they were, in `car.viv` and in the loose copy.  A file that grows has its
archive rebuilt around it; one that fits is written where it lies.
"""
import argparse
import json
import math
import re
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

CARMODEL = Path("gamedata/carmodel")

# The labels the game parses, in file order, with what each one is for.  The
# numbers are the "(n)" in the file; `short` is what --set accepts, and `unit`
# is only ever used in the readouts.
FIELDS = {
    0: ("Serial Number", "serial", ""),
    1: ("Car Classification", "class", ""),
    2: ("mass [kg]", "mass", "kg"),
    3: ("number of gears (reverse + neutral + forward gears)", "gears", ""),
    4: ("gear shift delay (ticks)", "shift delay", "ticks"),
    5: ("shift blip in rpm (size 8)", "shift blip", "rpm"),
    6: ("brake blip in rpm (size 8)", "brake blip", "rpm"),
    7: ("velocity to rpm ratio (size 8)", "v2r", "rpm/(m/s)"),
    8: ("gear ratios (size 8)", "ratios", ""),
    9: ("gear efficiency (size 8)", "efficiency", ""),
    10: ("torque curve (size 41) in 256rpm increments", "torque", "game units"),
    11: ("final gear", "final", ""),
    12: ("engine minimum rpm", "min rpm", "rpm"),
    13: ("engine redline in rpm", "redline", "rpm"),
    14: ("maximum velocity of car [m/s]", "vmax", "m/s"),
    15: ("top speed cap [m/s]", "speed cap", "m/s"),
    16: ("front drive ratio", "front drive", ""),
    17: ("Uses Antilock Brake System", "abs", ""),
    18: ("maximum braking deceleration", "brake decel", "m/s2"),
    19: ("front bias brake ratio", "brake bias", ""),
    20: ("gas increasing curve", "gas inc", ""),
    21: ("gas decreasing curve", "gas dec", ""),
    22: ("brake increasing curve", "brake inc", ""),
    23: ("brake decreasing curve", "brake dec", ""),
    24: ("wheel base", "wheelbase", "m"),
    25: ("front grip bias", "front grip", ""),
    26: ("power steering (boolean)", "power steering", ""),
    27: ("minimum steering acceleration", "steer accel", ""),
    28: ("turn in ramp", "turn in", "ticks"),
    29: ("turn out ramp", "turn out", "ticks"),
    30: ("lateral acceleration grip multiplier", "grip", "g"),
    31: ("aerodynamic downforce multiplier", "downforce", ""),
    32: ("gas off factor", "gas off", ""),
    33: ("g transfer factor", "g transfer", ""),
    34: ("turning circle radius", "turning circle", "m"),
    35: ("tire specs front", "tires front", "w/a/rim"),
    36: ("tire specs rear", "tires rear", "w/a/rim"),
    37: ("tire wear", "tire wear", ""),
    38: ("Slide Multiplier", "slide mult", ""),
    39: ("Spin Velocity Cap", "spin cap", ""),
    40: ("Slide Velocity Cap", "slide cap", ""),
    41: ("Slide Assistance Factor", "slide assist", ""),
    42: ("Push Factor", "push factor", ""),
    43: ("Low Turn Factor (the lower the figure, the better the turn)", "low turn", ""),
    44: ("High Turn Factor (the lower the figure, the better the turn)", "high turn", ""),
    45: ("pitch roll factor", "pitch roll", ""),
    46: ("road bumpiness factor", "bumpiness", ""),
    47: ("spoiler function type", "spoiler type", ""),
    48: ("spoiler activation speed [m/s]", "spoiler speed", "m/s"),
    49: ("gradual turn cutoff", "gradual turn", ""),
    50: ("medium turn cutoff", "medium turn", ""),
    51: ("sharp turn cutoff", "sharp turn", ""),
    52: ("medium turn speed modifier", "medium turn speed", ""),
    53: ("sharp turn speed modifier", "sharp turn speed", ""),
    54: ("extreme turn speed modifier", "extreme turn speed", ""),
    55: ("subdivide level", "subdivide", ""),
    56: ("camera arm", "camera arm", ""),
    57: ("Body Damage", "body damage", ""),
    58: ("Engine Damage", "engine damage", ""),
    59: ("Suspension Damage", "suspension damage", ""),
    60: ("Engine Tuning", "engine tuning", ""),
    61: ("Brake Balance", "brake balance", ""),
    62: ("Steering Speed", "steering speed", ""),
    63: ("Gear Rat Factor", "gear rat factor", ""),
    64: ("Suspension Stiffness", "suspension stiffness", ""),
    65: ("Aero Factor", "aero factor", ""),
    66: ("Tire Factor", "tire factor", ""),
    67: ("AI ACC0 acceleration table section", "acc 0", "m/s2"),
    68: ("AI ACC1 acceleration table section", "acc 1", "m/s2"),
    69: ("AI ACC2 acceleration table section", "acc 2", "m/s2"),
    70: ("AI ACC3 acceleration table section", "acc 3", "m/s2"),
    71: ("AI ACC4 acceleration table section", "acc 4", "m/s2"),
    72: ("AI ACC5 acceleration table section", "acc 5", "m/s2"),
    73: ("AI ACC6 acceleration table section", "acc 6", "m/s2"),
    74: ("AI ACC7 acceleration table section", "acc 7", "m/s2"),
    75: ("number of gears (automatic, r, n, forward)", "gears auto", ""),
    76: ("velocity to rpm ratio automatic(size 8)", "v2r auto", ""),
    77: ("gear ratios automatic (size 8)", "ratios auto", ""),
    78: ("gear efficiency automatic (size 8)", "efficiency auto", ""),
    79: ("final gear automatic", "final auto", ""),
}

SHORT_TO_INDEX = {short: index for index, (_, short, _) in FIELDS.items()}
ACC_FIRST, ACC_LAST = 67, 74
ACC_SAMPLES = 14 * (ACC_LAST - ACC_FIRST + 1)


class CarFileError(Exception):
    pass


@dataclass
class Field:
    """One labelled value: the line it lives on, and how it was written."""
    index: int
    label: str
    values: list
    integer: bool
    decimals: int
    trailing_comma: bool
    raw: str
    line: int = 0


class Carp:
    """A car's `carp.txt`, parsed so that it can be written back untouched.

    It lives inside the car's `car.viv` -- that is the copy the game opens the
    archive for and parses, and nothing reads the loose `carp.txt` beside it
    (delete that one and the game still drives the car).  The loose file is
    unpacked from the archive (tools/extract_viv.py) for editing and reading,
    so both are written, and the archive is the one that counts.
    """

    def __init__(self, folder):
        self.folder = Path(folder)
        self.path = self.folder / "carp.txt"
        data, self.from_archive = read_carp_text(self.folder)
        self.original = data
        self.trailing_nul = data.endswith(b"\0")
        text = data.decode("latin-1")
        if self.trailing_nul:
            text = text[:-1]
        self.lines = text.split("\r\n")
        self.fields = {}
        self.order = []
        line = 0
        while line < len(self.lines):
            match = re.match(r"^(.*?)\((\d+)\)$", self.lines[line])
            if not match or line + 1 >= len(self.lines):
                line += 1
                continue
            index = int(match.group(2))
            raw = self.lines[line + 1]
            self.fields[index] = self._parse_field(index, match.group(1), raw, line + 1)
            self.order.append(index)
            line += 2

    def _parse_field(self, index, label, raw, value_line):
        text = raw.strip()
        trailing_comma = text.endswith(",")
        parts = [part for part in text.split(",") if part.strip()]
        integer = all("." not in part for part in parts) and bool(parts)
        decimals = 0
        for part in parts:
            if "." in part:
                decimals = max(decimals, len(part.split(".")[1]))
        try:
            values = [int(part) if integer else float(part) for part in parts]
        except ValueError:
            raise CarFileError("%s: field %d (%s) is not a number: %r" % (self.path, index, label, raw))
        return Field(index, label, values, integer, decimals, trailing_comma, raw, value_line)

    # ---- reading ----

    def value(self, index):
        """The field's values: a list, or a single number for a one-value field."""
        values = self.fields[index].values
        return values[0] if len(values) == 1 else list(values)

    def number(self, index, default=0.0):
        values = self.fields[index].values
        return values[0] if values else default

    def label(self, index):
        return self.fields[index].label if index in self.fields else FIELDS.get(index, ("?", "?", ""))[0]

    # ---- writing ----

    def set(self, index, values):
        """Replace a field's values, writing them the way the file already does."""
        current = self.fields[index]
        values = list(values) if isinstance(values, (list, tuple)) else [values]
        if len(values) != len(current.values):
            raise CarFileError(
                "field %d (%s) holds %d value%s, not %d -- pass a comma separated list"
                % (index, current.label, len(current.values),
                   "" if len(current.values) == 1 else "s", len(values)))
        if current.integer:
            text = ",".join("%d" % int(round(v)) for v in values)
        else:
            text = ",".join("%.*f" % (current.decimals or 6, v) for v in values)
        if current.trailing_comma:
            text += ","
        self.lines[current.line] = text
        self.fields[index] = Field(current.index, current.label, list(values), current.integer,
                                    current.decimals, current.trailing_comma, text, current.line)

    def save(self):
        text = "\r\n".join(self.lines)
        if self.trailing_nul:
            text += "\0"
        payload = text.encode("latin-1")
        written = []
        if self.from_archive:
            replace_archive_entry(self.folder / "car.viv", "carp.txt", payload)
            written.append("car.viv")
        self.folder.mkdir(parents=True, exist_ok=True)
        self.path.write_bytes(payload)
        written.append("carp.txt")
        return written

    def index_of(self, name):
        """A field's number: its own number, its short name, or its label.

        A name that matches several fields has to be pinned down -- "final
        gear" is field 11's label and field 79's begins the same way, so an
        exact label wins, then an exact short name, then the one field whose
        short name contains the text.
        """
        text = str(name).strip()
        if text.isdigit() and int(text) in FIELDS:
            return int(text)
        low = text.lower()
        if low in SHORT_TO_INDEX:
            return SHORT_TO_INDEX[low]
        exact = [index for index, (label, _, _) in FIELDS.items() if label.lower() == low]
        if len(exact) == 1:
            return exact[0]
        matches = [index for index, (label, short, _) in FIELDS.items()
                   if low in label.lower() or low in short]
        short_matches = [index for index in matches if low in FIELDS[index][1]]
        if len(short_matches) == 1:
            return short_matches[0]
        if len(matches) == 1:
            return matches[0]
        if not matches:
            raise CarFileError("no field matches %r (try: list --fields)" % name)
        raise CarFileError("%r matches several fields: %s (use the number or the short name)"
                           % (name, ", ".join("%d %s" % (i, FIELDS[i][1]) for i in matches)))


class Car:
    """A folder under `gamedata/carmodel`: its `carp.txt` and its showroom text."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.key = self.folder.name
        self.carp = Carp(self.folder)

    # ---- what the file says ----

    @property
    def mass(self):
        return self.carp.number(2)

    @property
    def gears(self):
        """Forward gears, i.e. the slots the ratios hold after reverse and neutral."""
        return max(0, int(self.carp.number(3)) - 2)

    def ratios(self, automatic=False):
        return self.carp.value(77 if automatic else 8)

    def v2r(self, automatic=False):
        return self.carp.value(76 if automatic else 7)

    def efficiency(self, automatic=False):
        return self.carp.value(78 if automatic else 9)

    @property
    def final(self):
        return self.carp.number(11)

    @property
    def redline(self):
        return self.carp.number(13)

    @property
    def idle(self):
        return self.carp.number(12)

    @property
    def vmax(self):
        return self.carp.number(14)

    @property
    def torque(self):
        return self.carp.value(10)

    @property
    def grip(self):
        return self.carp.number(30)

    @property
    def brake_decel(self):
        return self.carp.number(18)

    @property
    def tires(self):
        return self.carp.value(35)

    # ---- what it means ----

    def torque_at(self, rpm):
        """Engine torque at `rpm`: the curve is 41 points 256 rpm apart."""
        curve = self.torque
        if not curve:
            return 0.0
        position = rpm / 256.0
        low = int(math.floor(position))
        if low < 0:
            return curve[0]
        if low >= len(curve) - 1:
            return curve[-1]
        frac = position - low
        return curve[low] * (1.0 - frac) + curve[low + 1] * frac

    def rpm_points(self):
        return [i * 256.0 for i in range(len(self.torque))]

    def power_kw(self, rpm):
        """Derived: torque x rpm, in kW (the curve's units are the game's own)."""
        return self.torque_at(rpm) * rpm * 2.0 * math.pi / 60.0 / 1000.0

    def peak(self):
        """(rpm, torque) of the curve's peak, and (rpm, kW) of its power peak."""
        curve = self.torque
        if not curve:
            return (0.0, 0.0), (0.0, 0.0)
        rpm = self.rpm_points()
        torque_rpm = rpm[max(range(len(curve)), key=lambda i: curve[i])]
        rpm = [r for r in rpm if r <= self.redline] or rpm
        power_rpm = max(rpm, key=self.power_kw)
        return (torque_rpm, max(curve)), (power_rpm, self.power_kw(power_rpm))

    def gear_speeds(self):
        """[(gear, ratio, rpm-per-m/s, max speed at redline)] for the forward gears."""
        ratios, v2r = self.ratios(), self.v2r()
        out = []
        for gear in range(1, self.gears + 1):
            slot = gear + 1  # 0 and 1 are reverse and neutral
            if slot >= len(ratios) or slot >= len(v2r) or v2r[slot] <= 0:
                continue
            out.append((gear, ratios[slot], v2r[slot], self.redline / v2r[slot]))
        return out

    def speed_at(self, gear, rpm):
        speeds = {g: v2r for g, _, v2r, _ in self.gear_speeds()}
        if gear not in speeds:
            return 0.0
        return rpm / speeds[gear]

    def rpm_at(self, gear, speed):
        speeds = {g: v2r for g, _, v2r, _ in self.gear_speeds()}
        if gear not in speeds:
            return 0.0
        return speed * speeds[gear]

    def accel_table(self):
        """The game's acceleration against speed: eight sections, one curve.

        Written as eight AI ACC sections of 14 samples; read in order they are a
        single curve sampled every 1 m/s (the last non-zero sample lands on the
        car's maximum velocity).
        """
        table = []
        for index in range(ACC_FIRST, ACC_LAST + 1):
            table.extend(self.carp.value(index))
        return table

    def accel_step(self):
        """Metres per second between acceleration samples.

        One by construction; a car whose table runs out before its stated top
        speed is scaled so the curve still spans 0..vmax, which is how the game
        reads it.
        """
        table = self.accel_table()
        last = max((i for i, a in enumerate(table) if a > 0.01), default=0)
        if last <= 0:
            return 1.0
        step = self.vmax / (last + 1.0)
        return step if 0.5 <= step <= 2.0 else 1.0

    def accel_curve(self):
        """[(speed in m/s, acceleration)].  After the table runs out, 0."""
        step = self.accel_step()
        table = self.accel_table()
        return [(i * step, a) for i, a in enumerate(table)]

    def time_to(self, speed, step_dt=0.01):
        """Seconds to reach `speed` m/s from rest, integrating the curve."""
        curve = self.accel_curve()
        if not curve:
            return None
        v, t = 0.0, 0.0
        while v < speed:
            a = self.accel_at(v, curve)
            if a <= 0.01:
                return None
            v += a * step_dt
            t += step_dt
            if t > 120.0:
                return None
        # last partial step
        return t - (v - speed) / max(a, 0.01)

    def accel_at(self, speed, curve=None):
        curve = curve or self.accel_curve()
        if not curve:
            return 0.0
        step = curve[1][0] - curve[0][0] if len(curve) > 1 else 1.0
        position = speed / step if step else 0.0
        low = int(position)
        if low >= len(curve) - 1:
            return curve[-1][1]
        frac = position - low
        return curve[low][1] * (1.0 - frac) + curve[low + 1][1] * frac

    def rolling_radius(self):
        """Wheel radius, from the gearing the game stores (metres)."""
        for ratio, v2r in zip(self.ratios(), self.v2r()):
            if ratio > 0 and v2r > 0:
                return ratio * self.final * 60.0 / v2r / (2.0 * math.pi)
        tires = self.tires
        if len(tires) == 3:
            width, aspect, rim = tires
            return (rim * 25.4 / 2.0 + width * aspect / 100.0) / 1000.0
        return 0.3

    def physics_curve(self, step=0.5):
        """[(speed, acceleration)] from the engine curve, gearing and mass.

        The car's own torque curve through its own gearbox, taking the best
        gear at each speed and bounded by the redline.  A drag term is fitted
        so that the curve reaches zero exactly at the car's stated maximum
        velocity, which is otherwise uncapped here.

        This is an estimate -- the torque curve's units are the game's own, and
        nothing models clutch, tyres or wheelspin -- but unlike the AI table it
        moves when mass, gearing or the curve is edited.
        """
        radius = self.rolling_radius()
        gears = self.gear_speeds()
        if not radius or not gears or not self.mass:
            return []
        efficiency = self.efficiency()
        vmax = self.vmax
        if vmax <= 0:
            return []

        def tractive(speed):
            best = 0.0
            for gear, ratio, v2r, _ in gears:
                if v2r * speed > self.redline:
                    continue
                # below idle the clutch slips and the engine sits at idle
                rpm = max(v2r * speed, self.idle)
                slot = gear + 1
                eff = efficiency[slot] if slot < len(efficiency) else 1.0
                force = self.torque_at(rpm) * ratio * self.final * (eff or 1.0) / radius
                best = max(best, force / self.mass)
            return best

        drag = tractive(vmax) / (vmax * vmax) if vmax else 0.0
        out = []
        speed = 0.0
        while speed <= vmax * 1.02:
            a = tractive(speed)
            if a <= 0.0 and speed > 0.0:
                break
            out.append((speed, max(0.0, a - drag * speed * speed)))
            speed += step
        return out

    @staticmethod
    def time_from_curve(curve, speed, step_dt=0.01):
        """Seconds to `speed` m/s, integrating an [(speed, accel)] curve."""
        if not curve:
            return None
        v, t = 0.0, 0.0
        while v < speed:
            a = Car._interpolate(curve, v)
            if a <= 0.01:
                return None
            v += a * step_dt
            t += step_dt
            if t > 120.0:
                return None
        return t - (v - speed) / max(a, 0.01)

    @staticmethod
    def _interpolate(curve, speed):
        span = curve[1][0] - curve[0][0] if len(curve) > 1 else 1.0
        position = speed / span if span else 0.0
        low = int(position)
        if low >= len(curve) - 1:
            return curve[-1][1]
        frac = position - low
        return curve[low][1] * (1.0 - frac) + curve[low + 1][1] * frac

    def quarter_mile(self):
        """Seconds to 402.34 m from rest, integrating the curve."""
        curve = self.accel_curve()
        if not curve:
            return None
        v, t, d = 0.0, 0.0, 0.0
        while d < 402.336:
            a = self.accel_at(v, curve)
            if a <= 0.01:
                return None
            v += a * 0.01
            d += v * 0.01
            t += 0.01
            if t > 120.0:
                return None
        return t

    def stated(self):
        """The showroom figures from fedata.eng, where the car has them."""
        path = self.folder / "fedata.eng"
        if not path.exists():
            return {}
        text = path.read_bytes().decode("latin-1")
        parts = [part.strip() for part in text.split("\0")]
        out = {}
        for part in parts:
            low = part.lower()
            if low.endswith("km/h"):
                out.setdefault("top speed", _first_number(part))
            elif part.endswith("kW") or " kW @" in part:
                out.setdefault("power", _first_number(part))
            elif part.endswith("bhp") or " bhp @" in part:
                out.setdefault("power bhp", _first_number(part))
            elif part.endswith("Nm") or " Nm @" in part:
                out.setdefault("torque", _first_number(part))
            elif part.endswith("lb-ft") or " lb-ft @" in part:
                out.setdefault("torque lb-ft", _first_number(part))
        seconds = [part for part in parts if re.match(r"^\d+(\.\d+)? sec$", part)]
        if seconds:
            out["0-62"] = float(seconds[0].split()[0])
        if len(seconds) > 1:
            out["quarter mile"] = float(seconds[1].split()[0])
        return out


def _first_number(text):
    match = re.search(r"-?\d+(?:\.\d+)?", text)
    return float(match.group(0)) if match else None


def find_car(name, root=CARMODEL):
    """A car by folder name, case and abbreviation aside."""
    root = Path(root)
    if not root.is_dir():
        raise CarFileError("no %s here; run me from the repository root" % root)
    wanted = name.lower()
    for folder in sorted(root.iterdir()):
        if folder.is_dir() and folder.name.lower() == wanted and (folder / "carp.txt").exists():
            return Car(folder)
    # a display name, as fedata.eng spells it
    for folder in sorted(root.iterdir()):
        if not (folder / "carp.txt").exists():
            continue
        car = Car(folder)
        if wanted in car.display_name().lower():
            return car
    raise CarFileError("no car called %r under %s" % (name, root))


def all_cars(root=CARMODEL):
    root = Path(root)
    if not root.is_dir():
        return []
    return [Car(folder) for folder in sorted(root.iterdir())
            if folder.is_dir() and (folder / "carp.txt").exists()]


def _car_text(folder):
    """(maker, short name, full name), out of the car's own `fedata.eng`.

    Its string table starts at 0xcf with the maker, the short name and the full
    name -- "HSV", "VT GTS", "HSV VT GTS" -- and carries on with the price, the
    class and the showroom figures.
    """
    path = Path(folder) / "fedata.eng"
    if not path.exists():
        return "", "", ""
    data = path.read_bytes()
    strings = []
    start = None
    for offset in range(0xcf, min(len(data), 0x180)):
        byte = data[offset]
        if start is None:
            if 31 < byte < 127:
                start = offset
        elif byte == 0:
            text = data[start:offset].decode("latin-1").strip()
            if text:
                strings.append(text)
            start = None
    maker = strings[0] if len(strings) > 0 else ""
    short = strings[1] if len(strings) > 1 else ""
    full = strings[2] if len(strings) > 2 else short
    return maker, short, full


Car.display_name = lambda self: _car_text(self.folder)[2]
Car.maker = lambda self: _car_text(self.folder)[0]


# ---------------------------------------------------------------- the numbers

CLASSES = {0: "A", 1: "B", 2: "C", 3: "bonus"}
BHP_TO_KW = 0.7457


def stated_of(car):
    """The showroom figures, with the imperial ones converted."""
    stated = dict(car.stated())
    if "power bhp" in stated:
        stated["power"] = stated.pop("power bhp") * BHP_TO_KW
    if "torque lb-ft" in stated:
        stated["torque"] = stated.pop("torque lb-ft") * 1.3558
    return stated


def metrics(car):
    """The headline figures, and whether each one was read or worked out."""
    (torque_rpm, torque_peak), (power_rpm, power_peak) = car.peak()
    stated = stated_of(car)
    t100 = car.time_to(100.0 / 3.6)
    power = stated.get("power")
    power_from_curve = power is None
    if power_from_curve:
        power = power_peak
    return {
        "power": power,
        "power_stated": not power_from_curve,
        "power_curve": power_peak,
        "power_rpm": power_rpm,
        "power_t": power / (car.mass / 1000.0) if car.mass else 0.0,
        "torque": torque_peak,
        "torque_stated": stated.get("torque"),
        "torque_rpm": torque_rpm,
        "top": car.vmax * 3.6,
        "top_stated": stated.get("top speed"),
        "t100": t100,
        "t100_stated": stated.get("0-62"),
        "t100_physics": Car.time_from_curve(car.physics_curve(), 27.78),
        "quarter": car.quarter_mile(),
        "mass": car.mass,
        "grip": car.grip,
        "brake": car.brake_decel,
        "circle": car.carp.number(34),
        "gears": car.gears,
        "redline": car.redline,
        "class": CLASSES.get(int(car.carp.number(1)), "?"),
    }


# key, label, unit, decimals, lower is better
BAR_METRICS = [
    ("power", "power", "kW", 0, False),
    ("power_t", "power per tonne", "kW/t", 0, False),
    ("top", "top speed", "km/h", 0, False),
    ("t100", "0-100 km/h", "s", 2, True),
    ("grip", "lateral grip", "g", 2, False),
    ("brake", "braking", "m/s2", 1, False),
    ("circle", "turning circle", "m", 1, True),
]


def fleet_range(cars, key):
    values = [m[key] for m in (metrics(car) for car in cars) if m.get(key) is not None]
    return (min(values), max(values)) if values else (0.0, 0.0)


def relative(value, low, high, lower_is_better):
    """How good `value` is against the field: 1.0 is the best car."""
    if value is None or high <= low:
        return 0.0
    if lower_is_better:
        return low / value if value else 0.0
    return value / high if high else 0.0


def _value_text(values, key, unit, decimals):
    value = values.get(key)
    if value is None:
        return "--"
    text = "%.*f %s" % (decimals, value, unit)
    if key == "power" and not values.get("power_stated"):
        text = "~" + text
    return text


# --------------------------------------------------------------- the drawing

def _pyplot(show):
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt


def _draw_bars(ax, car, cars):
    values = metrics(car)
    labels, rels, texts = [], [], []
    for key, label, unit, decimals, lower in BAR_METRICS:
        low, high = fleet_range(cars, key)
        labels.append(label)
        rels.append(relative(values.get(key), low, high, lower))
        texts.append(_value_text(values, key, unit, decimals))
    positions = list(range(len(labels)))
    ax.barh(positions, rels, color="#3d7ea6", height=0.62)
    ax.set_yticks(positions)
    ax.set_yticklabels(labels, fontsize=9)
    ax.invert_yaxis()
    ax.set_xlim(0, 1.30)
    ax.set_xticks([0.5, 1.0])
    ax.set_xticklabels(["half of the best", "best of the field"], fontsize=8)
    for position, rel, text in zip(positions, rels, texts):
        ax.text(max(rel, 0.0) + 0.02, position, text, va="center", fontsize=9)
    ax.set_title("against the other %d cars" % (len(cars) - 1), fontsize=10)
    ax.grid(axis="x", color="#dddddd", linewidth=0.6)
    ax.set_axisbelow(True)


def _draw_engine(ax, car):
    limit = max(car.redline * 1.12, 1200)
    rpm = [r for r in car.rpm_points() if r <= limit]
    torque = [car.torque_at(r) for r in rpm]
    power = [car.power_kw(r) for r in rpm]
    ax.plot(rpm, torque, color="#b5442a", linewidth=2)
    ax.set_xlabel("engine rpm", fontsize=9)
    ax.set_ylabel("torque", color="#b5442a", fontsize=9)
    ax.tick_params(labelsize=8)
    ax.set_xlim(0, limit)
    other = ax.twinx()
    other.plot(rpm, power, color="#2a6fb5", linewidth=2)
    other.set_ylabel("power, from that curve (kW)", color="#2a6fb5", fontsize=9)
    other.tick_params(labelsize=8)
    other.set_ylim(0, max(power) * 1.15 if power else 1)
    for axis in (ax, other):
        axis.axvline(car.redline, color="#999999", linestyle=":", linewidth=1)
    top = ax.get_ylim()[1]
    ax.annotate("redline %.0f" % car.redline, (car.redline, top), textcoords="offset points",
                xytext=(-4, -12), fontsize=8, color="#666666", ha="right")
    stated = stated_of(car)
    note = []
    if stated.get("torque"):
        note.append("showroom: %.0f Nm, %.0f kW" % (stated["torque"], stated.get("power") or 0))
    if note:
        ax.text(0.02, 0.03, "   ".join(note), transform=ax.transAxes, fontsize=8, color="#666666")
    ax.set_title("engine curve (Nm, and kW worked out from it)", fontsize=10)
    ax.grid(color="#eeeeee", linewidth=0.6)
    ax.set_axisbelow(True)


def _draw_gears(ax, car):
    gears = car.gear_speeds()
    if not gears:
        ax.text(0.5, 0.5, "no usable gear ratios", ha="center", va="center")
        ax.set_axis_on(False)
        return
    top = max(speed for _, _, _, speed in gears) * 3.6
    for gear, ratio, _, top_speed in gears:
        points = [i * car.redline / 40.0 for i in range(41)]
        ax.plot([car.speed_at(gear, rpm) * 3.6 for rpm in points], points,
                linewidth=2, label="%d (%.2f)" % (gear, ratio))
        ax.annotate("%d" % gear, (top_speed * 3.6, car.redline), textcoords="offset points",
                    xytext=(2, -9), fontsize=8, color="#444444")
    for gear, _, _, redline_speed in gears[:-1]:
        next_rpm = car.rpm_at(gear + 1, redline_speed)
        if next_rpm < car.redline:
            ax.plot([redline_speed * 3.6, redline_speed * 3.6], [car.redline, next_rpm],
                    color="#bbbbbb", linestyle="--", linewidth=1)
    ax.axhline(car.redline, color="#999999", linestyle=":", linewidth=1)
    ax.axvline(car.vmax * 3.6, color="#2a6fb5", linestyle=":", linewidth=1)
    ax.annotate("top speed %.0f" % (car.vmax * 3.6), (car.vmax * 3.6, car.idle),
                textcoords="offset points", xytext=(-6, 0), fontsize=8, color="#2a6fb5",
                rotation=90, va="bottom", ha="right")
    ax.set_xlabel("road speed (km/h)", fontsize=9)
    ax.set_ylabel("engine rpm", fontsize=9)
    ax.tick_params(labelsize=8)
    ax.set_xlim(0, max(top, car.vmax * 3.6) * 1.08)
    ax.set_ylim(0, car.redline * 1.10)
    ax.set_title("gears: rpm against speed (ratio in the key)", fontsize=10)
    ax.legend(fontsize=8, ncol=3, loc="lower right", frameon=False, title="gear", title_fontsize=8)
    ax.grid(color="#f2f2f2", linewidth=0.5)
    ax.set_axisbelow(True)


def _draw_accel(ax, car):
    curve = car.accel_curve()
    if not curve:
        ax.text(0.5, 0.5, "no acceleration table", ha="center", va="center")
        ax.set_axis_on(False)
        return
    limit = car.vmax * 3.6 * 1.06
    kept = [(v, a) for v, a in curve if v * 3.6 <= limit]
    speeds = [v * 3.6 for v, _ in kept]
    accels = [a for _, a in kept]
    ax.plot(speeds, accels, color="#2f7d4f", linewidth=2, label="acceleration")
    ax.fill_between(speeds, accels, color="#2f7d4f", alpha=0.12)
    ax.set_xlabel("road speed (km/h)", fontsize=9)
    ax.set_ylabel("acceleration (m/s2)", color="#2f7d4f", fontsize=9)
    ax.tick_params(labelsize=8)
    ax.set_xlim(0, max(speeds) * 1.02)
    other = ax.twinx()
    xs, ys = [], []
    marks = [40, 60, 80, 100, 120, 160, 200, 240]
    for kmh in marks:
        seconds = car.time_to(kmh / 3.6, step_dt=0.02)
        if seconds is not None:
            xs.append(kmh)
            ys.append(seconds)
    if xs:
        other.plot(xs, ys, color="#a05a00", linewidth=1.6, linestyle="--", label="seconds from rest")
        other.set_ylabel("seconds from rest", color="#a05a00", fontsize=9)
        other.tick_params(labelsize=8)
        other.set_ylim(0, max(ys) * 1.25)
        for kmh, seconds in zip(xs, ys):
            if kmh in (100, 200):
                other.annotate("%.1f s" % seconds, (kmh, seconds), textcoords="offset points",
                               xytext=(5, -2), fontsize=9, color="#a05a00", fontweight="bold")
    physics = car.physics_curve()
    if physics:
        ax.plot([v * 3.6 for v, _ in physics], [a for _, a in physics], color="#7a3f9d",
                linewidth=1.6, linestyle="--")
        ax.text(0.02, 0.03, "dashed: engine-curve estimate (optimistic: no clutch or tyre losses)", color="#7a3f9d", fontsize=7.5,
                transform=ax.transAxes, ha="left", va="bottom")
    said = stated_of(car).get("0-62")
    lines = []
    estimated = car.time_to(27.78)
    if estimated:
        lines.append("0-100: %.2f s from the table" % estimated)
    from_curve = Car.time_from_curve(physics, 27.78) if physics else None
    if from_curve:
        lines.append("%.2f s from the curve" % from_curve)
    if said:
        lines.append("game says %.1f s" % said)
    if lines:
        ax.text(0.98, 0.97, "   ".join(lines), transform=ax.transAxes, fontsize=9, va="top",
                ha="right", bbox=dict(facecolor="white", edgecolor="none", alpha=0.75, pad=1.5))
    ax.set_title("acceleration the game gives it", fontsize=10)
    ax.grid(color="#eeeeee", linewidth=0.6)
    ax.set_axisbelow(True)


def draw_car(car, cars, out=None, show=True):
    plt = _pyplot(show)
    from matplotlib.gridspec import GridSpec
    figure = plt.figure(figsize=(13.0, 8.8))
    figure.suptitle("%s   --   %s" % (car.display_name() or car.key, car.key), fontsize=16,
                    x=0.012, ha="left", y=0.985)
    values = metrics(car)
    stated = stated_of(car)
    first = ("%s   Class %s   %.0f kg   %d gears   redline %.0f rpm   top speed %.0f km/h"
             % (car.maker(), values["class"], car.mass, car.gears, car.redline, values["top"]))
    showroom = []
    if stated.get("torque"):
        showroom.append("%.0f Nm" % stated["torque"])
    if stated.get("power"):
        showroom.append("%.0f kW" % stated["power"])
    if stated.get("0-62"):
        showroom.append("0-100 in %.2f s" % stated["0-62"])
    if stated.get("quarter mile"):
        showroom.append("400 m in %.2f s" % stated["quarter mile"])
    stored = ("the stored curve peaks at %.0f @ %.0f rpm, and its acceleration table gives "
              "0-100 in %.2f s" % (values["torque"], values["torque_rpm"],
                                   values["t100"] or 0))
    figure.text(0.012, 0.945, first, fontsize=10.5, color="#222222")
    if showroom:
        figure.text(0.012, 0.916, "showroom: " + ", ".join(showroom), fontsize=10, color="#444444")
    figure.text(0.012, 0.890, stored, fontsize=10, color="#777777")
    grid = GridSpec(2, 2, figure=figure, left=0.145, right=0.93, top=0.855, bottom=0.07,
                    hspace=0.40, wspace=0.34)
    _draw_bars(figure.add_subplot(grid[0, 0]), car, cars)
    _draw_engine(figure.add_subplot(grid[0, 1]), car)
    _draw_gears(figure.add_subplot(grid[1, 0]), car)
    _draw_accel(figure.add_subplot(grid[1, 1]), car)
    return _finish(figure, out, show)


def draw_compare(cars, out=None, show=True):
    plt = _pyplot(show)
    from matplotlib.gridspec import GridSpec
    figure = plt.figure(figsize=(13.0, 7.6))
    figure.suptitle("comparison: " + ", ".join(car.key for car in cars), fontsize=15,
                    x=0.012, ha="left", y=0.985)
    grid = GridSpec(1, 2, figure=figure, left=0.155, right=0.97, top=0.88, bottom=0.09, wspace=0.24)
    ax = figure.add_subplot(grid[0, 0])
    rows = []
    for key, label, unit, decimals, lower in BAR_METRICS:
        low, high = fleet_range(cars, key)
        rows.append([relative(metrics(car).get(key), low, high, lower) for car in cars])
    width = 0.8 / max(len(cars), 1)
    positions = list(range(len(BAR_METRICS)))
    for slot, car in enumerate(cars):
        ax.barh([p + slot * width for p in positions], [row[slot] for row in rows],
                height=width, label=car.key)
    ax.set_yticks([p + 0.4 for p in positions])
    ax.set_yticklabels([label for _, label, _, _, _ in BAR_METRICS], fontsize=9)
    ax.invert_yaxis()
    ax.set_xlim(0, 1.02)
    ax.set_title("each metric against the best of these cars", fontsize=10)
    ax.legend(fontsize=9, ncol=len(cars), frameon=False, loc="upper center",
              bbox_to_anchor=(0.5, -0.06))
    ax.grid(axis="x", color="#dddddd", linewidth=0.6)
    ax.set_axisbelow(True)
    ax = figure.add_subplot(grid[0, 1])
    limit = max(car.vmax for car in cars) * 3.6 * 1.05
    for car in cars:
        curve = [(v, a) for v, a in car.accel_curve() if v * 3.6 <= limit]
        ax.plot([v * 3.6 for v, _ in curve], [a for _, a in curve], linewidth=2, label=car.key)
    ax.set_xlim(0, limit)
    ax.set_xlabel("road speed (km/h)", fontsize=9)
    ax.set_ylabel("acceleration (m/s2)", fontsize=9)
    ax.set_title("acceleration", fontsize=10)
    ax.legend(fontsize=8, frameon=False)
    ax.grid(color="#eeeeee", linewidth=0.6)
    ax.set_axisbelow(True)
    ax.tick_params(labelsize=8)
    return _finish(figure, out, show)


def _finish(figure, out, show):
    if out:
        figure.savefig(out, dpi=110)
        print("wrote %s" % out)
    if show:
        _pyplot(True).show()
    _pyplot(False).close(figure)


# ------------------------------------------------------------ editing a car

def parse_assignment(text):
    """`mass=1500`, `torque=340,350,...`, `"final gear=4.10"` -> (field, values)."""
    if "=" not in text:
        raise CarFileError("%r is not field=value" % text)
    name, raw = text.split("=", 1)
    values = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            values.append(float(part) if ("." in part or "e" in part.lower()) else int(part))
        except ValueError:
            raise CarFileError("%r is not a number, in %r" % (part, text))
    if not values:
        raise CarFileError("%r has no values" % text)
    return name.strip(), values


def accel_samples(car, scale=1.0):
    """The 112 AI acceleration samples for the car as it now stands, at 1 m/s a step."""
    curve = car.physics_curve(step=0.25)
    samples = []
    for index in range(ACC_SAMPLES):
        speed = float(index)
        value = 0.0
        if curve and speed <= car.vmax * 1.02:
            value = Car._interpolate(curve, speed) * scale
        samples.append(max(0.0, value))
    return samples


def table_time(samples, speed):
    """Seconds to `speed` m/s, integrating samples taken 1 m/s apart."""
    curve = [(float(i), a) for i, a in enumerate(samples)]
    return Car.time_from_curve(curve, speed)


_CALIBRATION = {}


def calibration(root=CARMODEL):
    """How much the engine-curve estimate overstates the retail cars.

    The estimate has no clutch, tyre or traction loss in it, so it is quicker
    than the acceleration tables EA shipped.  Measured against the whole field
    it is a steady factor, and that factor is what `derive --scale auto` takes
    out -- the tuning still shows through, the optimism does not.
    """
    key = str(root)
    if key in _CALIBRATION:
        return _CALIBRATION[key]
    ratios = []
    for car in all_cars(root):
        table = table_time(car.accel_table(), 27.78)
        raw = table_time(accel_samples(car, 1.0), 27.78)
        if table and raw:
            ratios.append(raw / table)
    if ratios:
        ratios.sort()
        factor = ratios[len(ratios) // 2]
    else:
        factor = 1.0
    _CALIBRATION[key] = factor
    return factor


def derive_tables(car, scale="auto", match=None, root=CARMODEL):
    """Rewrite the eight AI ACC sections from the engine curve, gearing and mass.

    Those sections are one curve of acceleration against speed, and they are
    what the AI drives to; nothing else recomputes them, so a car whose engine,
    gearing or mass has been edited otherwise keeps the acceleration its donor
    had.  `scale` can be "auto" (keep this car's current 0-100, changing only
    the shape), a number, or 1.0 for the raw engine-curve estimate.
    """
    before = car.accel_table()
    if match is not None:
        factor = calibration(root)
        for _ in range(4):
            samples = accel_samples(car, factor)
            now = table_time(samples, 27.78)
            if not now:
                break
            factor *= now / match
    elif scale == "auto":
        factor = calibration(root)
        samples = accel_samples(car, factor)
    else:
        factor = float(scale)
        samples = accel_samples(car, factor)
    samples = accel_samples(car, factor)
    return samples, factor, table_time(before, 27.78), table_time(samples, 27.78)


def write_accel(carp, samples):
    for section in range(ACC_LAST - ACC_FIRST + 1):
        chunk = samples[section * 14:(section + 1) * 14]
        carp.set(ACC_FIRST + section, chunk)


def resolve_changes(carp, changes):
    """[(field number, values)] for every assignment, before anything is written."""
    resolved = []
    for name, values in changes:
        index = carp.index_of(name)
        before = carp.fields[index].values
        if len(values) != len(before) and not (len(before) == 1 and len(values) == 1):
            raise CarFileError(
                "%s holds %d values%s, not %d"
                % (FIELDS[index][1], len(before),
                   " (%s)" % ",".join(_short(v) for v in before), len(values)))
        resolved.append((index, values))
    return resolved


V2R_SETS = ((7, 8, 11), (76, 77, 79))  # velocity-to-rpm, ratios, final -- and the automatic ones


def rolling_circumference(carp):
    """The wheel's rolling circumference, out of the gearing as it stands.

    The game keeps `velocity to rpm ratio` and the gearing as two separate
    fields that have to agree -- `v2r = ratio * final * 60 / circumference` --
    so the tyre size falls out of the pair already in the file.  Read it before
    the gearing is changed, or the change hides itself inside it.
    """
    for v2r_index, ratio_index, final_index in V2R_SETS:
        if ratio_index not in carp.fields or final_index not in carp.fields:
            continue
        ratios = carp.fields[ratio_index].values
        v2r = carp.fields[v2r_index].values
        final = carp.number(final_index)
        for ratio, speed_ratio in zip(ratios, v2r):
            if ratio > 0 and speed_ratio > 0:
                return ratio * final * 60.0 / speed_ratio
    return None


def sync_v2r(carp, touched, rolling):
    """Recompute `velocity to rpm ratio` after the gearing moved.

    Without this a change to the gear ratios or the final drive is invisible:
    the game works out speed from `velocity to rpm ratio`, not from the ratios
    themselves.
    """
    if not (touched & {8, 11, 77, 79}) or not rolling:
        return []
    done = []
    for v2r_index, ratio_index, final_index in V2R_SETS:
        if ratio_index not in carp.fields or final_index not in carp.fields:
            continue
        ratios = carp.fields[ratio_index].values
        v2r = carp.fields[v2r_index].values
        final = carp.number(final_index)
        updated = []
        for ratio, before in zip(ratios, v2r):
            if ratio > 0 and before != 0:
                speed_ratio = ratio * final * 60.0 / rolling
                updated.append(math.copysign(speed_ratio, before))  # reverse stays negative
            else:
                updated.append(before)
        if updated != v2r:
            carp.set(v2r_index, updated)
            done.append(FIELDS[v2r_index][1])
    return done


def apply_changes(carp, changes, quiet=False):
    """Set each assignment, reporting what moved."""
    resolved = resolve_changes(carp, changes)
    for index, values in resolved:
        before = carp.fields[index].values
        if len(values) == len(before):
            carp.set(index, values)
        elif len(before) == 1 and len(values) == 1:
            carp.set(index, values[0])
        else:
            raise CarFileError(
                "%s holds %d values%s, not %d"
                % (FIELDS[index][1], len(before),
                   " (%s)" % ",".join(_short(v) for v in before), len(values)))
        after = carp.fields[index].values
        if not quiet:
            if len(before) == 1:
                print("  %-18s %s -> %s" % (FIELDS[index][1], _short(before[0]), _short(after[0])))
            else:
                print("  %-18s %d values changed" % (FIELDS[index][1], len(after)))
    return {index for index, _ in resolved}


def _short(value):
    if isinstance(value, float):
        return ("%.6f" % value).rstrip("0").rstrip(".") or "0"
    return str(value)


def _car_text_from_bytes(data):
    strings, start = [], None
    for offset in range(0xcf, min(len(data), 0x180)):
        byte = data[offset]
        if start is None:
            if 31 < byte < 127:
                start = offset
        elif byte == 0:
            text = data[start:offset].decode("latin-1").strip()
            if text:
                strings.append(text)
            start = None
    return (strings[0] if len(strings) > 0 else "",
            strings[1] if len(strings) > 1 else "",
            strings[2] if len(strings) > 2 else "")


def car_id(folder):
    path = Path(folder) / "fedata.eng"
    if not path.exists():
        return None
    return path.read_bytes()[0x18]


def bigf_entries(data):
    """[(name, offset, size)] for everything an archive holds."""
    if data[:4] != b"BIGF":
        raise CarFileError("not a BIGF archive")
    count, = struct.unpack(">I", data[8:12])
    at = 16
    out = []
    for _ in range(count):
        offset, size = struct.unpack(">II", data[at:at + 8])
        at += 8
        end = data.index(b"\0", at)
        out.append((data[at:end].decode("latin-1"), offset, size))
        at = end + 1
    return out


def read_carp_text(folder):
    """(bytes, from_archive) -- the car's carp.txt, out of its archive if it can."""
    folder = Path(folder)
    archive = folder / "car.viv"
    if archive.exists():
        data = archive.read_bytes()
        try:
            for name, offset, size in bigf_entries(data):
                if name.lower() == "carp.txt":
                    return data[offset:offset + size], True
        except (CarFileError, struct.error, ValueError):
            pass
    loose = folder / "carp.txt"
    if loose.exists():
        return loose.read_bytes(), False
    raise CarFileError("%s: no carp.txt, in car.viv or loose" % folder)


def replace_archive_entry(archive, wanted, payload):
    """Put `payload` where an entry was, in place if it fits, else rebuild.

    The entries' offsets are the archive's own table, so a shorter file can be
    padded where it lies and a longer one needs the table rewritten.  Either
    way the game seeks to what the table says, so both are valid archives.
    """
    archive = Path(archive)
    data = archive.read_bytes()
    entries = bigf_entries(data)
    wanted_low = wanted.lower()
    for name, offset, size in entries:
        if name.lower() == wanted_low and len(payload) <= size:
            body = bytearray(data)
            body[offset:offset + len(payload)] = payload
            body[offset + len(payload):offset + size] = b"\0" * (size - len(payload))
            archive.write_bytes(bytes(body))
            return "in place"
    # rebuild: the table again, with the new contents
    contents = []
    for name, offset, size in entries:
        contents.append((name, payload if name.lower() == wanted_low else data[offset:offset + size]))
    if not any(name.lower() == wanted_low for name, _, _ in entries):
        contents.append((wanted, payload))
    start = 16 + sum(8 + len(name.encode("latin-1")) + 1 for name, _ in contents)
    table = []
    offset = start
    for name, body in contents:
        table.append(struct.pack(">II", offset, len(body)) + name.encode("latin-1") + b"\0")
        offset += len(body)
    header = b"BIGF" + struct.pack(">III", offset, len(contents), start)
    archive.write_bytes(header + b"".join(table) + b"".join(body for _, body in contents))
    return "rebuilt"


def _bigf_entry(data, wanted):
    """(offset, size) of a file inside a BIGF archive, or None."""
    if data[:4] != b"BIGF":
        return None
    count, = struct.unpack(">I", data[8:12])
    at = 16
    for _ in range(count):
        offset, size = struct.unpack(">II", data[at:at + 8])
        at += 8
        end = data.index(b"\0", at)
        name = data[at:end].decode("latin-1")
        at = end + 1
        if name.lower() == wanted.lower():
            return offset, size
    return None


def rename_car(folder, new_id=None, new_name=None):
    """Give a car a new id and display name, in `fedata.eng` and in its archive.

    Both are fixed-width there -- the id is one byte, and the name sits where it
    sits with the archive's offsets worked out around it -- so a name longer
    than the old one cannot be written.
    """
    folder = Path(folder)
    eng_path = folder / "fedata.eng"
    if not eng_path.exists():
        return []
    original = eng_path.read_bytes()
    _, _, full = _car_text_from_bytes(original)
    name_offset = -1
    if full:
        found = original.find(b"\0" + full.encode("latin-1") + b"\0")
        if found >= 0:
            name_offset = found + 1
    encoded = None
    if new_name:
        encoded = new_name.encode("latin-1", "replace")
        if name_offset < 0:
            raise CarFileError("%s: cannot find the name to replace" % folder)
        if len(encoded) > len(full):
            raise CarFileError("this car's name is %d characters wide (%r); %r is %d"
                               % (len(full), full, new_name, len(encoded)))
        encoded = encoded.ljust(len(full), b" ")
    edited = bytearray(original)
    done = []
    if new_id is not None:
        done.append("id %d -> %d" % (original[0x18], new_id))
        edited[0x18] = int(new_id) & 0xFF
    if encoded is not None:
        done.append("name %r -> %r" % (full, new_name))
        edited[name_offset:name_offset + len(encoded)] = encoded
    if not done:
        return []
    eng_path.write_bytes(bytes(edited))
    archive = folder / "car.viv"
    if archive.exists():
        data = bytearray(archive.read_bytes())
        entry = _bigf_entry(data, "fedata.eng")
        if entry:
            base, _ = entry
            if new_id is not None:
                data[base + 0x18] = int(new_id) & 0xFF
            if encoded is not None:
                data[base + name_offset:base + name_offset + len(encoded)] = encoded
            archive.write_bytes(bytes(data))
            done.append("(and inside car.viv)")
    return done


def used_ids(root):
    found = {}
    for car in all_cars(root):
        value = car_id(car.folder)
        if value is not None:
            found[value] = car.key
    return found


# --------------------------------------------------------------------- cli

def _load_changes(sets, preset):
    changes = []
    if preset:
        data = json.loads(Path(preset).read_text())
        if not isinstance(data, dict):
            raise CarFileError("%s: a preset is an object of field: value" % preset)
        for name, value in data.items():
            changes.append((name, value if isinstance(value, list) else [value]))
    for text in sets:
        changes.append(parse_assignment(text))
    return changes


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Show, compare and tune the performance in a car's carp.txt.",
        epilog="`--fields` (with `list`) prints every field you can set.")
    parser.add_argument("--root", default=str(CARMODEL),
                        help="the carmodel folder (default: %(default)s)")
    sub = parser.add_subparsers(dest="command", required=True)

    listing = sub.add_parser("list", help="every car and its headline numbers")
    listing.add_argument("--fields", action="store_true", help="list the fields instead")

    show = sub.add_parser("show", help="draw one car")
    show.add_argument("car")
    show.add_argument("--out", help="write a PNG instead of opening a window")
    show.add_argument("--no-window", action="store_true", help="never open a window")

    compare = sub.add_parser("compare", help="draw several cars against each other")
    compare.add_argument("cars", nargs="+")
    compare.add_argument("--out")
    compare.add_argument("--no-window", action="store_true")

    setter = sub.add_parser("set", help="change values in place, or in a copy")
    setter.add_argument("car")
    setter.add_argument("changes", nargs="*", help="field=value, e.g. mass=1500")
    setter.add_argument("--preset", help="a JSON object of field: value, applied first")
    setter.add_argument("--out", help="write the car here instead of editing it")
    setter.add_argument("--dry-run", action="store_true")
    setter.add_argument("--keep-v2r", action="store_true",
                        help="do not recompute velocity-to-rpm when the gearing changes")

    derive = sub.add_parser("derive",
                            help="rebuild the AI acceleration tables from the engine and gearing")
    derive.add_argument("car")
    derive.add_argument("--scale", default="auto",
                        help="auto (keep this car's 0-100, default), a number, or 1.0 for raw")
    derive.add_argument("--match", type=float,
                        help="a 0-100 to aim the derived curve at, in seconds")
    derive.add_argument("--out", help="write a copy of the car here")
    derive.add_argument("--dry-run", action="store_true")

    clone = sub.add_parser("clone", help="copy a car to a new folder with a new id and name")
    clone.add_argument("car")
    clone.add_argument("target", help="the new folder, under the carmodel folder")
    clone.add_argument("--id", type=int, help="the copy's id byte (must be unused)")
    clone.add_argument("--name", help="the display name (no longer than the old one)")
    clone.add_argument("changes", nargs="*", help="field=value for the copy's performance")
    clone.add_argument("--preset")
    clone.add_argument("--dry-run", action="store_true")
    clone.add_argument("--keep-v2r", action="store_true")
    clone.add_argument("--derive", action="store_true",
                       help="rebuild the AI acceleration tables from the tuned engine")

    args = parser.parse_args(argv)
    root = Path(args.root)
    try:
        if args.command == "list":
            if args.fields:
                for index in sorted(FIELDS):
                    label, short, unit = FIELDS[index]
                    print("%-3d %-24s %-10s %s" % (index, short, unit, label))
                return 0
            print("%-6s %-24s %-5s %6s %6s %6s %7s %7s"
                  % ("car", "name", "cls", "kg", "kW", "km/h", "0-100", "400 m"))
            for car in all_cars(root):
                values = metrics(car)
                print("%-6s %-24s %-5s %6.0f %6.0f %6.0f %7s %7s"
                      % (car.key, car.display_name()[:24], values["class"], car.mass,
                         values["power"], values["top"],
                         "--" if values["t100"] is None else "%.2f" % values["t100"],
                         "--" if values["quarter"] is None else "%.2f" % values["quarter"]))
            return 0

        if args.command == "show":
            car = find_car(args.car, root)
            draw_car(car, all_cars(root), out=args.out,
                     show=not args.no_window and not args.out)
            return 0

        if args.command == "compare":
            cars = [find_car(name, root) for name in args.cars]
            draw_compare(cars, out=args.out, show=not args.no_window and not args.out)
            return 0

        if args.command == "set":
            car = find_car(args.car, root)
            changes = _load_changes(args.changes, args.preset)
            if not changes:
                parser.error("nothing to set: pass field=value or --preset")
            if args.dry_run:
                resolve_changes(car.carp, changes)
                print("%s (%s):" % (car.key, car.folder))
                apply_changes(car.carp, changes)
                print("  (nothing written)")
                return 0
            resolve_changes(car.carp, changes)
            if args.out:
                target = Path(args.out)
                if target.exists():
                    raise CarFileError("%s already exists" % target)
                shutil.copytree(car.folder, target)
                car = Car(target)
            print("%s (%s):" % (car.key, car.folder))
            rolling = rolling_circumference(car.carp)
            touched = apply_changes(car.carp, changes)
            for name in ([] if args.keep_v2r else sync_v2r(car.carp, touched, rolling)):
                print("  %-18s recomputed from the gearing" % name)
            written = car.carp.save()
            print("  written to %s" % " and ".join(written))
            return 0

        if args.command == "derive":
            car = find_car(args.car, root)
            samples, factor, was, now = derive_tables(car, args.scale, args.match, root)
            print("%s (%s):" % (car.key, car.folder))
            print("  acceleration table  0-100 %s -> %s   (scale %.3f%s)"
                  % ("%.2f s" % was if was else "--", "%.2f s" % now if now else "--", factor,
                     ", the model's own optimism" if args.scale == "auto" and args.match is None
                     else ""))
            if args.dry_run:
                print("  (nothing written)")
                return 0
            if args.out:
                target = Path(args.out)
                if target.exists():
                    raise CarFileError("%s already exists" % target)
                shutil.copytree(car.folder, target)
                car = Car(target)
            write_accel(car.carp, samples)
            print("  written to %s" % " and ".join(car.carp.save()))
            return 0

        if args.command == "clone":
            source = find_car(args.car, root)
            target = root / args.target
            if target.exists():
                raise CarFileError("%s already exists" % target)
            if args.id is not None:
                if not 0 <= args.id <= 255:
                    raise CarFileError("the id is one byte: 0 to 255")
                taken = used_ids(root)
                if args.id in taken:
                    raise CarFileError("id %d is already %s" % (args.id, taken[args.id]))
            changes = _load_changes(args.changes, args.preset)
            resolve_changes(source.carp, changes)
            if args.name:
                _, _, full = _car_text_from_bytes((source.folder / "fedata.eng").read_bytes())
                if len(args.name) > len(full):
                    raise CarFileError("this car's name is %d characters wide (%r); %r is %d"
                                       % (len(full), full, args.name, len(args.name)))
            if args.dry_run:
                print("%s -> %s" % (source.key, target))
                if args.name:
                    print("  name -> %r" % args.name)
                if args.id is not None:
                    print("  id -> %d" % args.id)
                apply_changes(source.carp, changes)
                print("  (nothing written)")
                return 0
            shutil.copytree(source.folder, target)
            try:
                for line in rename_car(target, args.id, args.name):
                    print("  %s" % line)
                if changes:
                    copy = Car(target)
                    print("%s:" % target.name)
                    rolling = rolling_circumference(copy.carp)
                    touched = apply_changes(copy.carp, changes)
                    for name in ([] if args.keep_v2r else sync_v2r(copy.carp, touched, rolling)):
                        print("  %-18s recomputed from the gearing" % name)
                    if args.derive:
                        samples, factor, was, now = derive_tables(copy, root=root)
                        write_accel(copy.carp, samples)
                        print("  %-18s 0-100 %s -> %s (scale %.3f)"
                              % ("acceleration table",
                                 "%.2f s" % was if was else "--", "%.2f s" % now if now else "--",
                                 factor))
                    print("  written to %s" % " and ".join(copy.carp.save()))
            except Exception:
                shutil.rmtree(target, ignore_errors=True)
                raise
            print("made %s" % target)
            return 0
    except CarFileError as error:
        print("error: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
