#include <nfs3hp.h>
#include "native_guest.h"
#include <SDL3/SDL.h>
#include <cmath>

/* Port: crashes you feel (NFS_CRASH_SHAKE, NFS_CRASH_SOUND).
 *
 * How hard a car was hit is the game's own number: sub_42ac40, each car's
 * collision effects, finds it at [car+0x168] (m/s, the speed lost into what
 * it hit) with what it hit at [car+0x170] -- 0x4xxxx a wall or barrier, its
 * material in the low byte; 0x6xxxx a track object; the rest another car.
 * collisionEffects (native_effects.cpp, hooked at sub_42ac40's top) hands
 * every car's to crashHit.
 *
 * The shake.  For the first player's car each hit over kMinHit sets a shake
 * of the chase camera (cameraToView, native_effects.cpp): a jolt -- the
 * camera knocked aside and up, dying away in a few tenths of a second -- and
 * a vibration -- a fast small buzz under it, dying faster -- both as strong
 * as the hit (full at kFullHit), a wall's at full strength, another car's at
 * kCarShare, a sign's or a tree's at kObjectShare.  Scraping along a wall
 * keeps a light buzz going by the speed.  A hard landing (the car back on
 * the ground, [car+0x164] 0, after kMinAir ticks in the air, falling at
 * kLandMin m/s or more) jolts it down.  By the race's clock, so a pause or a
 * replay holds it still.  NFS_CRASH_SHAKE: 2, up to 3 for more, 0/off none.
 *
 * The sound.  The game's sounds are started by sub_4182b0: the bank's
 * handle in eax, the patch in edx, the volume (0 to 127, times the player's
 * effects volume) in ecx, the direction in ebx, one more byte as its argument;
 * it fills SND's play block (sub_4e9b20's defaults: +5 the note, 60; +8 the
 * volume) and plays it.  A crash is bank 2's patch 46 and a scrape along a
 * wall its patch 41, and the game starts its crash with a volume of 0, the 3D
 * sound's to set -- quiet for all but the biggest hits.  So for the first
 * player's car each hit over kMinHit also plays the crash itself, in front of
 * the player, as loud as the hit (kSound.quiet at kMinHit to kSound.loud at
 * kFullHit) -- a wall's at full strength, another car's and an object's at
 * their shares -- and every crash (the game's and the port's) is pitched by
 * the latest hit: a few semitones lower for a big one, higher for a tap; a
 * scrape a few higher the faster it scrapes (crashNote, in sub_4182b0 just
 * after the block's defaults are filled).
 * NFS_CRASH_SOUND: 1, 0/off leaves the game's. */
namespace nfs3hp
{

namespace
{

// The game's.
constexpr x86::reg32 kPlayerCar = 0x5efa48;
constexpr x86::reg32 kRaceClock = 0x7d3684;     // 64 ticks a second
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;   // 1: two players
constexpr x86::reg32 kCarAir = 0x164;           // word: ticks in the air
constexpr x86::reg32 kCarHit = 0x168;           // float: the hit, m/s
constexpr x86::reg32 kCarTouch = 0x170;         // what it touches
constexpr x86::reg32 kCarVelocity = 0xa4;
constexpr x86::reg32 kCarSpeed = 0xb8;          // float, m/s: the player's cars
constexpr x86::reg32 kTouchKind = 0xefff0000u, kTouchWall = 0x40000u, kTouchObject = 0x60000u;
constexpr x86::reg32 kCrashBank = 2, kCrashPatch = 46, kScrapePatch = 41;

// The port's.
struct Shake
{
    float minHit = 3.0f, fullHit = 20.0f;   // m/s
    float jolt = 0.22f;                     // m the camera is knocked at a full hit
    float buzz = 0.035f;                    // m of vibration at a full hit
    float joltDecay = 7.0f, buzzDecay = 10.0f;   // a second
    float buzzHz = 23.0f;
    float carShare = 0.7f, objectShare = 0.45f;
    float scrapeBuzz = 0.012f, scrapeFull = 40.0f;   // m at scrapeFull m/s along a wall
    float landMin = 4.0f, landFull = 14.0f;          // m/s falling
    x86::reg32 minAir = 8;                           // ticks
};
constexpr Shake kShake{};

struct Sound
{
    float quiet = 35.0f, loud = 127.0f;     // the port's crash's volume at minHit and at fullHit
    int note = 60;                          // SND's own: the patch as recorded
    int lowNote = -4, highNote = 3;         // semitones at a full hit and at a tap
    int scrapeNote = 5;                     // semitones up at scrapeFull m/s
    x86::sreg32 gap = 8;                    // ticks between two of the port's crashes
};
constexpr x86::reg32 kPlaySound = 0x4182b0;
constexpr x86::reg32 kPlayBlock = 0x14, kBlockNote = 5;   // the block at ebp-0x14 in sub_4182b0
constexpr Sound kSound{};

float settingOf(const char* name, float otherwise, float most)
{
    const char* v = SDL_getenv(name);
    if (!v || !*v)
        return otherwise;
    if (!SDL_strcasecmp(v, "off"))
        return 0.0f;
    if (!SDL_strcasecmp(v, "on"))
        return otherwise;
    return SDL_max(0.0f, SDL_min(float(SDL_atof(v)), most));
}

float shakeSetting()
{
    static const float s = settingOf("NFS_CRASH_SHAKE", 2.0f, 3.0f);
    return s;
}

bool soundOn()
{
    static const bool on = settingOf("NFS_CRASH_SOUND", 1.0f, 1.0f) > 0.0f;
    return on;
}

bool tracing()
{
    static const bool t = SDL_getenv("NFS_CRASH_TRACE") != nullptr;
    return t;
}

float share(float hit) { return SDL_max(0.0f, SDL_min(1.0f, (hit - kShake.minHit) / (kShake.fullHit - kShake.minHit))); }

struct State
{
    float jolt = 0.0f, buzz = 0.0f;   // m, now
    float dir[2] = { 0.0f, 0.0f };    // the jolt's way: right, up (camera)
    float scrape = 0.0f;              // m/s of the latest scrape along a wall
    x86::sreg32 scrapeSeen = -1000;
    x86::sreg32 last = -1;
    x86::sreg32 air = 0;
    float falling = 0.0f;
    float lastHit = 0.0f;             // the latest hit of any car, for the sound
    x86::sreg32 lastHitAt = -1000;
    x86::sreg32 played = -1000;       // the port's latest crash
};
State s;

void jolt(float amount, float right, float up)
{
    amount *= shakeSetting();
    if (amount <= s.jolt)
        return;
    s.jolt = amount;
    s.buzz = SDL_max(s.buzz, amount * (kShake.buzz / kShake.jolt));
    const float len = SDL_max(1e-3f, std::sqrt(right * right + up * up));
    s.dir[0] = right / len;
    s.dir[1] = up / len;
}

}  // namespace

/* Every car's hit, from collisionEffects.  False when the game stopped in
 * the port's crash sound. */
bool crashHit(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 car)
{
    const float hit = app->getMemory<float>(car + kCarHit);
    const x86::reg32 touch = app->getMemory<x86::reg32>(car + kCarTouch);
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    if (hit > 0.0f)
    {
        s.lastHit = hit;
        s.lastHitAt = clock;
    }
    if (car != app->getMemory<x86::reg32>(kPlayerCar) || app->getMemory<x86::reg32>(kSplitScreen) == 1)
        return true;
    const x86::reg32 kind = touch & kTouchKind;
    if (kind == kTouchWall)
    {
        s.scrape = std::fabs(app->getMemory<float>(car + kCarSpeed));
        s.scrapeSeen = clock;
    }
    if (!(hit > kShake.minHit))
        return true;
    const float part = kind == kTouchWall ? 1.0f : kind == kTouchObject ? kShake.objectShare : kShake.carShare;
    // Knocked aside one way or the other, and a little up.
    const float side = (clock & 1) ? 1.0f : -1.0f;
    jolt(kShake.jolt * share(hit) * part, side, 0.5f);
    if (tracing())
        SDL_Log("[CRASH] t %d hit %.1f touch %x: jolt %.3f m", int(clock), hit, unsigned(touch), s.jolt);
    if (!soundOn() || clock - s.played < kSound.gap)
        return true;
    s.played = clock;
    // The crash, in front of the player, as loud as the hit.
    const float loudness = kSound.quiet + (kSound.loud - kSound.quiet) * std::sqrt(share(hit) * part);
    const x86::reg32 eax = cpu.eax, ebx = cpu.ebx, ecx = cpu.ecx, edx = cpu.edx, esi = cpu.esi, edi = cpu.edi;
    cpu.eax = kCrashBank;
    cpu.edx = kCrashPatch;
    cpu.ecx = x86::reg32(SDL_min(127, int(loudness + 0.5f)));
    cpu.ebx = 0;
    if (tracing())
        SDL_Log("[CRASH] t %d the port's crash at volume %u", int(clock), unsigned(cpu.ecx));
    if (!guest::call(app, cpu, kPlaySound, { 0x40 }))
        return false;
    cpu.eax = eax;
    cpu.ebx = ebx;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

/* Each frame: the shake dies away; a hard landing jolts it. */
void crashFrame(win32::WinApplication* app)
{
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const x86::sreg32 ticks = s.last < 0 || clock < s.last ? 0 : SDL_min(clock - s.last, x86::sreg32(16));
    if (s.last < 0 || clock < s.last)
        s = State{};
    s.last = clock;
    const float dt = float(ticks) / 64.0f;
    s.jolt *= std::exp(-kShake.joltDecay * dt);
    s.buzz *= std::exp(-kShake.buzzDecay * dt);
    if (clock - s.scrapeSeen < 4 && shakeSetting() > 0.0f)
        s.buzz = SDL_max(s.buzz, kShake.scrapeBuzz * SDL_min(1.0f, s.scrape / kShake.scrapeFull) * shakeSetting());
    const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCar);
    if (!car || ticks == 0)
        return;
    const x86::sreg32 air = app->getMemory<uint16_t>(car + kCarAir);
    if (air > 0)
    {
        s.falling = -app->getMemory<float>(car + kCarVelocity + 4);
        s.air = air;
        return;
    }
    if (s.air >= x86::sreg32(kShake.minAir) && s.falling > kShake.landMin)
    {
        const float part = SDL_min(1.0f, (s.falling - kShake.landMin) / (kShake.landFull - kShake.landMin));
        jolt(kShake.jolt * (0.3f + 0.7f * part), 0.0f, -1.0f);
        if (tracing())
            SDL_Log("[CRASH] t %d landed at %.1f m/s: jolt %.3f m", int(clock), s.falling, s.jolt);
    }
    s.air = 0;
    s.falling = 0.0f;
}

/* From cameraToView: the chase camera's place moved by the shake, along its
 * own right ([axes+0]) and up ([axes+3]). */
void crashShake(win32::WinApplication* app, float place[3], const float right[3], const float up[3])
{
    if (s.jolt < 1e-4f && s.buzz < 1e-4f)
        return;
    const float t = float(app->getMemory<x86::reg32>(kRaceClock)) / 64.0f;
    const float w = 2.0f * float(M_PI) * kShake.buzzHz;
    // The jolt swings back and forth once or twice as it dies; the buzz is fast.
    const float swing = s.jolt * std::cos(t * 2.0f * float(M_PI) * 3.0f);
    const float bx = s.buzz * std::sin(t * w), by = s.buzz * std::sin(t * w * 1.37f + 1.0f);
    const float alongRight = swing * s.dir[0] + bx, alongUp = swing * s.dir[1] + by;
    for (int k = 0; k < 3; ++k)
        place[k] += right[k] * alongRight + up[k] * alongUp;
}

/* In sub_4182b0 just after the play block's defaults (0x4182dd; the patch
 * in esi, the bank's handle at [ebp-4], the block at ebp-0x14): a crash's note
 * by the latest hit, a scrape's by its speed. */
void crashNote(win32::WinApplication* app, x86::reg32 patch, x86::reg32 frame)
{
    if (!soundOn() || app->getMemory<x86::reg32>(frame - 4) != kCrashBank)
        return;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    int shift = 0;
    if (patch == kCrashPatch && clock - s.lastHitAt < 8)
    {
        const float part = share(s.lastHit);
        shift = int(std::lround(float(kSound.highNote) + float(kSound.lowNote - kSound.highNote) * part));
    }
    else if (patch == kScrapePatch && clock - s.scrapeSeen < 8)
        shift = int(std::lround(float(kSound.scrapeNote) * SDL_min(1.0f, s.scrape / kShake.scrapeFull)));
    else
        return;
    app->getMemory<x86::reg8>(frame - kPlayBlock + kBlockNote) = x86::reg8(kSound.note + shift);
    if (tracing())
        SDL_Log("[CRASH] t %d patch %u note %+d", int(clock), unsigned(patch), shift);
}

}  // namespace nfs3hp
