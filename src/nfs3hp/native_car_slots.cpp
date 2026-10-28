/* The car table, out of the game's data segment (car_slots.h).
 *
 * sub_439a50 is the game's "the record for car <id>", and it is also the only
 * thing that ever fills the table: a slot holds the record once it has been
 * found, and a missing one is looked up by walking the loaded cars in order.
 * The table itself is 50 pointers at 0x5f3c00, and the car count is kept at
 * 0x5f3cc8, the next dword -- so a longer table in place would have run into
 * it.  These two hold it instead: same pointers, same order, kCarSlots of
 * them, and tools/apply_car_slots.py has sub_439a50 call them where it used to
 * read and write guest memory.  Nothing else in the game touches the table.
 *
 * Kept as guest addresses -- they are values, not host pointers, and the code
 * that walks them is the game's. */
#include "car_slots.h"

namespace nfs3hp
{

namespace
{
x86::reg32 s_records[kCarSlots] = {};
}

x86::reg32 carSlotRecord(x86::reg32 index)
{
    return index < kCarSlots ? s_records[index] : 0;
}

void setCarSlotRecord(x86::reg32 index, x86::reg32 record)
{
    if (index < kCarSlots)
        s_records[index] = record;
}

}
