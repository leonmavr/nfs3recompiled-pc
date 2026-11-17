#ifndef NFS3HP_NATIVE_EXHAUSTS_H_
#define NFS3HP_NATIVE_EXHAUSTS_H_

#include <x86.h>
#include <string>
#include <vector>

namespace win32
{
class WinApplication;
}

namespace nfs3hp
{

/* One of a car's exhausts, as the marking tool put it (tools/car_exhausts.py,
 * or the editor's "Mark exhausts..."): in the car model's own space, metres --
 * x to the car's right, y up, z forward, the rear at the smallest z -- with the
 * pipe's radius. */
struct ExhaustTip
{
    float x, y, z, radius;
};

/* A car's exhausts and the colour its flames burn (0 to 1 each), read from the
 * `exhausts.json` in the car's folder.  The file may name the colour -- "flame":
 * "#f2380a", or three numbers 0 to 1 or 0 to 255 -- and it is this one when it
 * does not (native_exhausts.cpp, TECHNICAL.md). */
struct CarExhausts
{
    std::string folder;   // where it was found, from gamedata/carmodel/
    std::vector<ExhaustTip> tips;
    float flame[3] = { 0.949f, 0.2196f, 0.0392f };   // #f2380a
};

/* The exhausts of the car at this address of the race's cars (0x5e1120, a slot
 * each), or null when it has none marked -- and then it shows no flames.  The
 * car is found by the body name the race gave it ([car+0x1e4], what the AI's
 * own trace prints), every car of the race carrying one; failing that by its id
 * ([record+0x14]) the game's own way (sub_4392b0's). */
const CarExhausts* carExhausts(win32::WinApplication* app, x86::reg32 car);

/* Whether flames are wanted at all (NFS_EXHAUST_FLAMES, on; 0 or off for none). */
bool exhaustFlamesOn();

/* Whether a car with none marked gets two default pipes at its rear corners
 * instead of none (NFS_EXHAUST_DEFAULTS, off; 1 or on for them).  A car with a
 * file keeps its own, however many tips that names. */
bool exhaustDefaultsOn();

/* Read the exhausts of every car now, rather than at the first race frame, and
 * say what was found (NFS_EXHAUSTS_TRACE): main() calls it when asked, so the
 * files can be checked without driving a lap. */
void loadCarExhausts();

}

#endif /* !NFS3HP_NATIVE_EXHAUSTS_H_ */
