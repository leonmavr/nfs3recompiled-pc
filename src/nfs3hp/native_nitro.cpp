#include <nfs3hp.h>
#include "native_nitro.h"
#include "native_settings.h"
#include <SDL3/SDL.h>
#include <cmath>

/* Port: nitro (NFS_NITRO, 1; a number up to 3 for more, 0 or off for none).
 *
 * The first player's bottle, lit by a key of its own (NFS_NITRO_KEY, left shift
 * by default; Controls lists it as Nitro, native_frontend_controls.cpp).  Held
 * down, it burns for kSeconds and pushes the car along its own way -- the way
 * the slipstream pushes one (native_surface.cpp's `slipstream`): a share of the
 * velocity, +0xa4, +0xa8 and +0xac, added each tick, the vertical one left
 * alone so a crest does not throw the car.  kPush m/s2 at a standstill, and
 * less the faster the car goes, gone by kTaper m/s (324 km/h): what the engine,
 * the gears and the air do then is untouched, so the push is felt low down and
 * the car keeps its own top speed at the top.  NFS_NITRO_PUSH is that kPush,
 * NFS_NITRO_SECONDS how long a full bottle burns.
 *
 * Nothing burns in a split screen (one bottle, one key: the second player's is
 * the game's to give), nor while the start's countdown shows -- the game draws
 * it until the race's clock reaches 0x220 (sub_482620), which is where the cars
 * are let go -- so a car held on the line is not pushed out from under it.  A
 * standstill is where it is worth the most, and it is meant to be lit there.
 *
 * The bottle starts a race full and empties as it burns; how it is earned back
 * (slipstreaming, air time, drifting) is the next step of the milestone
 * (TODOs.md).  NFS_NITRO_TEST keeps it full, so a headless run can watch the
 * push, and NFS_NITRO_TRACE says what it is doing once a second, and as it
 * lights and goes out. */
namespace nfs3hp
{

bool raceIsRunning();                       // nfs3hp_main.cpp

namespace
{

namespace nitro
{
// The game's.
constexpr x86::reg32 kPlayerCar = 0x5efa48;      // the first player's car
constexpr x86::reg32 kRaceClock = 0x7d3684;      // 64 ticks a second
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;    // 1: two players
constexpr x86::reg32 kKeys = 0x564818;           // a byte a scan code: held
constexpr x86::reg32 kCarVelocity = 0xa4;        // floats x, y, z, m/s
constexpr x86::reg32 kCarForward = 0xd8;
constexpr x86::sreg32 kCountdownTo = 0x220;      // sub_482620 draws the start's countdown up to here
constexpr x86::reg32 kKeyboard = 4;              // a binding's low byte: the keyboard's
// The port's.
constexpr const char* kKeySetting = "NFS_NITRO_KEY";
constexpr x86::reg32 kDefaultKey = 0x00002a04;   // left shift: no character, its scan code, the keyboard
constexpr float kPush = 5.0f;                    // m/s2 at a standstill, NFS_NITRO_PUSH
constexpr float kPushMost = 15.0f;               // the most NFS_NITRO_PUSH may ask for
constexpr float kTaper = 90.0f;                  // m/s: the push is gone by here
constexpr float kSeconds = 3.0f;                 // a full bottle burns this long
constexpr float kMostSeconds = 10.0f;            // the most NFS_NITRO_SECONDS may ask for
constexpr float kRelight = 0.15f;                // emptied below this, it cannot be lit again
constexpr float kEase = 12.0f;                   // 1/s: how fast burning eases to on or off
}  // namespace nitro

float settingOf(const char* name, float otherwise, float most)
{
    const char* text = SDL_getenv(name);
    if (!text || !*text)
        return otherwise;
    if (!SDL_strcasecmp(text, "off"))
        return 0.0f;
    if (!SDL_strcasecmp(text, "on"))
        return otherwise;
    const float parsed = float(SDL_atof(text));
    return parsed >= 0.0f && parsed <= most ? parsed : otherwise;
}

struct State
{
    x86::reg32 car = 0;         // the car it belongs to: a new one fills the bottle
    x86::sreg32 lastClock = -1; // the race's clock as last seen: it going back is a new race
    float charge = 1.0f;        // 0 to 1
    float burning = 0.0f;       // 0 to 1, eased
    bool lit = false;           // burning this tick
    bool wasLit = false;
    x86::sreg32 traceAt = 0;    // the race tick it last said anything at
};
State s_state;

x86::reg32 s_key = 0;
bool s_keyRead = false;

float valueAt(win32::WinApplication* app, x86::reg32 at)
{
    return app->getMemory<float>(at);
}

x86::reg32 heldKey(win32::WinApplication* app, x86::reg32 key)
{
    if ((key & 0xff) != nitro::kKeyboard)
        return 0;
    return app->getMemory<x86::reg8>(nitro::kKeys + ((key >> 8) & 0xff)) != 0 ? 1 : 0;
}

}  // namespace

x86::reg32 nitroKey()
{
    if (!s_keyRead)
    {
        const char* text = SDL_getenv(nitro::kKeySetting);
        s_key = x86::reg32(text && *text ? SDL_strtoul(text, nullptr, 0) : nitro::kDefaultKey);
        s_keyRead = true;
    }
    return s_key;
}

void setNitroKey(x86::reg32 code)
{
    s_key = code;
    s_keyRead = true;
    char text[16];
    SDL_snprintf(text, sizeof text, "0x%08x", unsigned(code));
    settingsSet(nitro::kKeySetting, text);
    settingsSave();
}

float nitroCharge()
{
    return s_state.charge;
}

float nitroBurning()
{
    return s_state.burning;
}

void nitroFrame(win32::WinApplication* app, int ticks)
{
    using namespace nitro;
    const float setting = settingOf("NFS_NITRO", 1.0f, 3.0f);
    const float push = settingOf("NFS_NITRO_PUSH", kPush, kPushMost) * setting;
    const float seconds = settingOf("NFS_NITRO_SECONDS", kSeconds, kMostSeconds);
    const bool test = SDL_getenv("NFS_NITRO_TEST") != nullptr;
    const char* trace = SDL_getenv("NFS_NITRO_TRACE");

    const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCar);
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    /* A new car, the clock gone back, or the clock still short of where the
     * cars are let go -- a race restarted, or another one started -- fills the
     * bottle again.  All three, because a restart need not raise one of them:
     * it may keep the car, keep the clock's own running, or start afresh. */
    if (car != s_state.car || s_state.lastClock < 0 || clock < s_state.lastClock || clock <= kCountdownTo)
    {
        s_state.car = car;
        s_state.charge = 1.0f;
        s_state.burning = 0.0f;
        s_state.lit = false;
    }
    s_state.lastClock = clock;
    const float dt = ticks > 0 ? float(ticks) / 64.0f : 0.0f;
    float speed = 0.0f;
    if (car)
    {
        const float v[3] = { valueAt(app, car + kCarVelocity), valueAt(app, car + kCarVelocity + 4),
                             valueAt(app, car + kCarVelocity + 8) };
        speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    /* Whether it may burn at all this tick.  A pause is none of them: `ticks`
     * is 0 then, so the clock does not move and the bottle neither burns nor
     * fills.  NFS_NITRO_TEST skips the key, the countdown and the car's own
     * state, so a run with no hands can watch the push. */
    const bool usable = car && ticks > 0 && setting > 0.0f && raceIsRunning()
                        && app->getMemory<x86::reg32>(kSplitScreen) != 1;
    const bool allowed = usable && (test || clock > kCountdownTo);
    const bool keyDown = heldKey(app, nitroKey()) != 0;
    const bool keyHeld = allowed && (test || keyDown);
    if (!s_state.lit && keyHeld && (s_state.charge > kRelight || test))
        s_state.lit = true;
    if (!keyHeld || !allowed)
        s_state.lit = false;

    if (s_state.lit)
    {
        if (!test)
            s_state.charge = SDL_max(0.0f, s_state.charge - dt / SDL_max(seconds, 0.1f));
        const float taper = SDL_clamp(1.0f - speed / kTaper, 0.0f, 1.0f);
        const float step = push * taper * dt;
        const float f[3] = { valueAt(app, car + kCarForward), valueAt(app, car + kCarForward + 4),
                             valueAt(app, car + kCarForward + 8) };
        for (int k = 0; k < 3; k += 2)   // x and z: never the vertical
        {
            const x86::reg32 at = car + kCarVelocity + 4 * x86::reg32(k);
            app->getMemory<float>(at) = valueAt(app, at) + f[k] * step;
        }
        if (!test && s_state.charge <= 0.0f)
            s_state.lit = false;
    }

    s_state.burning += ((s_state.lit ? 1.0f : 0.0f) - s_state.burning) * SDL_min(1.0f, dt * kEase);

    if (trace && s_state.lit != s_state.wasLit)
        SDL_Log("[NITRO] t %d %s, charge %.2f speed %.1f", int(clock), s_state.lit ? "lit" : "out",
                s_state.charge, speed);
    s_state.wasLit = s_state.lit;

    if (trace && (clock / 64 != s_state.traceAt / 64 || s_state.traceAt == 0))
    {
        s_state.traceAt = clock;
        SDL_Log("[NITRO] t %d charge %.2f burning %.2f speed %.1f %.1f m/s2 key %d", int(clock), s_state.charge,
                s_state.burning, speed, push * SDL_clamp(1.0f - speed / kTaper, 0.0f, 1.0f), int(keyDown));
    }
}

}  // namespace nfs3hp
