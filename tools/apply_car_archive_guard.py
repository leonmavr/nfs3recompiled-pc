"""A car folder with no car.viv is skipped, not a crash.

Cars are found by listing gamedata/carmodel, and each folder the game finds is
loaded from its own archive, `<folder>/car.viv`.  A folder without one is not a
car the game can use -- but it does not stop: sub_49c700 builds the path, opens
it (fopen answers NULL), and carries on with that NULL.  The first thing to
touch it is the name lookup sub_4679f0, which reads the archive's table through
the pointer and dies:

    sub_4679f0(archive = NULL) -> mov eax,[ebp-4]; cmp edi,[eax+8]  -> SIGSEGV

The port's C runtime already answers a null FILE* with an error rather than a
crash (nfs3hp/native_watcom.cpp, nullStream), which gets the game past the
seek, read and close that follow.  This is the other half: the lookup itself,
which takes the archive as a pointer rather than a stream.  Returning at once
leaves the caller's "is this car there" answer empty, so the folder is skipped
and every other car loads as usual.

That is what a modder needs when a folder is half-made: a car that does not
appear, and a log line naming it, instead of a segfault on the way in.

Said once, by the log, before any of it: main() checks every car folder for its
archive and names the ones without (nfs3hp_main.cpp, checkCarArchives).
"""
from pathlib import Path

FILE = "nfs3hp.17.cpp"

HEADER = '''void Application::sub_4679f0(WinApplication* __restrict app, x86::CPU& cpu_)
{
  x86::Local cpu(cpu_);
  NFS2_USE(cpu);
  NFS2_USE(app);
'''

GUARD = '''  /* Port (tools/apply_car_archive_guard.py): no archive for this car -- the
   * folder it came from has no car.viv.  Answer "not found" rather than read
   * through a null table. */
  if (!cpu.eax)
    return;
'''


def apply(root: Path) -> None:
    path = root / "src/nfs3hp/disassembly" / FILE
    text = path.read_text(newline="")
    eol = "\r\n" if text.count("\r\n") * 2 > text.count("\n") else "\n"

    def native(s):
        return eol.join(s.split("\n"))

    header, guard = native(HEADER), native(GUARD)
    if guard in text:
        return
    if text.count(header) != 1:
        raise RuntimeError("%s: sub_4679f0 opening changed" % FILE)
    text = text.replace(header, header + guard, 1)
    path.write_text(text, newline="")


if __name__ == "__main__":
    apply(Path(__file__).resolve().parents[1])
