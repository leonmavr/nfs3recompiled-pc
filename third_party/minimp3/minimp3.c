/* minimp3's implementation, for the radio (src/nfs3hp/native_radio.cpp): MPEG
 * audio Layer III only -- the rest of MPEG 1 and 2 audio is never in an .mp3 --
 * with its SIMD where the compiler's target has it (SSE2 on x86, NEON on
 * arm64) and plain C elsewhere. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"
