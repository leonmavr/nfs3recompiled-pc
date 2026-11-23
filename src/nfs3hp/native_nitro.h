#ifndef NFS3HP_NATIVE_NITRO_H_
#define NFS3HP_NATIVE_NITRO_H_

#include <x86.h>

namespace win32
{
class WinApplication;
}

/* Port: nitro (native_nitro.cpp, NFS_NITRO): the first player's bottle -- how
 * full it is, the key that lights it, and the push it gives the car while it
 * burns.  The gauge that shows it and the feel it adds read it from here. */
namespace nfs3hp
{

/* Once a frame, on the race's clock: lit while the key is held and there is
 * something in the bottle, the car pushed along its own way. */
void nitroFrame(win32::WinApplication* app, int ticks);

/* The first player's bottle, 0 (empty) to 1 (full). */
float nitroCharge();

/* How hard it burns, 0 to 1, eased: for the gauge and the feel. */
float nitroBurning();

/* The key that lights it, in the game's own shape for a binding: a character,
 * a scan code and the device (4 is the keyboard). */
x86::reg32 nitroKey();
void setNitroKey(x86::reg32 code);

}  // namespace nfs3hp

#endif /* !NFS3HP_NATIVE_NITRO_H_ */
