"""Add a downloaded car to the game without touching the cars already there.

    python3 tools/import_car.py GAME_DIR SOURCE [SOURCE ...] [--name NAME] [--dry-run]

GAME_DIR is the folder holding fedata/ and gamedata/.  A SOURCE is what was
downloaded: a .zip, a .viv, or a folder it was unpacked into.  Add-on cars come
packed every which way -- a bare car.viv, an `Ftes.viv` to be renamed, a
`GameData/CarModel/<car>/` tree with its speech banks beside it, several cars in
one zip -- so every archive in the source that is a car (a BIGF holding a
`fedata.*` and a `car.fce`) is found and installed on its own.

Each one becomes a new folder in gamedata/carmodel, and nothing already there
changes:

- **Its folder** is the name it came with (the folder it sat in, or the
  archive's own name when that is not just "car", or its four-letter code),
  four characters at most as the game's own are.  If a car already has that
  folder, the new one gets another name (`lstm` -> `lst2`) instead of replacing
  it; `--name` picks one.
- **Its ID**, the byte at 0x18 of its `fedata.*` text, is kept when no other
  car has it, and otherwise moved to the first one free (67 to 255 first: 2 to
  49 are the retail cars' and 50 to 66 traffic's).  Its four-letter code, the
  first bytes of the same text, is changed to the folder's name when another
  car has the same: the game compares that header across all cars and stops
  with "Duplicate cars found in carmodel folder!" on a repeat.  Both are
  patched in place inside the archive, in every language.
- **Its pictures** -- the showcase art named after it (`h<car>.qfs`,
  `sh<car>.qfs`, `sl<car>NN.qfs`, `in_<car>.fsh`) and its slide named after its
  ID (`<id>_00.qfs`) -- go into fedata/art under its new folder name and ID,
  and only where nothing of that name is there yet.  Files every car shares,
  `compare.fsh` among them, are never written: add-ons ship a whole copy of it,
  and taking theirs would replace every other car's comparison picture.

Then tools/extract_viv.py's unpacking is done for it, as for the disc's cars.

Where the new car comes in the list is the game's to keep
(gamedata/carorder.txt, nfs3hp_main.cpp): a car not named there yet goes after
every car that is, so the cars already chosen -- yours, the opponents' -- stay
the cars they were.

Two more things it does with what is already installed:

    python3 tools/import_car.py GAME_DIR --list
    python3 tools/import_car.py GAME_DIR --remove CAR [CAR ...] [--backup DIR]

`--list` prints every car there with its four-letter code, its ID, its make and
model, and marks the ones that are another's duplicate -- two folders of the
same car, which happens when an add-on arrives twice under two names, and which
the menus show as two identical entries.

`--remove` takes cars out: the folder is zipped whole into `--backup` (default
the system's temporary folder, so nothing is lost) and then deleted, and its
line comes out of gamedata/carorder.txt with it.
"""
import argparse
import hashlib
import re
import shutil
import struct
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_viv import entries, extract  # noqa: E402

ID_AT = 0x18
HEADER = 16
FREE_IDS = list(range(67, 256)) + list(range(2, 50))
SHOWCASE = re.compile(r"^(h|sh|in_|sl)(.+?)(\d\d)?\.(qfs|fsh)$", re.I)
SLIDE = re.compile(r"^(\d+)_00\.qfs$", re.I)


def find(folder, name):
    """A child of `folder` by name, any case; None when there is none."""
    if not folder.is_dir():
        return None
    for child in folder.iterdir():
        if child.name.lower() == name.lower():
            return child
    return None


def car_texts(data):
    """(offset, size) of every fedata.<language> in an archive, and whether it
    holds a model: None when it is not a car's archive at all."""
    try:
        found = list(entries(data))
    except (ValueError, struct.error, IndexError):
        return None
    texts = [(o, s) for n, o, s in found if n.lower().startswith("fedata.") and n.lower() != "fedata.fsh"
             and s > ID_AT]
    model = any(n.lower() == "car.fce" for n, _, _ in found)
    return (texts, model) if texts else None


def car_text(folder):
    """The first bytes of a car's fedata text -- its code, its header and its
    ID -- from its archive, or from a loose fedata.eng; None where there is no
    car's text in the folder at all."""
    archive = find(folder, "car.viv")
    text = None
    if archive:
        data = archive.read_bytes()
        known = car_texts(data)
        if known:
            offset = known[0][0][0]
            text = data[offset:offset + HEADER + 9]
    if text is None:
        loose = find(folder, "fedata.eng")
        if loose:
            text = loose.read_bytes()[:HEADER + 9]
    return text if text and len(text) > ID_AT else None


def installed(carmodel):
    """name -> (id, 16-byte header) of every car already in carmodel."""
    cars = {}
    for folder in car_folders(carmodel):
        text = car_text(folder)
        if text:
            cars[folder.name.lower()] = (text[ID_AT], text[:HEADER])
    return cars


def car_folders(carmodel):
    """Every car's folder in carmodel, in name order; not the traffic's."""
    return [f for f in sorted(carmodel.iterdir()) if f.is_dir() and f.name.lower() != "traffic"]


def words_of(folder):
    """What a car's text holds as words, in order: its four-letter code, make,
    model, full name, then its statistics -- from a loose fedata.eng, else from
    the one in its archive."""
    text = None
    loose = find(folder, "fedata.eng")
    if loose:
        text = loose.read_bytes()
    else:
        archive = find(folder, "car.viv")
        if archive:
            data = archive.read_bytes()
            try:
                for entry, offset, size in entries(data):
                    if entry.lower() == "fedata.eng":
                        text = data[offset:offset + size]
                        break
            except (ValueError, struct.error, IndexError):
                text = None
    if not text:
        return []
    # A byte of the header that happens to be a printable character comes out a
    # run of its own: the words are the runs of three letters or more.
    return [run.decode("latin-1").strip() for run in re.split(rb"[^\x20-\x7e]+", text) if len(run.strip()) >= 3]


def name_of(folder, text):
    """A car's make and model, as the menus show them: the first two words
    after its code; its code where its text has no more."""
    words = words_of(folder)
    if len(words) > 2:
        return " ".join(words[1:3])
    return words[1] if len(words) > 1 else text[:4].decode("latin-1").strip()


def listing(carmodel):
    """Every car installed, with the ones that are another's duplicate marked."""
    rows = []
    for folder in car_folders(carmodel):
        text = car_text(folder)
        if not text:
            continue
        rows.append((folder.name, text[:4].decode("latin-1").strip(), text[ID_AT], name_of(folder, text)))
    groups = {}
    for name, code, car_id, car in rows:
        groups.setdefault(car.lower(), []).append(name)
    width = max([len(car) for _, _, _, car in rows] + [1])
    for name, code, car_id, car in rows:
        same = [other for other in groups[car.lower()] if other != name]
        print("%-8s %-4s ID %3d  %-*s%s" % (name, code, car_id, width, car,
                                            "  same car as " + ", ".join(same) if same else ""))
    print("%d car%s, %d of them a duplicate" %
          (len(rows), "" if len(rows) == 1 else "s", sum(len(g) - 1 for g in groups.values())))


def remove(game, names, backup, dry):
    """Take cars out: each folder zipped into `backup`, then gone, and its line
    out of gamedata/carorder.txt with it."""
    carmodel = find(game, "gamedata") / "carmodel"
    order = find(find(game, "gamedata"), "carorder.txt")
    gone, missing = [], []
    for wanted in names:
        folder = find(carmodel, wanted)
        if folder is None or not folder.is_dir() or folder.name.lower() == "traffic":
            missing.append(wanted)
            continue
        archive = Path(backup) / ("carmodel-%s-%s.zip" % (folder.name, time.strftime("%Y%m%d-%H%M%S")))
        if dry:
            print("%s: would zip to %s and remove" % (folder.name, archive))
            gone.append(folder.name)
            continue
        Path(backup).mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            for file in sorted(folder.rglob("*")):
                if file.is_file():
                    zipped.write(file, str(Path(folder.name) / file.relative_to(folder)))
        shutil.rmtree(folder)
        print("%s: %s (%s)" % (folder.name, archive, "and removed" if not dry else ""))
        gone.append(folder.name)
    if not dry and gone and order:
        lines = order.read_text(encoding="latin-1").splitlines(True)
        kept = [line for line in lines
                if not (line.strip() and not line.lstrip().startswith("#")
                        and line.strip().lower() in {name.lower() for name in gone})]
        order.write_text("".join(kept), encoding="latin-1")
        print("gamedata/carorder.txt: %d line%s out" % (len(lines) - len(kept),
              "" if len(lines) - len(kept) == 1 else "s"))
    for name in missing:
        print("%s: no such car" % name)
    print("%d car%s %s" % (len(gone), "" if len(gone) == 1 else "s",
                           "would be removed" if dry else "removed"))


def download_name(label):
    """The car's name from the download's: `nfsaddons.com__1745522156_FordGT.zip`
    is FordGT, `1243872229-Stratos.zip` Stratos."""
    stem = Path(str(label)).name
    stem = re.sub(r"\.(zip|viv)$", "", stem, flags=re.I)
    stem = re.sub(r"^nfsaddons\.com_+", "", stem, flags=re.I)
    stem = re.sub(r"^\d+[-_ ]*", "", stem)
    stem = re.sub(r"\(\d+\)$", "", stem)
    return stem


def folder_name(archive, root, text, label="", taken=()):
    """The name the car came with, four letters at most: the archive's own name
    when that is not just "car", else its four-letter code, else the
    download's name (its folders -- "Race", "realistic performance" -- say
    which version, not which car), else the folder it sat in.  A name another
    car has is passed over while there is one that is free."""
    candidates = []
    if archive.stem.lower() != "car":
        candidates.append(archive.stem)
    candidates.append(text[:4].decode("latin-1"))
    if label:
        candidates.append(download_name(label))
    if archive.parent != root:
        candidates.append(archive.parent.name)
    names = []
    for candidate in candidates:
        name = re.sub(r"[^a-z0-9]", "", candidate.lower())[:4]
        if name and name not in ("car", "game", "data", "carm"):
            names.append(name)
    for name in names:
        if name not in taken:
            return name
    # All taken: the download's name, numbered (clkg, clk2), else the first.
    if label:
        name = re.sub(r"[^a-z0-9]", "", download_name(label).lower())[:4]
        if name:
            return name
    return names[0] if names else "addn"


def unique_name(name, taken):
    if name not in taken:
        return name
    for n in range(2, 1000):
        suffix = str(n)
        candidate = name[:4 - len(suffix)] + suffix
        if candidate not in taken:
            return candidate
    raise RuntimeError("no free folder name for %s" % name)


def sources(paths, scratch):
    """Every source unpacked to a folder: (folder, label)."""
    for path in paths:
        path = Path(path)
        if path.is_dir():
            yield path, path.name
        elif zipfile.is_zipfile(path):
            target = Path(tempfile.mkdtemp(dir=scratch))
            with zipfile.ZipFile(path) as archive:
                for member in archive.infolist():
                    name = member.filename.replace("\\", "/")
                    if member.is_dir() or name.startswith("/") or ".." in name.split("/"):
                        continue
                    out = target / name
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(member) as src, open(out, "wb") as dst:
                        shutil.copyfileobj(src, dst)
            yield target, path.name
        elif path.is_file():
            target = Path(tempfile.mkdtemp(dir=scratch))
            shutil.copy(path, target / path.name)
            yield target, path.name
        else:
            print("%s: not found" % path, file=sys.stderr)


def art_files(root, old_name, old_id):
    """The source's pictures of this car: (file, kind, suffix)."""
    for file in root.rglob("*"):
        if not file.is_file():
            continue
        slide = SLIDE.match(file.name)
        if slide:
            if int(slide.group(1)) == old_id:
                yield file, "slide", ""
            continue
        shown = SHOWCASE.match(file.name)
        if shown and shown.group(2).lower() == old_name.lower():
            yield file, shown.group(1).lower(), shown.group(3) or ""


def import_car(game, archive, root, label, wanted, cars, dry):
    data = bytearray(archive.read_bytes())
    known = car_texts(bytes(data))
    if known is None:
        return False
    texts, model = known
    if not model:
        print("%s: %s has no car.fce in it -- not a car this game can draw" % (label, archive.name))
        return False
    first = texts[0][0]
    text = bytes(data[first:first + HEADER + 9])
    old_id = text[ID_AT]
    old_code = text[:4].decode("latin-1")
    carmodel = find(game, "gamedata") / "carmodel"

    taken = set(cars) | {f.name.lower() for f in carmodel.iterdir()}
    came_as = folder_name(archive, root, text, label, taken)
    name = unique_name(re.sub(r"[^a-z0-9]", "", wanted.lower())[:4] if wanted else came_as, taken)

    used = {car_id for car_id, _ in cars.values()}
    new_id = old_id
    # 50 to 66 are traffic's: an add-on that came with one is moved too.
    if old_id in used or 50 <= old_id <= 66:
        new_id = next(i for i in FREE_IDS if i not in used)
    headers = {header for _, header in cars.values()}
    code = text[:4]
    if text[:HEADER] in headers or any(h[:4].lower() == code.lower() for h in headers):
        code = name.upper().ljust(4, "_").encode("latin-1")
    for offset, _ in texts:
        data[offset + ID_AT] = new_id
        data[offset:offset + 4] = code

    notes = []
    if wanted:
        if name != came_as:
            notes.append("folder %s as asked" % name)
    elif name != came_as:
        notes.append("folder %s taken, so %s" % (came_as, name))
    if new_id != old_id:
        notes.append("ID %d taken%s, so %d" % (old_id, " (traffic's)" if 50 <= old_id <= 66 else "", new_id))
    if code != text[:4]:
        notes.append("code %s taken, so %s" % (old_code, code.decode("latin-1")))
    print("%s: %s -> gamedata/carmodel/%s (ID %d)%s" % (label, archive.relative_to(root), name, new_id,
                                                     "; " + "; ".join(notes) if notes else ""))

    target = carmodel / name
    showcase = find(game, "fedata") / "art" / "showcase"
    slides = find(game, "fedata") / "art" / "slides"
    art = []
    for file, kind, suffix in art_files(root, came_as, old_id):
        if kind == "slide":
            art.append((file, slides / ("%02d_00.qfs" % new_id)))
        elif kind == "in_":
            art.append((file, showcase / ("in_%s%s" % (name, file.suffix.lower()))))
        else:
            art.append((file, showcase / ("%s%s%s%s" % (kind, name, suffix, file.suffix.lower()))))
    if not art and old_code.strip():
        art = [(f, showcase / ("%s%s%s%s" % (k, name, s, f.suffix.lower())))
               for f, k, s in art_files(root, old_code, -1) if k != "slide"]

    if dry:
        for src, dst in art:
            print("    would copy %s -> %s%s" % (src.name, dst.relative_to(game),
                                                 " (there already: left alone)" if dst.exists() else ""))
        cars[name] = (new_id, bytes(data[first:first + HEADER]))
        return True

    target.mkdir(parents=True)
    # The archive, and whatever came beside it that is this car's own:
    # its speech, its sound banks.  Not other cars' archives, not readmes.
    (target / "car.viv").write_bytes(bytes(data))
    for sibling in archive.parent.iterdir():
        if sibling == archive or not sibling.is_file():
            continue
        lower = sibling.name.lower()
        if lower.endswith(".bnk") or (lower.startswith("ipspch") and lower.endswith(".viv")):
            shutil.copy(sibling, target / lower)
    for src, dst in art:
        if dst.exists():
            print("    %s is there already: left alone" % dst.relative_to(game))
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(src, dst)
        print("    %s" % dst.relative_to(game))
    extract(target / "car.viv", False)
    cars[name] = (new_id, bytes(data[first:first + HEADER]))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("game", help="the folder holding fedata/ and gamedata/")
    parser.add_argument("source", nargs="*", help="a .zip, a .viv or a folder")
    parser.add_argument("--name", help="the folder to install a single car as")
    parser.add_argument("--list", action="store_true", help="list the cars installed, duplicates marked")
    parser.add_argument("--remove", nargs="+", metavar="CAR", help="take cars out, zipping each to --backup first")
    parser.add_argument("--backup", default=tempfile.gettempdir(), help="where --remove puts each car's zip")
    parser.add_argument("--dry-run", action="store_true", help="say what would be done, change nothing")
    args = parser.parse_args()

    game = Path(args.game)
    gamedata = find(game, "gamedata")
    if not gamedata or not find(gamedata, "carmodel") or not find(game, "fedata"):
        sys.exit("%s: no fedata/ and gamedata/carmodel/ there" % game)
    if args.list:
        listing(gamedata / "carmodel")
        return
    if args.remove:
        remove(game, args.remove, args.backup, args.dry_run)
        return
    if not args.source:
        parser.error("nothing to import: give a source, or --list / --remove")
    cars = installed(gamedata / "carmodel")
    done = 0
    seen = set()   # an archive packed twice in one download (a copy in a second folder) goes in once
    with tempfile.TemporaryDirectory() as scratch:
        for root, label in sources(args.source, Path(scratch)):
            found = 0
            for archive in sorted(root.rglob("*")):
                if not archive.is_file() or archive.suffix.lower() != ".viv":
                    continue
                head = archive.open("rb").read(4)
                if head != b"BIGF":
                    print("%s: %s is not an archive (%r...) -- a broken or fake download" %
                          (label, archive.relative_to(root), archive.open("rb").read(24)))
                    continue
                digest = hashlib.sha1(archive.read_bytes()).hexdigest()
                if digest in seen:
                    print("%s: %s is the same as one already taken, skipped" % (label, archive.relative_to(root)))
                    continue
                seen.add(digest)
                if import_car(game, archive, root, label, args.name, cars, args.dry_run):
                    found += 1
            if not found:
                print("%s: no car in it" % label)
            done += found
    print("%d car%s %s" % (done, "" if done == 1 else "s", "would be added" if args.dry_run else "added"))


if __name__ == "__main__":
    main()
