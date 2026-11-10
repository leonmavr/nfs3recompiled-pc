#include "native_ai.h"
#include <nfs3hp.h>
#include <SDL3/SDL.h>
#include <cstring>

/* The opponents' driving, decompiled (native_ai.h), entered from the top of
 * the generated sub_4064f0 and sub_4070c0 (tools/apply_native_ai.py).  They
 * come out as the generated code does to the bit, in every precision the race
 * runs at (tools/native_ai_checks.py), so they change nothing the game does:
 * they are the two functions written to be read -- and to be changed, the
 * opponents' pull and steering being here as C++.  NFS_NATIVE_AI=0 leaves the
 * generated code to run instead.  sub_408600, an opponent's every frame, is
 * entered here first too (aiDrive), and the port's changes to how the
 * opponents drive -- below, NFS_AI_TUNE -- are made around these, never in
 * them: native_ai.h stays the game's to the bit. */
namespace nfs3hp
{

namespace
{

bool aiNativesOn()
{
    static const bool on = []() {
        const char* value = SDL_getenv("NFS_NATIVE_AI");
        return !(value && SDL_strcmp(value, "0") == 0);
    }();
    return on;
}

/* Port: the opponents' driving, made a little better (NFS_AI_TUNE=0 for the
 * game's own).  What the original does, read from the generated code:
 *
 * - Every frame sub_4088f0 sends a racer to sub_408600, which picks how it
 *   drives: sub_4076b0 at speed, sub_407c30 slowly or in reverse (while
 *   [car+0x7d4] is 1), sub_407fa0 off its line or sideways; sub_4071a0 then
 *   turns the force and torque those leave into velocity (+0xa4) and spin
 *   (+0xe4) -- an opponent is pushed, not driven through its wheels.
 * - It reverses (sub_405fd0: [+0x7d4] 1, [+0x620] bit 0x40 and a time to stop
 *   at in [+0x7d0], [+0x658] counting the tries) only when a collision says so
 *   (sub_406120, sub_406160, sub_42b900, sub_430da0), and a collision while it
 *   reverses with no time set turns it forward again: two cars touching flip
 *   each other forward and back and never part -- the clumps that sat there.
 *   Nose against a wall with nothing touching it at the side, it pushed on.
 * - It is put back on the road (sub_429100) after nine tries at reversing, or
 *   off the road after 1000 frames under 3 m/s ([+0x7d8]) -- half a minute.
 *
 * So the port watches each opponent's progress along the track (its node,
 * [+0x1c]) and, when one has stopped getting anywhere -- slow and stuck at the
 * same node, or facing well away from the road -- has it reverse out for a
 * set time, as the game's own reverse does but with the timer it ignores
 * collisions under; each driver for a different time, so a clump comes apart.
 * Stuck past that, the game's own counters are run up so that it puts the car
 * back on the road at once instead of half a minute later.
 *
 * Each driver -- the racer in its slot, [+0x1f0], one driver to a slot -- has
 * a character of its own (kDrivers): how hard it pulls away (its engine's
 * pull, aiPull), how soon it gives up on a position it is stuck in and how
 * long it backs off.  On top of that every opponent pulls NFS_AI_PACE harder
 * (1.06 by default), the difficulty a notch up.  Network races are left alone:
 * every machine has to drive the opponents alike.  All of it runs off the
 * race's own clock, so a replay drives as the race did. */
constexpr x86::reg32 kCars = 0x5e1120;
constexpr x86::reg32 kCarSize = 0x9ac;
constexpr int kMaxCars = 16;
constexpr x86::reg32 kRaceClock = 0x7d3684;   // the race's ticks, 64 a second
constexpr int kTicksPerSecond = 64;

struct Driver
{
    const char* character;
    float pull;        // the engine's pull against the others'
    float corners;     // the speed it takes the track at against the game's
    float patience;    // seconds stuck before it backs off
    float backOff;     // seconds it reverses for
};

/* By slot.  The game names its seven racers by slot too. */
constexpr Driver kDrivers[] = {
    { "player",      1.00f, 1.000f, 1.6f, 1.2f },
    { "hothead",     1.05f, 1.020f, 1.0f, 0.9f },   // flat out everywhere, quick to retry
    { "smooth",      0.99f, 1.030f, 1.6f, 1.3f },   // gentle on the throttle, quick through corners
    { "late braker", 1.01f, 1.035f, 1.3f, 1.0f },   // carries speed deep into corners
    { "steady",      1.00f, 1.000f, 1.9f, 1.5f },
    { "bully",       1.04f, 1.010f, 0.9f, 0.8f },   // pushes, shoves its way out
    { "cautious",    0.98f, 0.985f, 2.1f, 1.7f },
    { "opportunist", 1.03f, 1.015f, 1.2f, 1.1f },
};

/* The speed it aims for, [+0x7b0], is the track's for that place times the
 * game's factors (sub_409330); each frame, before it drives, the port takes
 * it up by the driver's corners and a part of NFS_AI_PACE -- once for each
 * value the game wrote, so that it never builds up. */

struct Opponent
{
    x86::sreg32 node = 0;        // the furthest track node reached
    x86::sreg32 progressAt = 0;  // the race tick it got there
    x86::sreg32 stuckFrom = -1;  // since when it has been stuck, or -1
    x86::sreg32 lastTick = 0;
    int tries = 0;               // backing off since it last got going
    x86::sreg32 avoidUntil = -1; // till when it keeps to avoidLateral
    float wantWritten = 0.0f;    // the aim [+0x7b0] as the port last left it
    float avoidLateral = 0.0f;   // the line it keeps to, across the road
    float heading = 0.0f;        // the last heading error aiHeading worked out
};
Opponent s_opponents[kMaxCars];

int opponentIndex(x86::reg32 car)
{
    if (car < kCars || (car - kCars) % kCarSize != 0)
        return -1;
    const x86::reg32 i = (car - kCars) / kCarSize;
    return i < kMaxCars ? int(i) : -1;
}

bool aiTracing()
{
    static const bool on = []() {
        const char* value = SDL_getenv("NFS_AI_TRACE");
        return value && *value && SDL_strcmp(value, "0") != 0;
    }();
    return on;
}

bool aiTuned()
{
    static const bool on = []() {
        const char* value = SDL_getenv("NFS_AI_TUNE");
        return !(value && SDL_strcmp(value, "0") == 0);
    }();
    return on;
}

float aiPace()
{
    static const float pace = []() {
        const char* value = SDL_getenv("NFS_AI_PACE");
        const float parsed = value && *value ? float(SDL_atof(value)) : 1.06f;
        return parsed >= 0.5f && parsed <= 2.0f ? parsed : 1.06f;
    }();
    return pace;
}

bool networkRace(win32::WinApplication* app)
{
    return app->getMemory<x86::reg32>(0x6fd3b0) == 2;
}

const Driver& driverOf(win32::WinApplication* app, x86::reg32 car)
{
    const x86::reg32 slot = app->getMemory<x86::reg32>(car + 0x1f0);
    return kDrivers[slot < sizeof kDrivers / sizeof kDrivers[0] ? slot : 0];
}

/* What is around a car, from the cars' positions (+0x130) against its own
 * heading (+0xd8, the forward row of its matrix): another car nose to tail
 * in front of it, or one right behind it that reversing would back into. */
struct Surroundings
{
    bool blockedAhead = false;
    bool blockedBehind = false;
    float aheadAcross = 0.0f;    // the nearest one ahead: how far to its right
    float aheadAlong = 1e9f;
};

Surroundings around(win32::WinApplication* app, x86::reg32 car, int self)
{
    Surroundings result;
    const float px = app->getMemory<float>(car + 0x130), py = app->getMemory<float>(car + 0x134),
                pz = app->getMemory<float>(car + 0x138);
    const float fx = app->getMemory<float>(car + 0xd8), fy = app->getMemory<float>(car + 0xdc),
                fz = app->getMemory<float>(car + 0xe0);
    const float rx = app->getMemory<float>(car + 0xc0), ry = app->getMemory<float>(car + 0xc4),
                rz = app->getMemory<float>(car + 0xc8);
    for (int k = 0; k < 8; ++k)
    {
        if (k == self)
            continue;
        const x86::reg32 other = kCars + kCarSize * x86::reg32(k);
        const float dx = app->getMemory<float>(other + 0x130) - px;
        const float dy = app->getMemory<float>(other + 0x134) - py;
        const float dz = app->getMemory<float>(other + 0x138) - pz;
        const float along = dx * fx + dy * fy + dz * fz;
        const float across = dx * rx + dy * ry + dz * rz;
        if (!(across > -2.6f && across < 2.6f))
            continue;
        if (along > 0.0f && along < 7.0f)
        {
            if (along < result.aheadAlong)
            {
                result.aheadAlong = along;
                result.aheadAcross = across;
            }
            result.blockedAhead = true;
        }
        if (along < 0.0f && along > -7.0f)
            result.blockedBehind = true;
    }
    return result;
}

/* The game's own reverse (sub_405fd0, way 0), with a time to stop at so that
 * collisions do not turn it forward again. */
void backOff(win32::WinApplication* app, x86::reg32 car, x86::sreg32 tick, float seconds)
{
    app->getMemory<x86::reg32>(car + 0x7d4) = 1;
    app->getMemory<x86::reg32>(car + 0x658) = app->getMemory<x86::reg32>(car + 0x658) + 1;
    app->getMemory<x86::reg32>(car + 0x7d0) = x86::reg32(tick + x86::sreg32(seconds * kTicksPerSecond));
    app->getMemory<x86::reg8>(car + 0x620) = x86::reg8(app->getMemory<x86::reg8>(car + 0x620) | 0x40);
}

void unstick(win32::WinApplication* app, x86::reg32 car, int i)
{
    Opponent& o = s_opponents[i];
    const x86::sreg32 tick = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const x86::sreg32 node = x86::sreg32(app->getMemory<x86::reg32>(car + 0x1c));
    // A new race, or the car put back somewhere else: start over.
    if (tick < o.lastTick || tick - o.lastTick > kTicksPerSecond || node < o.node - 30 || node > o.node + 30)
    {
        o = Opponent();
        o.node = node;
        o.progressAt = tick;
    }
    o.lastTick = tick;
    /* After getting stuck it keeps to another line across the road for a
     * while (unstick, below): the lane the game gives it, [+0x7b4], set each
     * frame before sub_408870 steers it there. */
    if (o.avoidUntil >= 0)
    {
        if (tick < o.avoidUntil)
            app->getMemory<float>(car + 0x7b4) = o.avoidLateral;
        else
            o.avoidUntil = -1;
    }
    if (node > o.node)
    {
        o.node = node;
        o.progressAt = tick;
        o.tries = 0;
    }
    {
        const float want = app->getMemory<float>(car + 0x7b0);
        if (want != o.wantWritten && want > 0.0f)
        {
            o.wantWritten = want * driverOf(app, car).corners * (1.0f + (aiPace() - 1.0f) * 0.5f);
            app->getMemory<float>(car + 0x7b0) = o.wantWritten;
        }
    }
    // Not yet racing, or airborne.
    if (tick < x86::sreg32(app->getMemory<x86::reg32>(car + 0x65c)) || app->getMemory<x86::reg16>(car + 0x164) != 0)
        return;
    const Driver& driver = driverOf(app, car);
    const float forward = app->getMemory<float>(car + 0x4f4);
    const float speed = forward < 0.0f ? -forward : forward;
    const float turned = o.heading < 0.0f ? -o.heading : o.heading;
    const float still = float(tick - o.progressAt) / kTicksPerSecond;
    const bool reversing = app->getMemory<x86::reg32>(car + 0x7d4) == 1;
    const bool stuck = (speed < 3.0f && still > driver.patience)
                       || (turned > 0.3f && speed < 8.0f && still > driver.patience * 0.75f);
    if (!stuck)
    {
        o.stuckFrom = -1;
        return;
    }
    if (o.stuckFrom < 0)
        o.stuckFrom = tick;
    const float stuckFor = float(tick - o.stuckFrom) / kTicksPerSecond;
    // Nothing has helped: the game's own reset, now rather than in 30 s.
    if (stuckFor > 3.0f * (driver.backOff + 1.0f) || o.tries >= 3)
    {
        if (aiTracing())
            SDL_Log("[AI] car %d stuck %.1fs at node %d: put back on the road", i, double(stuckFor), int(node));
        app->getMemory<x86::reg32>(car + 0x658) = 9;
        app->getMemory<x86::reg32>(car + 0x7d8) = 1001;
        o.progressAt = tick;
        o.stuckFrom = -1;
        o.tries = 0;
        return;
    }
    /* In a clump the one in front drives away and the ones behind make room:
     * each steps aside from the car ahead of it, and backs off only when
     * there is nothing right behind it to back into.  The road runs from
     * -left to right across ([node+0x1c] and [+0x20], 16.16), the car half as
     * wide as [+0x114]. */
    const Surroundings near = around(app, car, i);
    if (near.blockedBehind && !near.blockedAhead && o.tries < 2)
        return;
    const x86::reg32 record = app->getMemory<x86::reg32>(0x5dd948) + x86::reg32(node) * 0x24;
    const float left = float(x86::sreg32(app->getMemory<x86::reg32>(record + 0x1c))) / 65536.0f;
    const float right = float(x86::sreg32(app->getMemory<x86::reg32>(record + 0x20))) / 65536.0f;
    const float half = app->getMemory<float>(car + 0x114);
    const float lateral = app->getMemory<float>(car + 0x7b8);
    auto keepTo = [&](float target, const char* why) {
        const float lowest = -left + half * 1.5f, highest = right - half * 1.5f;
        if (!(highest > lowest))
            return;
        o.avoidLateral = target < lowest ? lowest : target > highest ? highest : target;
        o.avoidUntil = tick + 4 * kTicksPerSecond;
        if (aiTracing())
            SDL_Log("[AI] car %d %s at node %d across %.1f (road %.1f..%.1f): keeps to %.1f", i, why, int(node),
                    double(lateral), double(-left), double(right), double(o.avoidLateral));
    };
    if (o.avoidUntil < 0)
    {
        if (near.blockedAhead)
            keepTo(lateral + (near.aheadAcross > 0.0f ? -1.0f : 1.0f) * 4.0f * half, "behind a car");
        else if (turned < 0.12f)
            // Facing the right way, nothing ahead but the road itself: the wall
            // between the two ways of a fork.  The other half of the road.
            keepTo(lateral >= (right - left) * 0.5f ? -left : right, "against a wall");
    }
    if (near.blockedAhead && near.blockedBehind && o.tries < 2)
    {
        // Hemmed in: only the step aside, until it has had a chance to work.
        ++o.tries;
        o.progressAt = tick + kTicksPerSecond;
        return;
    }
    if (!reversing)
    {
        // Each try a little longer; each slot on its own time.
        const float seconds = driver.backOff * (1.0f + 0.35f * float(o.tries)) + 0.1f * float(i % 4);
        backOff(app, car, tick, seconds);
        ++o.tries;
        o.progressAt = tick + x86::sreg32(seconds * kTicksPerSecond);  // give it the reverse and a moment after
        if (aiTracing())
            SDL_Log("[AI] car %d (%s) stuck at node %d, speed %.1f, heading %+.2f: backs off %.1fs (try %d)"
                    " [ahead %d behind %d]", i, driver.character, int(node), double(speed), double(o.heading),
                    double(seconds), o.tries, int(near.blockedAhead), int(near.blockedBehind));
    }
}

}  // namespace

bool aiDrive(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 car = cpu.eax;
    const int i = opponentIndex(car);
    if (i < 0)
        return false;
    if (aiTuned() && !networkRace(app))
        unstick(app, car, i);
    if (aiTracing())
    {
        Opponent& o = s_opponents[i];
        static x86::sreg32 s_last[kMaxCars] = {};
        const x86::sreg32 tick = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
        if (tick - s_last[i] >= kTicksPerSecond || tick < s_last[i])
        {
            s_last[i] = tick;
            SDL_Log("[AI] %ums car %d node %d speed %.1f want %.1f heading %+.2f reverse %u tries %d still %.1fs", unsigned(SDL_GetTicks()), i,
                    int(app->getMemory<x86::reg32>(car + 0x1c)), double(app->getMemory<float>(car + 0x7c4)),
                    double(app->getMemory<float>(car + 0x7b0)), double(o.heading),
                    unsigned(app->getMemory<x86::reg32>(car + 0x7d4)), o.tries,
                    double(tick - o.progressAt) / kTicksPerSecond);
        }
    }
    return false;
}

bool aiPull(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!aiNativesOn())
        return false;
    const x86::reg32 car = cpu.eax;
    ai::aiPull(app, cpu);
    // Port: the difficulty a notch up, and each driver's own pull (above).
    if (aiTuned() && !networkRace(app) && opponentIndex(car) >= 0)
    {
        const float scale = aiPace() * driverOf(app, car).pull;
        float p;
        std::memcpy(&p, &cpu.eax, 4);
        if (p > 0.0f && scale != 1.0f)
        {
            p *= scale;
            std::memcpy(&cpu.eax, &p, 4);
            cpu.fpu.st(0) = x86::Float(p);
        }
    }
    return true;
}

bool aiHeading(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!aiNativesOn())
        return false;
    const x86::reg32 car = cpu.eax;
    ai::aiHeading(app, cpu);
    const int i = opponentIndex(car);
    if (i >= 0)
        s_opponents[i].heading = app->getMemory<float>(0x56ec60);
    return true;
}

}  // namespace nfs3hp
