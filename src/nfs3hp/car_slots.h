#ifndef NFS3HP_CAR_SLOTS_H_
#define NFS3HP_CAR_SLOTS_H_

#include <x86.h>

/* How many cars the game can hold, by the place a car has in the list of
 * loaded cars -- the order the game scans `carmodel` in (tools/apply_car_slots.py).
 *
 * The game itself keeps this table in its data segment, 50 pointers from
 * 0x5f3c00 (sub_439a50 rejects an index past 0x31), and the car count sits at
 * 0x5f3cc8 immediately after it, so it could not simply be made longer.  The
 * port keeps it instead: kCarSlots pointers, in host memory, reached through
 * carSlotRecord() and setCarSlotRecord(), which the patched sub_439a50 calls.
 * Nothing else reads it.
 *
 * 512, twice the id range, because the game asks this same table with two
 * different numbers: a car's place in the list (0 to the number of cars) and
 * traffic body ids, which run to 0x1f3.  The tables the scan fills
 * (car_arrays.h) are sized for the second of those, so that both fit, and a
 * car's id may be any of the 256 values a byte has. */
namespace nfs3hp
{

constexpr x86::reg32 kCarSlots = 512;

/* The record for car `index`, 0 when it has not been loaded yet. */
x86::reg32 carSlotRecord(x86::reg32 index);

/* `record` kept for car `index`. */
void setCarSlotRecord(x86::reg32 index, x86::reg32 record);

}

#endif /* !NFS3HP_CAR_SLOTS_H_ */
