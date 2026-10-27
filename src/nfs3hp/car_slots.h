#ifndef NFS3HP_CAR_SLOTS_H_
#define NFS3HP_CAR_SLOTS_H_

#include <x86.h>

/* How many cars the game can hold, by the id a car carries in its own data --
 * one byte at offset 0x18 of its fedata.eng, which is what the duplicate check
 * compares and what indexes this table (tools/apply_car_slots.py).
 *
 * The game itself keeps this table in its data segment, 50 pointers from
 * 0x5f3c00 (sub_439a50 rejects an index past 0x31), and the car count sits at
 * 0x5f3cc8 immediately after it, so it could not simply be made longer.  The
 * port keeps it instead: 256 pointers, in host memory, reached through
 * carSlotRecord() and setCarSlotRecord(), which the patched sub_439a50 calls.
 * Nothing else reads it.
 *
 * 256 is the whole of the range the id is: one byte.  A car's id must be
 * distinct and within 0..255. */
namespace nfs3hp
{

constexpr x86::reg32 kCarSlots = 256;

/* The record for car `index`, 0 when it has not been loaded yet. */
x86::reg32 carSlotRecord(x86::reg32 index);

/* `record` kept for car `index`. */
void setCarSlotRecord(x86::reg32 index, x86::reg32 record);

}

#endif /* !NFS3HP_CAR_SLOTS_H_ */
