#ifndef NFS3HP_CAR_ARRAYS_H_
#define NFS3HP_CAR_ARRAYS_H_

#include <x86.h>
#include "car_slots.h"

/* The per-car tables, out of the game's data segment.
 *
 * As the game scans `carmodel` it keeps one entry per car, in load order, in
 * six arrays that sit among its globals at 0x554fa8-0x5552d7: two 16-bit
 * arrays at 0x554fa8 and 0x55500a, two more at 0x555138 and 0x5551a2, and two
 * 32-bit ones at 0x55506c and 0x555214.  Each was sized for fifty cars -- 49
 * entries and the terminator the scan writes one past the last car -- and each
 * is packed hard against the next, so the fifty-first car wrote into the array
 * after its own, and past the last one (0x5552d8 onwards) is live data.
 *
 * The port gives them a home of their own instead: a section of memory added
 * to the machine's map (nfs3hp.cpp, and MemMap) holding nothing but these
 * tables, kCarArraySlots entries each -- every car the list can hold, and the
 * terminator.  Every access in the generated code is `base + index * element`,
 * so tools/apply_car_arrays.py only had to rename the base; the code that
 * fills and walks them is the game's own.
 *
 * Array A : 0x554fa8, 16-bit      Array D : 0x5551a2, 16-bit
 * Array B : 0x55500a, 16-bit      Array E : 0x55506c, 32-bit
 * Array C : 0x555138, 16-bit      Array F : 0x555214, 32-bit
 */
namespace nfs3hp
{

/* Every car the list can hold, and the terminator after the last of them. */
constexpr x86::reg32 kCarArraySlots = kCarSlots + 4;

constexpr x86::reg32 kCarArrayA = 0xA98000; /* kCarArraySlots x 2 bytes */
constexpr x86::reg32 kCarArrayB = 0xA98440; /* kCarArraySlots x 2 bytes */
constexpr x86::reg32 kCarArrayC = 0xA98880; /* kCarArraySlots x 2 bytes */
constexpr x86::reg32 kCarArrayD = 0xA98CC0; /* kCarArraySlots x 2 bytes */
constexpr x86::reg32 kCarArrayE = 0xA99100; /* kCarArraySlots x 4 bytes */
constexpr x86::reg32 kCarArrayF = 0xA99940; /* kCarArraySlots x 4 bytes */

/* The lists the game sorts to build its car menus: four of them, at 0x5552d8,
 * 0x5553a0, 0x555468 and 0x555530, a group each, 200 bytes apart -- fifty
 * records per group, which is the last of the fifty-car limits and the one the
 * fifty-first car crashed in: past the fourth group's fifty sits the counters
 * array, and past that, live globals.  Same treatment.  How many each holds
 * stays where the game keeps it, at 0x5555f8.  tools/apply_car_groups.py moves
 * them, and the two places that work out a group's address from 200 a group.
 */
constexpr x86::reg32 kCarGroupSlots  = kCarArraySlots;
constexpr x86::reg32 kCarGroupStride = kCarGroupSlots * 4;
constexpr x86::reg32 kCarGroups      = 4;
constexpr x86::reg32 kCarGroupTable  = 0xA9A800;
constexpr x86::reg32 kCarGroupTableSize = kCarGroups * kCarGroupStride;

/* The section all of it lives in, added to the map in nfs3hp.cpp.  It ends
 * past the last of the group lists. */
constexpr x86::reg32 kCarArrayBase = kCarArrayA;
constexpr x86::reg32 kCarArraySize = 0x8000;

}

#endif /* !NFS3HP_CAR_ARRAYS_H_ */
