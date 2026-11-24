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
 * Earned back.  Three ways, each at a rate of its own, NFS_NITRO_FILL times them
 * all (1 by default; 0 or `off` for none):
 *  - the slipstream: kSlipSeconds of one at full strength fills an empty bottle
 *    (slipstreamOf, native_surface.cpp, the same number that pushes the car);
 *  - a drift: the tyres dragged sideways, kDriftSeconds of one at full scrub --
 *    the sideways speed the tyre smoke and marks come of (native_particles.cpp,
 *    the same 1.2 m/s up to 4.2), which is the sliding of a turn, brakes or no
 *    brakes;
 *  - a jump: kAirSeconds of one per second off the ground, once it has been off
 *    for kLeastAir ticks -- the same air the crash shake calls a landing
 *    ([car+0x164], native_crash.cpp).
 * Earning and burning at once is allowed and nets out: a bottle burns in
 * kSeconds, which is quicker than any of the three fills it.
 *
 * NFS_NITRO_TEST keeps it full, so a headless run can watch the push, and
 * NFS_NITRO_TRACE says what it is doing once a second -- its charge, what the
 * second earned and of what: `slip`, `sideways` and `air` -- and as it lights
 * and goes out. */
namespace nfs3hp
{

bool raceIsRunning();                                            // nfs3hp_main.cpp
float slipstreamOf(win32::WinApplication* app, x86::reg32 car);   // native_surface.cpp

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
constexpr x86::reg32 kCarRight = 0xc0;
constexpr x86::reg32 kCarForward = 0xd8;
constexpr x86::reg32 kCarAir = 0x164;            // word: ticks off the ground
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
// What fills it, in seconds of each to fill an empty bottle (NFS_NITRO_FILL
// scales all three).
constexpr float kSlipSeconds = 8.0f;             // tucked in behind another car
constexpr float kDriftSeconds = 15.0f;           // tyres dragged sideways
constexpr float kAirSeconds = 3.5f;              // off the ground, a second each 3.5
constexpr float kDriftFrom = 1.2f;               // m/s sideways: the tyre smoke's own thresholds
constexpr float kDriftFull = 4.2f;               // (native_particles.cpp's scrub)
constexpr float kDriftLeastSpeed = 6.0f;         // m/s: slower than this is not a drift
constexpr x86::sreg32 kLeastAir = 8;             // ticks off the ground before a jump counts
// The view's pull-back when the bottle lights (the projection's tangents, times
// the speed's own widening): a kick to kKickPull, eased out over kKickIn and
// back to kHoldPull over kKickOut, which it holds while it burns.
constexpr float kKickPull = 1.3f;
constexpr float kHoldPull = 1.1f;
constexpr float kKickIn = 0.2f;                  // s: eased out to the kick
constexpr float kKickOut = 0.25f;                // s: and eased back to the held pull
constexpr float kEase = 12.0f;                   // 1/s: how fast burning eases to on or off
constexpr float kOut = 0.005f;                   // under this on the way out, it is out
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

/* NFS_NITRO_FILL: how quickly the race fills the bottle, a multiplier on the
 * three ways of earning it (1 by default, up to 5; 0 or `off` for none). */
float fillRate()
{
    static const float rate = [] {
        const char* text = SDL_getenv("NFS_NITRO_FILL");
        if (!text || !*text)
            return 1.0f;
        if (!SDL_strcasecmp(text, "off"))
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed >= 0.0f && parsed <= 5.0f ? parsed : 1.0f;
    }();
    return rate;
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
    float earned = 0.0f;        // what the race has put in since that, for the trace
    float pull = 1.0f;          // the view's pull-back now, kKickPull down to 1
    float sinceLit = 0.0f;      // s since it last lit: the kick's own clock
    unsigned ignitions = 0;     // how many times it has lit: the sound's cue
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

float nitroPull()
{
    return s_state.pull;
}

unsigned nitroIgnitions()
{
    return s_state.ignitions;
}

float nitroPush()
{
    using namespace nitro;
    /* NFS_NITRO, 4 by default -- four times what the push first was, which a
     * car could hardly feel -- up to twice that again. */
    const float setting = settingOf("NFS_NITRO", 4.0f, 8.0f);
    return settingOf("NFS_NITRO_PUSH", kPush, kPushMost) * setting;
}

float nitroTaper()
{
    return nitro::kTaper;
}

float nitroSeconds()
{
    return settingOf("NFS_NITRO_SECONDS", nitro::kSeconds, nitro::kMostSeconds);
}

void nitroFrame(win32::WinApplication* app, int ticks)
{
    using namespace nitro;
    const float setting = settingOf("NFS_NITRO", 4.0f, 8.0f);
    const float push = nitroPush();
    const float seconds = nitroSeconds();
    const char* trace = SDL_getenv("NFS_NITRO_TRACE");

    const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCar);
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    bool test = SDL_getenv("NFS_NITRO_TEST") != nullptr;
    if (test)
    {
        /* Test only: NFS_NITRO_TEST_SECONDS burn for that long and no more, so
         * that letting go of it can be watched with no hands on the keys. */
        static const float seconds = [] {
            const char* text = SDL_getenv("NFS_NITRO_TEST_SECONDS");
            return text && *text ? float(SDL_atof(text)) : 0.0f;
        }();
        static x86::sreg32 s_first = -1;
        if (seconds > 0.0f && clock > kCountdownTo)
        {
            if (s_first < 0 || clock < s_first)
                s_first = clock;
            test = float(clock - s_first) / 64.0f < seconds;
        }
    }
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
    {
        s_state.lit = true;
        ++s_state.ignitions;
    }
    if (!keyHeld || !allowed)
        s_state.lit = false;

    /* What the race has earned it: the slipstream, a drift, a jump.  On the
     * race's clock with the rest, so a pause earns nothing. */
    if (ticks > 0)
    {
        float gain = 0.0f;
        const float slip = slipstreamOf(app, car);
        if (slip > 0.0f)
            gain += slip * dt / kSlipSeconds;
        const bool grounded = app->getMemory<uint16_t>(car + kCarAir) == 0;
        if (!grounded)
        {
            if (app->getMemory<uint16_t>(car + kCarAir) >= uint16_t(kLeastAir))
                gain += dt / kAirSeconds;
        }
        else if (speed > kDriftLeastSpeed)
        {
            const float right[3] = { valueAt(app, car + kCarRight), valueAt(app, car + kCarRight + 4),
                                     valueAt(app, car + kCarRight + 8) };
            const float v[3] = { valueAt(app, car + kCarVelocity), valueAt(app, car + kCarVelocity + 4),
                                 valueAt(app, car + kCarVelocity + 8) };
            const float sideways = SDL_fabsf(v[0] * right[0] + v[1] * right[1] + v[2] * right[2]);
            const float drift = SDL_clamp((sideways - kDriftFrom) / (kDriftFull - kDriftFrom), 0.0f, 1.0f);
            if (drift > 0.0f)
                gain += drift * dt / kDriftSeconds;
        }
        if (gain > 0.0f)
        {
            gain *= fillRate();
            s_state.charge = SDL_min(1.0f, s_state.charge + gain);
            s_state.earned += gain;
        }
    }

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
    /* Easing out is exponential, so it only ever approaches nought: whatever
     * reads "is it burning" would go on reading yes for ever.  Out is out. */
    if (!s_state.lit && s_state.burning < kOut)
        s_state.burning = 0.0f;

    /* The light: the view kicked back as it lights, then held a little wider
     * while it burns, and back to none after -- eased on the race's clock.  The
     * kick is a hump of its own, out over a fifth of a second and back over a
     * quarter -- the whole of it eased in and out, rather than the one frame the
     * first of it was, which was no more than a flicker. */
    s_state.sinceLit = (s_state.lit && !s_state.wasLit) ? 0.0f : s_state.sinceLit + dt;
    const float up = SDL_max(0.0f, SDL_min(1.0f, s_state.sinceLit / kKickIn));
    const float down = SDL_max(0.0f, SDL_min(1.0f, (s_state.sinceLit - kKickIn) / kKickOut));
    const float kick = up * up * (3.0f - 2.0f * up) * (1.0f - down * down * (3.0f - 2.0f * down));
    s_state.pull = 1.0f + (kHoldPull - 1.0f) * s_state.burning + (kKickPull - kHoldPull) * kick;

    if (trace && s_state.lit != s_state.wasLit)
        SDL_Log("[NITRO] t %d %s, charge %.2f speed %.1f, the view pulled to %.2f", int(clock),
                s_state.lit ? "lit" : "out", s_state.charge, speed, s_state.pull);
    s_state.wasLit = s_state.lit;

    if (trace && (clock / 64 != s_state.traceAt / 64 || s_state.traceAt == 0))
    {
        s_state.traceAt = clock;
        const float sideways = car ? SDL_fabsf(valueAt(app, car + kCarVelocity) * valueAt(app, car + kCarRight)
                                               + valueAt(app, car + kCarVelocity + 4) * valueAt(app, car + kCarRight + 4)
                                               + valueAt(app, car + kCarVelocity + 8) * valueAt(app, car + kCarRight + 8))
                                   : 0.0f;
        SDL_Log("[NITRO] t %d charge %.2f burning %.2f speed %.1f %.1f m/s2 key %d slip %.2f sideways %.1f air %u"
                " earned %.3f pull %.2f",
                int(clock), s_state.charge, s_state.burning, speed, push * SDL_clamp(1.0f - speed / kTaper, 0.0f, 1.0f),
                int(keyDown), car ? slipstreamOf(app, car) : 0.0f, sideways,
                car ? unsigned(app->getMemory<uint16_t>(car + kCarAir)) : 0u, s_state.earned, s_state.pull);
        s_state.earned = 0.0f;
    }
}

}  // namespace nfs3hp
