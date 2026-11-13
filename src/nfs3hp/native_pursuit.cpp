#include <nfs3hp.h>
#include <SDL3/SDL.h>
#include <array>
#include <cmath>
#include <vector>

/* Port: Hot Pursuit's spike strips (NFS_SPIKE_STRIPS=1; off for now).
 *
 * The game has spike belts for a player who drives a cop, and never has its
 * own cops lay one.  Here a cop that has kept ahead of the player for a while
 * stops and lays one across the road between them:
 *
 * - When: Hot Pursuit, the player a racer ([car+0x200] & 0x20 is a cop's), a
 *   cop of the game's 6 to 90 nodes ahead of the player along the track ([car
 *   +0x1c]) for 6 seconds on end, the player faster than 15 m/s and at least
 *   150 m short of the strip (or four seconds at the speed it goes): time to
 *   see it and steer round it.  One strip at a time, the first after 20
 *   seconds and 25 between them, none in a network race.
 * - Where: two car lengths behind the cop, across the way it drives, 7 m of it
 *   on the cop's own line -- so there is road beside it.  (Laid from the cars,
 *   not the track's nodes, [0x5dd948]: their places are not where the cars'
 *   put the road.)  The cop is put a car's length on past one end and held
 *   there, standing (its place and speed set each frame and each step of its
 *   driving, aiDrive; its node, [+0x1c], the one it had, so it is drawn); it
 *   rejoins the chase when the player has gone past -- over the strip or round
 *   it -- or after 20 seconds.  The strip is taken up once the player is 60 m
 *   past it, or after 30 seconds.
 * - A hit: each of a car's four wheels (its place, +0x98, to each side by
 *   0.8 m and fore and aft by 1.3 m along its axes, +0xc0 and +0xd8) that
 *   crosses the strip between two frames counts once.  Every car does --
 *   the player and the other racers alike, not the cops.  For 8 seconds and
 *   4 more a tyre the car is slower: its top speed falls over two seconds to
 *   12% less a tyre, the speed it gains each frame is 15% less a tyre, both
 *   coming back over the last third.  The car's speed is its velocity, +0xa4,
 *   scaled each frame.
 *
 * The strip is drawn as a row of spikes on a dark belt by the renderer
 * (EffectParticle::Spike, native_particles.cpp's particlesFrame). */
namespace nfs3hp
{

namespace
{

constexpr x86::reg32 kCars = 0x5e1120, kCarSize = 0x9ac;
constexpr int kMaxCars = 16;
constexpr x86::reg32 kPlayerCar = 0x5efa48;
constexpr x86::reg32 kRaceType = 0x6fd3b8;
constexpr x86::reg32 kNodeCount = 0x5dd940;

// Off unless NFS_SPIKE_STRIPS=1 while the strip and its cop are not yet seen
// on screen (see the handoff): a strip no one can see is no fair strip.
bool spikesOn()
{
    static const bool on = [] {
        const char* value = SDL_getenv("NFS_SPIKE_STRIPS");
        return value && (SDL_strcmp(value, "1") == 0 || SDL_strcasecmp(value, "on") == 0);
    }();
    return on;
}

bool tracing()
{
    static const bool on = [] {
        const char* value = SDL_getenv("NFS_AI_TRACE");
        return value && SDL_strcmp(value, "1") == 0;
    }();
    return on;
}

struct Strip
{
    bool live = false;
    x86::sreg32 node = 0;
    float centre[3] = {}, along[3] = {}, across[3] = {};
    float offset = 0.0f, half = 3.5f;
    x86::reg32 cop = 0;
    bool copHeld = false;
    float copAt[3] = {};
    float laidAt = 0.0f;
    float passedAt = -1.0f;
    float lastAlong[kMaxCars][4] = {};
    bool seen[kMaxCars] = {};
    bool hit[kMaxCars] = {};
};

struct Slowed
{
    float until = -1.0f, from = 0.0f, length = 0.0f;
    float top = 0.0f;          // the top speed it falls to
    float startSpeed = 0.0f;
    float accel = 1.0f;
    float lastSpeed = -1.0f;
};

Strip s_strip;
Slowed s_slowed[kMaxCars];
float s_time = 0.0f;           // seconds of the race's clock
x86::sreg32 s_clock = -1;
float s_aheadSince[kMaxCars];
float s_nextStrip = 0.0f;

float f(win32::WinApplication* app, x86::reg32 address)
{
    return app->getMemory<float>(address);
}

x86::sreg32 nodesAhead(x86::sreg32 from, x86::sreg32 to, x86::sreg32 count)
{
    const x86::sreg32 d = to - from;
    return d < -count / 2 ? d + count : d > count / 2 ? d - count : d;
}

void wheels(win32::WinApplication* app, x86::reg32 car, float out[4][3])
{
    const float p[3] = { f(app, car + 0x98), f(app, car + 0x9c), f(app, car + 0xa0) };
    const float r[3] = { f(app, car + 0xc0), f(app, car + 0xc4), f(app, car + 0xc8) };
    const float fw[3] = { f(app, car + 0xd8), f(app, car + 0xdc), f(app, car + 0xe0) };
    for (int w = 0; w < 4; ++w)
    {
        const float side = (w & 1) ? 0.8f : -0.8f, ahead = (w & 2) ? 1.3f : -1.3f;
        for (int k = 0; k < 3; ++k)
            out[w][k] = p[k] + r[k] * side + fw[k] * ahead;
    }
}

float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

x86::sreg32 s_holdNode = 0;

void hold(win32::WinApplication* app, x86::reg32 car, const float at[3])
{
    app->getMemory<x86::reg32>(car + 0x1c) = x86::reg32(s_holdNode);   // drawn and driven as where it stands
    for (int k = 0; k < 3; ++k)
    {
        app->getMemory<float>(car + 0x98 + x86::reg32(k) * 4) = at[k];
        app->getMemory<float>(car + 0xa4 + x86::reg32(k) * 4) = 0.0f;
        app->getMemory<float>(car + 0xe4 + x86::reg32(k) * 4) = 0.0f;
    }
}

/* A strip laid at `at`, across a road running `way` (level, made unit here),
 * and the cop held on the shoulder past one end of it.  The track's nodes
 * ([0x5dd948]) are not where the cars' places put the road -- the player's
 * nearest one came out 25 m from it -- so the strip is laid from the cars:
 * where a cop drives is road. */
void lay(win32::WinApplication* app, x86::reg32 player, x86::reg32 cop, const float at[3], const float way[3], x86::sreg32 node)
{
    Strip& s = s_strip;
    s = Strip();
    float a[3] = { way[0], 0.0f, way[2] };
    const float length = std::sqrt(a[0] * a[0] + a[2] * a[2]);
    if (!(length > 0.01f))
        return;
    s.live = true;
    s.node = node;
    s.cop = cop;
    s.laidAt = s_time;
    a[0] /= length;
    a[2] /= length;
    for (int k = 0; k < 3; ++k)
    {
        s.centre[k] = at[k];
        s.along[k] = a[k];
    }
    s.centre[1] -= 0.45f;   // a car's place is its middle: the road is below
    s.across[0] = a[2];
    s.across[1] = 0.0f;
    s.across[2] = -a[0];
    s.offset = 0.0f;
    // The cop beside one end, a car's length on, on a side picked by its seed.
    const float side = (int(s_time * 10.0f) & 1) ? 1.0f : -1.0f;
    for (int k = 0; k < 3; ++k)
        s.copAt[k] = s.centre[k] + s.across[k] * side * (s.half + 2.4f) + s.along[k] * 4.0f;
    s.copAt[1] = at[1];
    s.copHeld = true;
    s_holdNode = node;
    hold(app, cop, s.copAt);
    if (tracing())
        SDL_Log("[PURSUIT] cop %d lays a spike strip at %.0f %.0f %.0f; the player at %.0f %.0f %.0f",
                int((cop - kCars) / kCarSize), double(s.centre[0]), double(s.centre[1]), double(s.centre[2]),
                double(f(app, player + 0x98)), double(f(app, player + 0x9c)), double(f(app, player + 0xa0)));
}

void slow(win32::WinApplication* app, int c, x86::reg32 car, int tyres)
{
    Slowed& w = s_slowed[c];
    const float speed = std::sqrt(f(app, car + 0xa4) * f(app, car + 0xa4) + f(app, car + 0xa8) * f(app, car + 0xa8)
                                  + f(app, car + 0xac) * f(app, car + 0xac));
    w.from = s_time;
    w.length = 8.0f + 4.0f * float(tyres);
    w.until = s_time + w.length;
    w.startSpeed = SDL_max(speed, 20.0f);
    w.top = w.startSpeed * (1.0f - 0.12f * float(tyres));
    w.accel = 1.0f - 0.15f * float(tyres);
    w.lastSpeed = speed;
    if (tracing())
        SDL_Log("[PURSUIT] car %d over the spikes on %d tyre%s at %.0f km/h: slowed for %.0f s", c, tyres,
                tyres == 1 ? "" : "s", double(speed * 3.6f), double(w.length));
}

void applySlowed(win32::WinApplication* app)
{
    for (int c = 0; c < kMaxCars; ++c)
    {
        Slowed& w = s_slowed[c];
        if (w.until < 0.0f)
            continue;
        const x86::reg32 car = kCars + kCarSize * x86::reg32(c);
        if (s_time >= w.until || s_time < w.from)
        {
            w.until = -1.0f;
            continue;
        }
        // How much of the slowing holds now: in over two seconds, out over the last third.
        const float t = s_time - w.from;
        const float in = SDL_min(1.0f, t / 2.0f);
        const float out = SDL_min(1.0f, (w.until - s_time) / (w.length / 3.0f));
        const float k = SDL_min(in, out);
        const float top = w.startSpeed + (w.top - w.startSpeed) * k;
        const float accel = 1.0f + (w.accel - 1.0f) * out;
        float v[3] = { f(app, car + 0xa4), f(app, car + 0xa8), f(app, car + 0xac) };
        const float speed = std::sqrt(dot3(v, v));
        float wanted = speed;
        if (w.lastSpeed >= 0.0f && speed > w.lastSpeed)
            wanted = w.lastSpeed + (speed - w.lastSpeed) * accel;
        wanted = SDL_min(wanted, top);
        if (speed > 0.1f && wanted < speed)
        {
            const float scale = wanted / speed;
            for (int q = 0; q < 3; ++q)
                app->getMemory<float>(car + 0xa4 + x86::reg32(q) * 4) = v[q] * scale;
        }
        w.lastSpeed = SDL_min(speed, wanted);
    }
}

}  // namespace

/* aiDrive (native_ai.cpp), each step of a car's driving: true when it is a cop
 * held by its spike strip, which is kept standing. */
bool pursuitHolds(win32::WinApplication* app, x86::reg32 car)
{
    if (!s_strip.live || !s_strip.copHeld || car != s_strip.cop)
        return false;
    hold(app, car, s_strip.copAt);
    return true;
}

/* Each frame, as the main view's sky is drawn (native_particles.cpp): the
 * cops watched, a strip laid, held, crossed and taken up, the slowed cars kept
 * slow; `teeth` gets the strip's spikes, where they stand. */
void pursuitFrame(win32::WinApplication* app, std::vector<std::array<float, 3>>& teeth)
{
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(0x7d3684));
    if (s_clock < 0 || clock < s_clock)
    {
        // A new race: nothing carried over.
        s_strip = Strip();
        for (Slowed& w : s_slowed)
            w = Slowed();
        for (float& a : s_aheadSince)
            a = -1.0f;
        s_time = 0.0f;
        s_nextStrip = 20.0f;
    }
    if (clock - s_clock <= 64 && s_clock >= 0)
        s_time += float(clock - s_clock) / 64.0f;
    s_clock = clock;
    if (!spikesOn() || app->getMemory<x86::reg32>(kRaceType) != 3 || app->getMemory<x86::reg32>(0x6fd3b0) == 2)
        return;
    const x86::reg32 player = app->getMemory<x86::reg32>(kPlayerCar);
    const x86::sreg32 count = x86::sreg32(app->getMemory<x86::reg32>(kNodeCount));
    if (!player || count <= 0 || (app->getMemory<x86::reg8>(player + 0x200) & 0x20))
        return;   // no race, or the player is a cop
    const x86::sreg32 playerNode = x86::sreg32(app->getMemory<x86::reg32>(player + 0x1c));
    const float playerSpeed = std::fabs(f(app, player + 0xb8));
    int cars = int(app->getMemory<x86::reg32>(0x6fd510));
    cars = cars < 1 ? 1 : cars > kMaxCars ? kMaxCars : cars;
    applySlowed(app);

    Strip& s = s_strip;
    if (s.live)
    {
        // Crossings, by every racer's wheels.
        for (int c = 0; c < cars; ++c)
        {
            const x86::reg32 car = kCars + kCarSize * x86::reg32(c);
            if (app->getMemory<x86::reg8>(car + 0x200) & 0x30)
                continue;   // a cop, or traffic
            float w[4][3];
            wheels(app, car, w);
            int crossed = 0;
            for (int q = 0; q < 4; ++q)
            {
                const float d[3] = { w[q][0] - s.centre[0], 0.0f, w[q][2] - s.centre[2] };
                const float along = dot3(d, s.along), across = dot3(d, s.across) - s.offset;
                const float last = s.lastAlong[c][q];
                if (s.seen[c] && !s.hit[c] && ((last < 0.0f) != (along < 0.0f)) && std::fabs(along - last) < 6.0f
                    && std::fabs(across) <= s.half && std::fabs(w[q][1] - s.centre[1]) < 3.0f)
                    ++crossed;
                s.lastAlong[c][q] = along;
            }
            s.seen[c] = true;
            if (crossed > 0)
            {
                s.hit[c] = true;
                slow(app, c, car, crossed);
            }
        }
        // The player past it, over it or round it.
        const float pd[3] = { f(app, player + 0x98) - s.centre[0], 0.0f, f(app, player + 0xa0) - s.centre[2] };
        const float playerAlong = dot3(pd, s.along);
        if (s.passedAt < 0.0f && playerAlong > 3.0f && std::fabs(playerAlong) < 40.0f)
        {
            s.passedAt = s_time;
            if (tracing())
                SDL_Log("[PURSUIT] the player is past the strip (%s)", s.hit[(player - kCars) / kCarSize] ? "hit" : "missed");
        }
        if (s.copHeld && ((s.passedAt >= 0.0f && s_time - s.passedAt > 1.0f) || s_time - s.laidAt > 20.0f))
        {
            s.copHeld = false;
            if (tracing())
                SDL_Log("[PURSUIT] the cop rejoins the chase");
        }
        if (s.copHeld)
            hold(app, s.cop, s.copAt);
        if (tracing())
        {
            static float s_said = -1.0f;
            if (s_time - s_said >= 1.0f || s_time < s_said)
            {
                s_said = s_time;
                SDL_Log("[PURSUIT] %.1f s: the player %.0f m from the strip; the cop at %.1f %.1f %.1f (held at %.1f %.1f %.1f) 910: %.1f %.1f %.1f",
                        double(s_time), double(playerAlong), double(f(app, s.cop + 0x98)), double(f(app, s.cop + 0x9c)),
                        double(f(app, s.cop + 0xa0)), double(s.copAt[0]), double(s.copAt[1]), double(s.copAt[2]),
                        double(f(app, s.cop + 0x910)), double(f(app, s.cop + 0x914)), double(f(app, s.cop + 0x918)));
            }
        }
        if ((s.passedAt >= 0.0f && playerAlong > 60.0f) || s_time - s.laidAt > 30.0f)
        {
            s.live = false;
            s_nextStrip = s_time + 25.0f;
            if (tracing())
                SDL_Log("[PURSUIT] the strip is taken up");
        }
        // Its spikes, every quarter metre along it.
        for (float x = -s.half; x <= s.half + 1e-3f; x += 0.18f)
            teeth.push_back({ s.centre[0] + s.across[0] * (s.offset + x), s.centre[1] + 0.1f,
                              s.centre[2] + s.across[2] * (s.offset + x) });
        return;
    }

    // A cop that has kept ahead of the player.
    if (s_time < s_nextStrip)
        return;
    static const bool test = SDL_getenv("NFS_SPIKE_TEST") != nullptr;
    if (test && s_time > 4.0f && playerSpeed > 10.0f)
    {
        // Testing: the first cop lays one 25 nodes ahead of the player at once.
        for (int c = 0; c < cars; ++c)
        {
            const x86::reg32 car = kCars + kCarSize * x86::reg32(c);
            if ((app->getMemory<x86::reg8>(car + 0x200) & 0x20) && car != player)
            {
                const float way[3] = { f(app, player + 0xd8), 0.0f, f(app, player + 0xe0) };
                const float at[3] = { f(app, player + 0x98) + way[0] * 90.0f, f(app, player + 0x9c),
                                      f(app, player + 0xa0) + way[2] * 90.0f };
                lay(app, player, car, at, way, playerNode);
                return;
            }
        }
    }
    for (int c = 0; c < cars; ++c)
    {
        const x86::reg32 car = kCars + kCarSize * x86::reg32(c);
        if (!(app->getMemory<x86::reg8>(car + 0x200) & 0x20) || car == player)
        {
            s_aheadSince[c] = -1.0f;
            continue;
        }
        const x86::sreg32 node = x86::sreg32(app->getMemory<x86::reg32>(car + 0x1c));
        const x86::sreg32 lead = nodesAhead(playerNode, node, count);
        if (lead < 6 || lead > 90)
        {
            s_aheadSince[c] = -1.0f;
            continue;
        }
        if (s_aheadSince[c] < 0.0f)
            s_aheadSince[c] = s_time;
        if (s_time - s_aheadSince[c] < 6.0f || playerSpeed < 15.0f)
            continue;
        // Two car lengths behind the cop, across the way it drives.
        const float way[3] = { f(app, car + 0xd8), 0.0f, f(app, car + 0xe0) };
        const float at[3] = { f(app, car + 0x98) - way[0] * 9.0f, f(app, car + 0x9c), f(app, car + 0xa0) - way[2] * 9.0f };
        const float pp[3] = { f(app, player + 0x98), f(app, player + 0x9c), f(app, player + 0xa0) };
        const float d[3] = { at[0] - pp[0], at[1] - pp[1], at[2] - pp[2] };
        const float distance = std::sqrt(dot3(d, d));
        if (distance < SDL_max(150.0f, playerSpeed * 4.0f) || distance > 450.0f)
            continue;
        lay(app, player, car, at, way, node);
        s_aheadSince[c] = -1.0f;
        return;
    }
}

}  // namespace nfs3hp
