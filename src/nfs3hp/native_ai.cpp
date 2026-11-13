#include "native_ai.h"
#include <nfs3hp.h>
#include <SDL3/SDL.h>
#include <cmath>
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
    x86::sreg32 forkPast = -1;   // keeping off a split in the road till past this node, or -1
    x86::sreg32 behindFrom = -1; // since when it has been held up behind the player, or -1
    x86::sreg32 behindLast = -1; // the last tick it was
    x86::sreg32 flashFrom = -1;  // since when it has been flashing its lights, or -1
    x86::reg32 flashKept = 0;    // its lights, [+0x660], as they were before
    float behindWait = 2.0f;     // seconds held up before it flashes
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

/* The cars in the race ([0x6fd510]): racers, cops and traffic, which with a
 * full grid and traffic (raceGrid below) go past the first eight. */
int carsInRace(win32::WinApplication* app)
{
    const x86::sreg32 count = x86::sreg32(app->getMemory<x86::reg32>(0x6fd510));
    return count < 8 ? 8 : count > kMaxCars ? kMaxCars : int(count);
}

const Driver& driverOf(win32::WinApplication* app, x86::reg32 car)
{
    const x86::reg32 slot = app->getMemory<x86::reg32>(car + 0x1f0);
    return kDrivers[slot < sizeof kDrivers / sizeof kDrivers[0] ? slot : 0];
}

/* What is around a car, from the cars' positions (+0x98) against its own
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
    const float px = app->getMemory<float>(car + 0x98), py = app->getMemory<float>(car + 0x9c),
                pz = app->getMemory<float>(car + 0xa0);
    const float fx = app->getMemory<float>(car + 0xd8), fy = app->getMemory<float>(car + 0xdc),
                fz = app->getMemory<float>(car + 0xe0);
    const float rx = app->getMemory<float>(car + 0xc0), ry = app->getMemory<float>(car + 0xc4),
                rz = app->getMemory<float>(car + 0xc8);
    for (int k = 0; k < carsInRace(app); ++k)
    {
        if (k == self)
            continue;
        const x86::reg32 other = kCars + kCarSize * x86::reg32(k);
        const float dx = app->getMemory<float>(other + 0x98) - px;
        const float dy = app->getMemory<float>(other + 0x9c) - py;
        const float dz = app->getMemory<float>(other + 0xa0) - pz;
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

/* Splits in the road, ahead of time.  Each track node's word at +0xc is the
 * road across it in slices, a bit to each: set where there is road to drive
 * on, the lowest set bit at the right-hand edge ([node+0x20] across) and the
 * highest at the left-hand one ([+0x1c], the negative side of [car+0x7b8]).
 * Most nodes are one run of set bits; where the road splits -- a fork, an
 * island, a pillar between the two ways -- there is a gap in it (the game's
 * camera looks for just that, sub_423fe0, and the opponents never did).  So
 * every opponent looks a couple of seconds down the road, and when the line
 * it is aiming for runs into the gap it picks the nearer way round and keeps
 * to it till it is past. */
struct RoadAcross
{
    unsigned mask = 0;
    int lo = 0, hi = -1;
    float left = 0.0f, right = 0.0f, width = 0.0f;   // width: of a slice
    bool split() const
    {
        if (hi < lo)
            return false;
        const unsigned run = ((2u << hi) - 1u) & ~((1u << lo) - 1u);
        return (mask & run) != run;
    }
    int sliceAt(float lateral) const
    {
        const int k = hi - int((lateral + left) / width);
        return k < lo ? lo : k > hi ? hi : k;
    }
    float edgeOf(int slice) const { return -left + float(hi - slice) * width; }  // its left-hand edge
};

RoadAcross roadAcross(win32::WinApplication* app, x86::sreg32 node)
{
    RoadAcross road;
    const x86::reg32 record = app->getMemory<x86::reg32>(0x5dd948) + x86::reg32(node) * 0x24;
    road.mask = app->getMemory<x86::reg16>(record + 0xc);
    road.left = float(x86::sreg32(app->getMemory<x86::reg32>(record + 0x1c))) / 65536.0f;
    road.right = float(x86::sreg32(app->getMemory<x86::reg32>(record + 0x20))) / 65536.0f;
    for (int k = 0; k < 16; ++k)
        if (road.mask >> k & 1u)
        {
            if (road.hi < road.lo)
                road.lo = k;
            road.hi = k;
        }
    if (road.hi >= road.lo)
        road.width = (road.left + road.right) / float(road.hi - road.lo + 1);
    if (!(road.width > 0.5f))
        road.hi = road.lo - 1;
    return road;
}

float nodeDistance(win32::WinApplication* app, x86::sreg32 a, x86::sreg32 b)
{
    const x86::reg32 nodes = app->getMemory<x86::reg32>(0x5dd948);
    float sum = 0.0f;
    for (int q = 0; q < 3; ++q)
    {
        const float d = float(x86::sreg32(app->getMemory<x86::reg32>(nodes + x86::reg32(a) * 0x24 + q * 4))
                              - x86::sreg32(app->getMemory<x86::reg32>(nodes + x86::reg32(b) * 0x24 + q * 4))) / 65536.0f;
        sum += d * d;
    }
    return SDL_sqrtf(sum);
}

void forkAhead(win32::WinApplication* app, x86::reg32 car, int i, x86::sreg32 node, x86::sreg32 tick)
{
    Opponent& o = s_opponents[i];
    const x86::sreg32 count = x86::sreg32(app->getMemory<x86::reg32>(0x5dd940));
    if (count <= 0 || node < 0 || node >= count)
        return;
    auto ahead = [&](x86::sreg32 from, x86::sreg32 to) {   // nodes from `from` on to `to`
        const x86::sreg32 d = to - from;
        return d < -count / 2 ? d + count : d > count / 2 ? d - count : d;
    };
    if (o.forkPast >= 0)
    {
        if (ahead(node, o.forkPast) >= 0 && tick < o.avoidUntil)
            return;
        o.forkPast = -1;
        o.avoidUntil = -1;
    }
    if (o.avoidUntil >= 0)
        return;   // stepping out of trouble already (unstick)
    const float forward = app->getMemory<float>(car + 0x4f4);
    const float reach = SDL_max(30.0f, (forward < 0.0f ? -forward : forward) * 2.5f);
    const float half = app->getMemory<float>(car + 0x114);
    const float aim = app->getMemory<float>(car + 0x7b4);
    float along = 0.0f;
    x86::sreg32 previous = node;
    for (int step = 0; step <= 40 && along <= reach; ++step)
    {
        const x86::sreg32 m = (node + step) % count;
        along += step ? nodeDistance(app, previous, m) : 0.0f;
        previous = m;
        const RoadAcross road = roadAcross(app, m);
        if (!road.split())
            continue;
        // The slices its car would cover, there.
        const int from = road.sliceAt(aim + half), to = road.sliceAt(aim - half);
        bool clear = true;
        for (int k = from; k <= to; ++k)
            clear = clear && (road.mask >> k & 1u);
        if (clear)
            return;
        // The nearer run of road, and in it the line nearest the one it had.
        float best = 0.0f, bestOff = 1e9f;
        for (int k = road.lo; k <= road.hi; ++k)
        {
            if (!(road.mask >> k & 1u) || (k > road.lo && (road.mask >> (k - 1) & 1u)))
                continue;
            int top = k;
            while (top < road.hi && (road.mask >> (top + 1) & 1u))
                ++top;
            const float low = road.edgeOf(top) + half * 1.3f, high = road.edgeOf(k) + road.width - half * 1.3f;
            const float line = high > low ? (aim < low ? low : aim > high ? high : aim) : (road.edgeOf(top) + road.edgeOf(k) + road.width) * 0.5f;
            const float off = line > aim ? line - aim : aim - line;
            if (off < bestOff)
            {
                bestOff = off;
                best = line;
            }
        }
        if (bestOff > 1e8f)
            return;
        x86::sreg32 past = m;
        for (int k = 0; k < 40 && roadAcross(app, (past + 1) % count).split(); ++k)
            past = (past + 1) % count;
        o.avoidLateral = best;
        o.avoidUntil = tick + 8 * kTicksPerSecond;
        o.forkPast = past;
        if (aiTracing())
            SDL_Log("[AI] car %d at node %d: road splits at %d..%d (%.0f m on), aiming %.1f: keeps to %.1f", i, int(node),
                    int(m), int(past), double(along), double(aim), double(best));
        return;
    }
}

/* Flashing the player out of the way.  An opponent held up right behind the
 * player -- in its slipstream, wanting to go faster than it is -- for two
 * seconds flicks its lights between full beam and what they were, [+0x660]
 * (1 dipped, 2 full beam, 0 off: set at the start of the race by
 * sub_42bcb0 and not touched again for an opponent), four times, then waits;
 * three seconds on and still held up, it does it again. */
constexpr x86::sreg32 kFlashTicks = 6;   // each on or off
constexpr int kFlashes = 4;

void signal(win32::WinApplication* app, x86::reg32 car, int i)
{
    Opponent& o = s_opponents[i];
    const x86::sreg32 tick = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    if (o.flashFrom >= 0)
    {
        const x86::sreg32 phase = (tick - o.flashFrom) / kFlashTicks;
        if (phase >= 0 && phase < 2 * kFlashes)
        {
            const x86::reg32 bright = o.flashKept == 2 ? 1 : 2;
            app->getMemory<x86::reg32>(car + 0x660) = phase % 2 == 0 ? bright : o.flashKept;
            return;
        }
        app->getMemory<x86::reg32>(car + 0x660) = o.flashKept;
        o.flashFrom = -1;
        o.behindFrom = tick;
        o.behindWait = 3.0f;
    }
    if (tick < x86::sreg32(app->getMemory<x86::reg32>(car + 0x65c)))
        return;
    const float px = app->getMemory<float>(car + 0x98), py = app->getMemory<float>(car + 0x9c),
                pz = app->getMemory<float>(car + 0xa0);
    const float fx = app->getMemory<float>(car + 0xd8), fy = app->getMemory<float>(car + 0xdc),
                fz = app->getMemory<float>(car + 0xe0);
    const float rx = app->getMemory<float>(car + 0xc0), ry = app->getMemory<float>(car + 0xc4),
                rz = app->getMemory<float>(car + 0xc8);
    auto relative = [&](x86::reg32 other, float& along, float& across) {
        const float dx = app->getMemory<float>(other + 0x98) - px, dy = app->getMemory<float>(other + 0x9c) - py,
                    dz = app->getMemory<float>(other + 0xa0) - pz;
        along = dx * fx + dy * fy + dz * fz;
        across = dx * rx + dy * ry + dz * rz;
    };
    // The player's car, [0x5efa48]; with two on one screen ([0x6fd3b0] 1) the
    // second's too, the nearer of them in front.
    float along = 1e9f, across = 0.0f;
    x86::reg32 target = 0;
    for (x86::reg32 p = 0; p < (app->getMemory<x86::reg32>(0x6fd3b0) == 1 ? 2u : 1u); ++p)
    {
        const x86::reg32 player = app->getMemory<x86::reg32>(0x5efa48 + p * 4);
        float a = 0.0f, c = 0.0f;
        if (!player || player == car)
            continue;
        relative(player, a, c);
        if (!target || (a > 0.0f && (along < 0.0f || a < along)))
        {
            target = player;
            along = a;
            across = c;
        }
    }
    if (!target)
        return;
    const float speed = app->getMemory<float>(car + 0x4f4);
    const float want = app->getMemory<float>(car + 0x7b0);
    const float facing = fx * app->getMemory<float>(target + 0xd8) + fy * app->getMemory<float>(target + 0xdc)
                         + fz * app->getMemory<float>(target + 0xe0);
    bool behind = along > 3.0f && along < 16.0f && across > -2.5f && across < 2.5f && facing > 0.9f
                  && speed > 10.0f && want > speed + 1.0f && app->getMemory<x86::reg32>(car + 0x7d4) != 1;
    // Held up by the player, not by another opponent in between.
    for (int k = 0; behind && k < carsInRace(app); ++k)
    {
        const x86::reg32 other = kCars + kCarSize * x86::reg32(k);
        if (k == i || other == target)
            continue;
        float a = 0.0f, c = 0.0f;
        relative(other, a, c);
        if (a > 0.0f && a < along && c > -2.5f && c < 2.5f)
            behind = false;
    }
    if (behind)
    {
        if (o.behindFrom < 0)
            o.behindFrom = tick;
        o.behindLast = tick;
    }
    else if (o.behindFrom >= 0 && tick - o.behindLast > kTicksPerSecond / 2)
    {
        o.behindFrom = -1;
        o.behindWait = 2.0f;
    }
    if (behind && o.behindFrom >= 0 && float(tick - o.behindFrom) >= o.behindWait * kTicksPerSecond)
    {
        o.flashKept = app->getMemory<x86::reg32>(car + 0x660);
        o.flashFrom = tick;
        if (aiTracing())
            SDL_Log("[AI] car %d held up %.1fs behind the player (%.1f m): flashes its lights", i,
                    double(tick - o.behindFrom) / kTicksPerSecond, double(along));
    }
}

void unstick(win32::WinApplication* app, x86::reg32 car, int i)
{
    Opponent& o = s_opponents[i];
    const x86::sreg32 tick = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const x86::sreg32 node = x86::sreg32(app->getMemory<x86::reg32>(car + 0x1c));
    // A new race, or the car put back somewhere else: start over.
    if (tick < o.lastTick || tick - o.lastTick > kTicksPerSecond || node < o.node - 30 || node > o.node + 30)
    {
        if (o.flashFrom >= 0 && tick >= o.lastTick)
            app->getMemory<x86::reg32>(car + 0x660) = o.flashKept;
        o = Opponent();
        o.node = node;
        o.progressAt = tick;
    }
    o.lastTick = tick;
    forkAhead(app, car, i, node, tick);
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
     * each steps aside from the car ahead of it before it backs off.  The road runs from
     * -left to right across ([node+0x1c] and [+0x20], 16.16), the car half as
     * wide as [+0x114]. */
    const Surroundings near = around(app, car, i);
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
        o.forkPast = -1;
        if (aiTracing())
            SDL_Log("[AI] car %d %s at node %d across %.1f (road %.1f..%.1f): keeps to %.1f", i, why, int(node),
                    double(lateral), double(-left), double(right), double(o.avoidLateral));
    };
    if (o.avoidUntil < 0 || o.forkPast >= 0)
    {
        if (near.blockedAhead)
            keepTo(lateral + (near.aheadAcross > 0.0f ? -1.0f : 1.0f) * 4.0f * half, "behind a car");
        else if (turned < 0.12f)
        {
            /* Facing the right way, nothing ahead but the road itself: a wall,
             * a post, the island between the two ways of a fork that the
             * look ahead (forkAhead) did not know of.  A few metres aside,
             * to where there is more road first, the other way next try --
             * not across to the far edge, which on a wide stretch is the
             * kerb and the buildings. */
            const bool roomier = lateral < (right - left) * 0.5f;
            const bool toRight = o.tries % 2 == 0 ? roomier : !roomier;
            keepTo(lateral + (toRight ? 1.0f : -1.0f) * 5.0f * half, "against a wall");
        }
    }
    if (o.tries < 2 && (near.blockedAhead || near.blockedBehind || turned < 0.3f))
    {
        /* The step aside first, twice, and backing off only when that has
         * not worked: reversing at once, in a crowd or in the wet, only
         * backs into the next car or spins on the spot (the races with it
         * so came out slower). */
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

/* Port: traffic lighter (NFS_TRAFFIC_WEIGHT, 0.5 by default), so a car that
 * hits it is not stopped dead, and the cops heavier (NFS_COP_WEIGHT, 1.25), so
 * they shove harder; 0 or off for the game's.  A car's mass is at [+0xb0]
 * with its inverse at [+0xb4], its three moments of inertia at +0xf0 .. +0xf8
 * with theirs at +0xfc .. +0x104 (the masses read from a race: 1472 the
 * player's car, 1700 a traffic van, each beside 1 over it); all are scaled
 * together, so the car turns as one of that weight would.  The game's own
 * driving of these cars (sub_4071a0) never reads the mass -- they are pushed
 * by their pull, not driven through their wheels -- so only how they hit and
 * are hit changes, not how fast they go.  Done the first time a traffic car
 * ([+0x200] & 0x10, sub_428700) or a cop ([+0x200] & 0x20) is driven here,
 * and again whenever its mass is not the one set (a new race, a car made
 * again). */
bool pursuitHolds(win32::WinApplication* app, x86::reg32 car);  // native_pursuit.cpp

/* Port: the cops' cars tuned up (NFS_COP_PACE; 0 or off for the game's), so
 * they can catch what they chase.  An opponent -- a cop too -- is pushed by a
 * pull it reads from its car's table by its speed (sub_4064f0: [car+0x528] +
 * 0x2ec, a float for each m/s up to 111, at [car+0x7c4]), and aims for a
 * speed the game sets ([+0x7b0]).  A tuned cop's pull is the table read at its
 * speed over `top` -- the curve stretched up the speeds, so it keeps pulling
 * where the stock car runs out -- times `pull`, and its aim is times `top`:
 *
 *   the Pursuit Corvette (traffic/pursuit/0005)   pull 1.20, top 1.12
 *   the Pursuit Diablo (traffic/pursuit/0001)     pull 1.15, top 1.15
 *   the rest of the force (0000, 0002-0004)       pull 1.10, top 1.08
 *
 * Which is which by the folder the race gave the car ([+0x1e4], "0005").  A
 * cop is [+0x200] & 0x20 (sub_428700); the player's own cop car is not
 * driven here. */
struct CopTune
{
    float pull = 1.0f, top = 1.0f;
};

static CopTune copTune(win32::WinApplication* app, x86::reg32 car)
{
    static const float pace = [] {
        const char* value = SDL_getenv("NFS_COP_PACE");
        if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(value));
        return parsed > 0.0f && parsed <= 3.0f ? parsed : 1.0f;
    }();
    CopTune tune;
    if (pace <= 0.0f || !(app->getMemory<x86::reg8>(car + 0x200) & 0x20))
        return tune;
    char name[5] = {};
    for (int q = 0; q < 4; ++q)
        name[q] = char(app->getMemory<x86::reg8>(car + 0x1e4 + x86::reg32(q)));
    if (SDL_strcmp(name, "0005") == 0)
        tune = { 1.20f, 1.12f };
    else if (SDL_strcmp(name, "0001") == 0)
        tune = { 1.15f, 1.15f };
    else
        tune = { 1.10f, 1.08f };
    // NFS_COP_PACE a number: how much of the tuning, 1 the above.
    tune.pull = 1.0f + (tune.pull - 1.0f) * pace;
    tune.top = 1.0f + (tune.top - 1.0f) * pace;
    return tune;
}


static float weightSetting(const char* name, float otherwise)
{
    const char* value = SDL_getenv(name);
    if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
        return otherwise;
    if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
        return 1.0f;
    const float parsed = float(SDL_atof(value));
    return parsed >= 0.1f && parsed <= 3.0f ? parsed : otherwise;
}

static void weigh(win32::WinApplication* app, x86::reg32 car, int i)
{
    static const float traffic = weightSetting("NFS_TRAFFIC_WEIGHT", 0.5f);
    static const float cops = weightSetting("NFS_COP_WEIGHT", 1.25f);
    static float s_set[kMaxCars] = {};
    const x86::reg8 kind = app->getMemory<x86::reg8>(car + 0x200);
    const float weight = (kind & 0x10) ? traffic : (kind & 0x20) ? cops : 1.0f;
    if (weight == 1.0f || networkRace(app))
        return;
    const float mass = app->getMemory<float>(car + 0xb0);
    if (!(mass > 0.0f) || mass == s_set[i])
        return;
    for (x86::reg32 o : { 0xb0u, 0xf0u, 0xf4u, 0xf8u })
        app->getMemory<float>(car + o) = app->getMemory<float>(car + o) * weight;
    for (x86::reg32 o : { 0xb4u, 0xfcu, 0x100u, 0x104u })
        app->getMemory<float>(car + o) = app->getMemory<float>(car + o) / weight;
    s_set[i] = app->getMemory<float>(car + 0xb0);
    if (aiTracing())
        SDL_Log("[AI] car %d is %s: %.0f kg, now %.0f", i, (kind & 0x10) ? "traffic" : "a cop", double(mass),
                double(s_set[i]));
}

bool aiDrive(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 car = cpu.eax;
    const int i = opponentIndex(car);
    if (i < 0)
        return false;
    /* The tuning is for the racers: the game drives its traffic (and cops)
     * through here too, and backing off, looking for forks and flashing are
     * not for them -- a racer's flag in [+0x200] is 8 (sub_428700). */
    const bool racer = (app->getMemory<x86::reg8>(car + 0x200) & 8) != 0;
    weigh(app, car, i);
    if (pursuitHolds(app, car))
        return false;   // a cop standing by its spike strip (native_pursuit.cpp)
    if (!networkRace(app))
    {
        // A tuned cop aims faster (copTune): once for each aim the game wrote.
        const CopTune tune = copTune(app, car);
        if (tune.top != 1.0f)
        {
            Opponent& o = s_opponents[i];
            const float want = app->getMemory<float>(car + 0x7b0);
            if (want != o.wantWritten && want != 0.0f)
            {
                o.wantWritten = want * tune.top;
                app->getMemory<float>(car + 0x7b0) = o.wantWritten;
            }
        }
    }
    if (aiTuned() && !networkRace(app) && racer)
    {
        unstick(app, car, i);
        signal(app, car, i);
    }
    if (aiTracing())
    {
        Opponent& o = s_opponents[i];
        // Which car it is, once a race: the folder name the race gave it, [+0x1e4].
        static x86::sreg32 s_named[kMaxCars] = {};
        const x86::sreg32 now = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
        if (now < s_named[i] || s_named[i] == 0)
        {
            char name[9] = {};
            for (int q = 0; q < 8; ++q)
                name[q] = char(x86::reg8(app->getMemory<x86::reg8>(car + 0x1e4 + q)));
            SDL_Log("[AI] car %d is %s (driver: %s)", i, name, driverOf(app, car).character);
        }
        s_named[i] = now > 0 ? now : 1;
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
            // With the first opponent, the cars past the racers ([0x6fd514]): cops, traffic.
            const int racers = int(app->getMemory<x86::reg32>(0x6fd514));
            for (int k = racers; i == 1 && k < carsInRace(app); ++k)
            {
                const x86::reg32 other = kCars + kCarSize * x86::reg32(k);
                SDL_Log("[AI] %ums car %d (not racing) node %d speed %.1f", unsigned(SDL_GetTicks()), k,
                        int(app->getMemory<x86::reg32>(other + 0x1c)), double(app->getMemory<float>(other + 0xb8)));
            }
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
    // Port: a cop's car tuned up (copTune): its curve stretched up the speeds.
    if (!networkRace(app))
    {
        const CopTune tune = copTune(app, car);
        float p;
        std::memcpy(&p, &cpu.eax, 4);
        if (tune.pull != 1.0f && p > 0.0f)
        {
            const float speed = app->getMemory<float>(car + 0x7c4);
            const x86::reg32 table = app->getMemory<x86::reg32>(car + 0x528) + 0x2ec;
            auto at = [&](float v) {
                const int k = SDL_max(0, SDL_min(0x6f, int(std::floor(v - 0.5f + 0.5f))));
                return app->getMemory<float>(table + x86::reg32(k) * 4);
            };
            const float stock = at(speed), stretched = at(speed / tune.top);
            if (stock > 0.0f)
            {
                p *= stretched / stock * tune.pull;
                std::memcpy(&cpu.eax, &p, 4);
                cpu.fpu.st(0) = x86::Float(p);
            }
        }
    }
    // Port: the difficulty a notch up, and each driver's own pull (above).
    // Racers only ([+0x200] & 8), not the traffic or the cops.
    if (aiTuned() && !networkRace(app) && opponentIndex(car) >= 0 && (app->getMemory<x86::reg8>(car + 0x200) & 8))
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

/* Port: the grid, as a race is set up.
 *
 * sub_473bf0 sets up a single race's cars, 0x6c-byte records from 0x6fd52c,
 * sixteen at most: the players ([0x6fd518]), the opponents ([0x6fd520]: with
 * a full grid, [0x6fd4d0] 2, eight less the players, else one or none), then,
 * with traffic on ([0x6fd4e8] 0), traffic ([0x6fd528]) to make eight cars in
 * all -- none, with a full grid, and the menu greys traffic out for it
 * (onOpponentFrame).  NFS_GRID_TRAFFIC (on; 0 or off for the game's) lets a
 * full grid have traffic, a third of what one opponent would have had (eight
 * less the players less one): two cars for one player.  gridTraffic runs as the
 * count is made (tools/apply_race_grid.py), the cars made after it as the game
 * makes them.
 *
 * sub_474640 sets up Hot Pursuit's: a racer has one opponent, up to six cops
 * and some traffic; the menu forces the opponents to "full grid" and greys the
 * number out (fitRaceSettings, onOpponentFrame).  The cops chase every racer
 * there is -- with the player as the cop there are six of them -- so
 * NFS_PURSUIT_GRID (on; 0 or off for the game's) lets the number be chosen,
 * and a full grid makes the player one of seven racers (kPursuitRacers, as
 * many as the cops' radio has banks for): six opponents, the cops as many as
 * the game picked, the traffic what is left of the sixteen.
 * pursuitGrid runs as the counts are made, before any car is.  Neither in a
 * network race, whose machines must all set up alike. */
namespace
{

bool gridSetting(const char* name)
{
    const char* value = SDL_getenv(name);
    return !(value && (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0));
}

constexpr x86::reg32 kRaceType = 0x6fd3b8;     // 0 a single race, 3 Hot Pursuit
constexpr x86::reg32 kOpponentNumber = 0x6fd4d0;  // 0 none, 1 one, 2 a full grid
constexpr x86::reg32 kPlayersRacing = 0x6fd518, kPlayersCops = 0x6fd51c;
constexpr x86::reg32 kOpponentCount = 0x6fd520, kCopCount = 0x6fd524, kTrafficCount = 0x6fd528;
constexpr x86::sreg32 kSlots = 16;
/* Hot Pursuit's racers, the players among them: each car that is not a cop
 * gets a bank of the cops' radio calls about it (sub_40a010, banks 0xf to
 * 0x16), and the eighth stops the game ("TOO MANY RACER BANKS"). */
constexpr x86::sreg32 kPursuitRacers = 7;

}  // namespace

bool gridTrafficOn()
{
    static const bool on = gridSetting("NFS_GRID_TRAFFIC");
    return on;
}

bool pursuitGridOn()
{
    static const bool on = gridSetting("NFS_PURSUIT_GRID");
    return on;
}

void gridTraffic(win32::WinApplication* app)
{
    auto word = [app](x86::reg32 address) -> decltype(auto) { return app->getMemory<x86::reg32>(address); };
    if (!gridTrafficOn() || networkRace(app) || word(kRaceType) != 0 || word(kOpponentNumber) != 2)
        return;
    const x86::sreg32 players = x86::sreg32(word(kPlayersRacing));
    const x86::sreg32 racers = players + x86::sreg32(word(kOpponentCount));
    const x86::sreg32 alone = 8 - players - 1;   // the traffic with one opponent
    x86::sreg32 traffic = (alone + 1) / 3;
    traffic = SDL_max(0, SDL_min(traffic, kSlots - racers));
    word(kTrafficCount) = x86::reg32(traffic);
    SDL_Log("[GRID] a full grid of %d with %d of traffic", int(racers), int(traffic));
}

void pursuitGrid(win32::WinApplication* app)
{
    auto word = [app](x86::reg32 address) -> decltype(auto) { return app->getMemory<x86::reg32>(address); };
    if (!pursuitGridOn() || networkRace(app) || word(kRaceType) != 3 || word(kOpponentNumber) != 2)
        return;
    const x86::sreg32 players = x86::sreg32(word(kPlayersRacing));
    if (players < 1 || word(kPlayersCops) != 0 || x86::sreg32(word(kOpponentCount)) < 1)
        return;   // the player a cop, or no racer to race
    const x86::sreg32 opponents = kPursuitRacers - players;
    const x86::sreg32 cops = SDL_min(x86::sreg32(word(kCopCount)), 6);
    const x86::sreg32 traffic =
        SDL_max(0, SDL_min(x86::sreg32(word(kTrafficCount)), kSlots - players - opponents - cops));
    SDL_Log("[GRID] Hot Pursuit: %d opponents (the game's %d), %d cops, %d of traffic (the game's %d)",
            int(opponents), int(word(kOpponentCount)), int(cops), int(traffic), int(word(kTrafficCount)));
    word(kOpponentCount) = x86::reg32(opponents);
    word(kCopCount) = x86::reg32(cops);
    word(kTrafficCount) = x86::reg32(traffic);
}

}  // namespace nfs3hp
