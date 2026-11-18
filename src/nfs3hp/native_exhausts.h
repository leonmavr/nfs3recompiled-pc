#ifndef NFS3HP_NATIVE_EXHAUSTS_H_
#define NFS3HP_NATIVE_EXHAUSTS_H_

#include <x86.h>
#include <cstdint>
#include <memory>
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
/* The car's own body, for cutting its flames where the body stands between
 * them and the camera (exhaustBodyLetsThrough): the high-detail body's
 * triangles out of its car.fce, in the same model space as the exhausts, with
 * a box tree over them so a ray asks only the few it can meet. */
struct CarBody
{
    struct Node
    {
        float lo[3], hi[3];
        std::uint32_t first, count;   // a leaf's triangles in `order`; count 0 for a branch
        std::uint32_t left, right;    // a branch's two children
    };
    std::vector<float> corners;          // nine floats a triangle
    std::vector<std::uint32_t> order;    // the triangles, as the tree's leaves take them
    std::vector<Node> nodes;             // nodes[0] the root
};

struct CarExhausts
{
    std::string folder;   // where it was found, from gamedata/carmodel/
    std::vector<ExhaustTip> tips;
    float flame[3] = { 0.949f, 0.2196f, 0.0392f };   // #f2380a
    std::shared_ptr<const CarBody> body;   // null where the car's model could not be read
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

/* How much of a flame is seen past its own car's body, 0 to 1: rays from the
 * camera (`eye`, in the car's own space) to the flame's middle (`at`) and to four
 * points round it (`radius` out, square to the view) are tried against the body's
 * triangles, and the share that get through is the answer.  A hit within a
 * hand's width of the flame is the pipe it comes out of, not the body in front
 * of it, and does not count.  1 where the car has no body read, or where the
 * camera is inside the body (a view from in the car, which draws none of it). */
float exhaustBodyLetsThrough(const CarExhausts& exhausts, const float eye[3], const float at[3], float radius);

/* Read the exhausts of every car now, rather than at the first race frame, and
 * say what was found (NFS_EXHAUSTS_TRACE): main() calls it when asked, so the
 * files can be checked without driving a lap. */
void loadCarExhausts();

}

#endif /* !NFS3HP_NATIVE_EXHAUSTS_H_ */
