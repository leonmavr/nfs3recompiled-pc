#include <nfs3hp.h>
#include <SDL3/SDL.h>
#include <cmath>

/* Port: the ground under the player's wheels felt (NFS_SURFACE_GRIP).
 *
 * What a wheel stands on is sub_429b40's: on the ground ([car+0x164] 0) and
 * with its contact ([wheel+0x14], [wheel+0x18]; wheels are 0x78 bytes from
 * car+0x300), the byte after the polygon's first ([[wheel+0x18]+1]) & 0x3f;
 * else the car's own ([car+0x18c]).  The game's table of surfaces (16 of
 * 0x24 bytes at 0x553090) has the paved ones 1 in their first dword.  Seen:
 * 1 the road, 10 and 12 paved verges, 3 Atlantica's grass, 13 Redrock's sand
 * and dirt, 15 Summit's snow.
 *
 * The player's car, and every other car with it (NFS_GRIP_ON_OPPONENTS, on),
 * and only gently -- never enough to lose it: each
 * tick of the race's clock (64 a second) a surface off the asphalt
 *   - drags the car back (speed and acceleration lost),
 *   - softens the steering (the yaw rate [+0xe8] a little less) -- or on
 *     snow sharpens it a little, the tail stepping out (a little more, only
 *     while turning and below 1 rad/s),
 *   - on loose ground keeps a little of the sideways slip the tyres took
 *     away (the velocity along the car's right [+0xc0]), never past 2.5 m/s,
 *   - and shakes it a little (up and down, and a hair of yaw),
 * by the share of the four wheels on it, none below 15 km/h and all of it
 * from 32 km/h.  NFS_SURFACE_GRIP: 1, a number up to 3 for stronger, 0 or off
 * for none. */
namespace nfs3hp
{

namespace
{

struct Feel
{
    float drag;    // m/s lost a second for each m/s of the speed and 10 more (to 40)
    float steer;   // a fraction of the yaw rate taken a second; below 0, added (oversteer)
    float loose;   // a fraction of the lost slip kept
    float shake;   // m/s up and down, at most
};

constexpr Feel kAsphalt{ 0.0f, 0.0f, 0.0f, 0.0f };
constexpr Feel kGrass{ 0.030f, 0.9f, 0.10f, 0.10f };
constexpr Feel kDirt{ 0.020f, 0.5f, 0.25f, 0.12f };    // sand, dirt, gravel
constexpr Feel kSnow{ 0.017f, -0.45f, 0.45f, 0.06f };   // about 5% slower, the tail out a little
constexpr Feel kMud{ 0.045f, 1.0f, 0.15f, 0.08f };
constexpr Feel kOther{ 0.015f, 0.4f, 0.10f, 0.08f };   // off the road, not known

x86::reg32 word(win32::WinApplication* app, x86::reg32 a) { return app->getMemory<x86::reg32>(a); }
float value(win32::WinApplication* app, x86::reg32 a) { return app->getMemory<float>(a); }
void put(win32::WinApplication* app, x86::reg32 a, float v) { app->getMemory<float>(a) = v; }

float setting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_SURFACE_GRIP");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

/* sub_429b40: what a wheel stands on. */
x86::reg32 wheelSurface(win32::WinApplication* app, x86::reg32 car, int wheel)
{
    const x86::reg32 under = word(app, car + 0x18c);
    if (app->getMemory<uint16_t>(car + 0x164) != 0)
        return under;
    const x86::reg32 record = car + 0x300 + x86::reg32(wheel) * 0x78;
    if (word(app, record + 0x14) == 0 || word(app, record + 0x18) == 0)
        return under;
    return app->getMemory<x86::reg8>(word(app, record + 0x18) + 1) & 0x3f;
}

Feel feelOf(win32::WinApplication* app, x86::reg32 surface)
{
    // Test only (NFS_SURFACE_TEST=grass, dirt, snow or mud): every surface that one.
    static const char* test = SDL_getenv("NFS_SURFACE_TEST");
    if (test)
    {
        if (SDL_strcasecmp(test, "grass") == 0) return kGrass;
        if (SDL_strcasecmp(test, "dirt") == 0) return kDirt;
        if (SDL_strcasecmp(test, "snow") == 0) return kSnow;
        if (SDL_strcasecmp(test, "mud") == 0) return kMud;
    }
    if (word(app, 0x553090 + (surface & 0xf) * 0x24) == 1)
        return kAsphalt;   // paved
    switch (surface & 0xf)
    {
    case 2: case 3: return kGrass;
    case 8: case 9: case 13: return kDirt;
    case 15: return kSnow;
    case 5: return kMud;
    default: return kOther;
    }
}

float smoothStep(float edge0, float edge1, float x)
{
    const float t = SDL_max(0.0f, SDL_min(1.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

std::uint32_t s_random = 0x9e3779b9u;
float random11()
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return float(s_random >> 8) / 8388608.0f - 1.0f;
}

}  // namespace

/* For native_particles.cpp: what a car's wheel (0 to 3) stands on, as a kind
 * of ground -- 0 paved, 1 grass, 2 sand/dirt/gravel, 3 snow, 4 mud, 5 other. */
int groundUnder(win32::WinApplication* app, x86::reg32 car, int wheel)
{
    const x86::reg32 surface = wheelSurface(app, car, wheel);
    if (word(app, 0x553090 + (surface & 0xf) * 0x24) == 1)
        return 0;
    switch (surface & 0xf)
    {
    case 2: case 3: return 1;
    case 8: case 9: case 13: return 2;
    case 15: return 3;
    case 5: return 4;
    default: return 5;
    }
}

/* The other cars too (NFS_GRIP_ON_OPPONENTS, 1 by default; 0 or off for the
 * player's alone): the same feel of the ground on every car in the race --
 * racers, cops and traffic -- their driving left as it is. */
static bool onOpponents()
{
    static const bool value = [] {
        const char* text = SDL_getenv("NFS_GRIP_ON_OPPONENTS");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return value;
}

/* One car's tick or ticks on what its wheels stand on; slip its sideways
 * speed after the last time. */
static void feelGround(win32::WinApplication* app, x86::reg32 car, int ticks, float& slip, bool trace, x86::sreg32 clock)
{
    if (app->getMemory<uint16_t>(car + 0x164) != 0)
        return;   // in the air
    Feel feel{};
    x86::reg32 surfaces[4];
    for (int w = 0; w < 4; ++w)
    {
        surfaces[w] = wheelSurface(app, car, w);
        const Feel f = feelOf(app, surfaces[w]);
        feel.drag += f.drag * 0.25f;
        feel.steer += f.steer * 0.25f;
        feel.loose += f.loose * 0.25f;
        feel.shake += f.shake * 0.25f;
    }
    const float speed = std::fabs(value(app, car + 0xb8));
    const float amount = setting() * smoothStep(4.0f, 9.0f, speed);
    float v[3] = { value(app, car + 0xa4), value(app, car + 0xa8), value(app, car + 0xac) };
    float yaw = value(app, car + 0xe8);
    const float right[3] = { value(app, car + 0xc0), value(app, car + 0xc4), value(app, car + 0xc8) };
    const float sideways = v[0] * right[0] + v[1] * right[1] + v[2] * right[2];
    if (trace)
        SDL_Log("[SURFACE] t %d car %x wheels %u %u %u %u speed %.1f drag %.3f steer %.2f loose %.2f yaw %.3f slip %.2f",
                clock, car, surfaces[0], surfaces[1], surfaces[2], surfaces[3], speed, feel.drag, feel.steer, feel.loose,
                yaw, sideways);
    if (amount <= 0.0f || feel.drag + std::fabs(feel.steer) + feel.loose + feel.shake <= 0.0f)
    {
        slip = sideways;
        return;
    }
    const float dt = float(ticks) / 64.0f;
    // Dragged back: the speed along the way it goes.
    const float keep = 1.0f - SDL_min(feel.drag * amount * dt * SDL_min(speed + 10.0f, 40.0f) / SDL_max(speed, 1.0f), 0.05f);
    for (int k = 0; k < 3; ++k)
        if (k != 1)
            v[k] *= keep;
    // The steering softer.
    if (feel.steer >= 0.0f)
        yaw *= 1.0f - SDL_min(feel.steer * amount * dt, 0.2f);
    else if (std::fabs(yaw) > 0.05f && std::fabs(yaw) < 1.0f)
        yaw *= 1.0f + SDL_min(-feel.steer * amount * dt, 0.05f);   // turning: a little more, never past 1 rad/s
    // Some of the slip the tyres took away kept, never much of it.
    if (std::fabs(slip) > std::fabs(sideways) && std::fabs(sideways) < 2.5f)
    {
        float add = (slip - sideways) * SDL_min(feel.loose * amount, 0.6f);
        add = SDL_max(-0.3f, SDL_min(add, 0.3f));
        for (int k = 0; k < 3; ++k)
            v[k] += right[k] * add;
    }
    slip = v[0] * right[0] + v[1] * right[1] + v[2] * right[2];
    // A little shaken.
    const float pace = SDL_min(speed / 25.0f, 1.0f);
    v[1] += random11() * feel.shake * amount * pace * SDL_min(float(ticks), 2.0f) * 0.5f;
    yaw += random11() * feel.shake * amount * pace * 0.04f;
    for (int k = 0; k < 3; ++k)
        put(app, car + 0xa4 + 4 * x86::reg32(k), v[k]);
    put(app, car + 0xe8, yaw);
}

/* Cars in the air roll less (NFS_AIR_ROLL_DAMP, 1; a number up to 3 for
 * more, 0 or off for the game's).  Off a ramp the game lets a car keep
 * whatever spin it took off with -- the word at +0x164 counts the ticks it has
 * been in the air, and the spin (+0xe4, the world's axes) is left alone until
 * it lands -- so a car that left a ramp a little crooked rolled on through
 * the whole jump and often landed on its roof.  While it is in the air its
 * roll -- the spin about its own forward axis (+0xd8), and the same in the
 * car's own axes (the third of +0x4f8) -- now dies away, three
 * quarters of it in a quarter of a second (faster with a bigger number): a
 * test throw that rolled the car onto its roof and past now tips it about 40
 * degrees and it lands on its wheels.  The height of the jump,
 * the pitch and the turning are the game's.  Every car in the race. */
static float airRollSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_AIR_ROLL_DAMP");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

static void airRoll(win32::WinApplication* app, int ticks)
{
    const float setting = airRollSetting();
    if (setting <= 0.0f || ticks <= 0)
        return;
    const float keep = std::exp(-6.0f * setting * float(ticks) / 64.0f);
    const x86::reg32 count = SDL_min(word(app, 0x5efda0), x86::reg32(16));
    for (x86::reg32 k = 0; k < count; ++k)
    {
        const x86::reg32 car = word(app, 0x5efc48 + 4 * k);
        if (car < 0x5e1120 || car >= 0x5e1120 + 16 * 0x9ac || (car - 0x5e1120) % 0x9ac != 0)
            continue;
        if (app->getMemory<uint16_t>(car + 0x164) == 0)
            continue;   // on the ground
        float w[3], f[3];
        for (int i = 0; i < 3; ++i)
        {
            w[i] = value(app, car + 0xe4 + 4 * x86::reg32(i));
            f[i] = value(app, car + 0xd8 + 4 * x86::reg32(i));
        }
        const float roll = w[0] * f[0] + w[1] * f[1] + w[2] * f[2];
        for (int i = 0; i < 3; ++i)
            put(app, car + 0xe4 + 4 * x86::reg32(i), w[i] - f[i] * roll * (1.0f - keep));
        // The same spin in the car's own axes (+0x4f8; its third, about forward), which the game turns it by too.
        put(app, car + 0x500, value(app, car + 0x500) * keep);
    }
}

/* Slipstreaming (NFS_SLIPSTREAM, 1; a number up to 3 for more, 0 or off for
 * none): a car close behind another meets less air.  Each tick, for every car
 * of the race over 80 km/h, the nearest car ahead of it -- 3 to 37.5 m along
 * its way, within 1.6 m to either side, heading the same way (their forward
 * axes within 25 degrees) and itself over 80 km/h -- gives it a push forward:
 * 0.5 m/s2 at 200 km/h when right behind, as the square of the speed and less
 * the further back and the further aside, which is about 5% more top speed
 * tucked in.  Every car, the opponents, cops and traffic too; the AI is not
 * told to look for one.  How strongly each car is in one, 0 to 1, is kept for
 * the air trails beside the player's car (native_particles.cpp). */
static float slipSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_SLIPSTREAM");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

static float s_slip[16] = {};   // each car slot's slipstream, 0 to 1

float slipstreamOf(win32::WinApplication* app, x86::reg32 car)
{
    (void)app;
    if (car < 0x5e1120 || car >= 0x5e1120 + 16 * 0x9ac || (car - 0x5e1120) % 0x9ac != 0)
        return 0.0f;
    return s_slip[(car - 0x5e1120) / 0x9ac];
}

static float speedOf(win32::WinApplication* app, x86::reg32 car)
{
    // Out of its velocity: +0xb8 is kept up only for the cars the player drives.
    const float v[3] = { value(app, car + 0xa4), value(app, car + 0xa8), value(app, car + 0xac) };
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void slipstream(win32::WinApplication* app, int ticks)
{
    const float setting = slipSetting();
    if (ticks <= 0)
        return;
    const x86::reg32 count = SDL_min(word(app, 0x5efda0), x86::reg32(16));
    x86::reg32 cars[16];
    x86::reg32 n = 0;
    for (x86::reg32 k = 0; k < count; ++k)
    {
        const x86::reg32 car = word(app, 0x5efc48 + 4 * k);
        if (car >= 0x5e1120 && car < 0x5e1120 + 16 * 0x9ac && (car - 0x5e1120) % 0x9ac == 0)
            cars[n++] = car;
    }
    const float dt = float(ticks) / 64.0f;
    static const float minKmh = SDL_getenv("NFS_SLIP_TEST") ? 10.0f : 80.0f;   // test only: from 10 km/h
    const float minSpeed = minKmh / 3.6f;
    for (x86::reg32 i = 0; i < n; ++i)
    {
        const x86::reg32 car = cars[i];
        float& slip = s_slip[(car - 0x5e1120) / 0x9ac];
        const float speed = speedOf(app, car);
        float best = 0.0f;
        if (setting > 0.0f && speed > minSpeed && app->getMemory<uint16_t>(car + 0x164) == 0)
        {
            const float p[3] = { value(app, car + 0x98), value(app, car + 0x9c), value(app, car + 0xa0) };
            const float f[3] = { value(app, car + 0xd8), value(app, car + 0xdc), value(app, car + 0xe0) };
            const float r[3] = { value(app, car + 0xc0), value(app, car + 0xc4), value(app, car + 0xc8) };
            for (x86::reg32 j = 0; j < n; ++j)
            {
                const x86::reg32 other = cars[j];
                if (other == car || speedOf(app, other) < minSpeed)
                    continue;
                const float d[3] = { value(app, other + 0x98) - p[0], value(app, other + 0x9c) - p[1],
                                     value(app, other + 0xa0) - p[2] };
                const float along = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
                const float aside = std::fabs(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
                const float facing = f[0] * value(app, other + 0xd8) + f[1] * value(app, other + 0xdc)
                                     + f[2] * value(app, other + 0xe0);
                if (along < 3.0f || along > 37.5f || aside > 1.6f || facing < 0.9f || std::fabs(d[1]) > 3.0f)
                    continue;
                const float close = 1.0f - smoothStep(3.0f, 37.5f, along);
                const float centred = 1.0f - smoothStep(0.6f, 1.6f, aside);
                best = SDL_max(best, close * centred);
            }
        }
        // Test only (NFS_SLIP_TEST): the player's car always in one, from 10 km/h.
        if (SDL_getenv("NFS_SLIP_TEST") && car == word(app, 0x5efa48) && speed > minSpeed)
            best = 1.0f;
        // Eased in and out, a quarter of a second, so the push and the trails do not flicker.
        slip += (best - slip) * SDL_min(1.0f, dt * 4.0f);
        if (SDL_getenv("NFS_SLIP_TRACE") && car == word(app, 0x5efa48))
        {
            static x86::sreg32 last = -1;
            const x86::sreg32 clock = x86::sreg32(word(app, 0x7d3684));
            if (clock / 16 != last / 16)
                SDL_Log("[SLIP] t %d speed %.1f behind %.2f slip %.2f", clock, speed, best, slip);
            last = clock;
        }
        if (slip < 1e-3f || setting <= 0.0f)
            continue;
        const float pace = speed / (200.0f / 3.6f);
        const float push = 0.5f * setting * slip * pace * pace * dt;   // m/s this tick
        const float f[3] = { value(app, car + 0xd8), value(app, car + 0xdc), value(app, car + 0xe0) };
        for (int k = 0; k < 3; k += 2)
            put(app, car + 0xa4 + 4 * x86::reg32(k), value(app, car + 0xa4 + 4 * x86::reg32(k)) + f[k] * push);
    }
}

void objectsFrame(win32::WinApplication* app);   // native_objects.cpp

void surfaceFrame(win32::WinApplication* app)
{
    objectsFrame(app);
    static x86::sreg32 last = -1;
    static x86::reg32 lastPlayer = 0;
    static float slips[16] = {};   // each car slot's sideways speed after the last tick
    const x86::reg32 player = word(app, 0x5efa48);
    const x86::sreg32 clock = x86::sreg32(word(app, 0x7d3684));
    const int ticks = last < 0 || clock < last || player != lastPlayer ? 0 : SDL_min(int(clock - last), 8);
    last = clock;
    lastPlayer = player;
    if (player && SDL_getenv("NFS_AIR_TEST"))
    {
        /* Test only: 5 s on the clock the player's car is thrown up 3 m,
         * rolling at 2 rad/s; its up axis is logged as it flies. */
        static bool thrown = false;
        if (clock < 64)
            thrown = false;
        if (!thrown && clock > 64 * 5)
        {
            thrown = true;
            put(app, player + 0x9c, value(app, player + 0x9c) + 3.0f);
            put(app, player + 0xa8, value(app, player + 0xa8) + 4.0f);
            for (int k = 0; k < 3; ++k)
                put(app, player + 0xe4 + 4 * x86::reg32(k), value(app, player + 0xd8 + 4 * x86::reg32(k)) * 2.0f);
        }
        if (ticks > 0 && clock / 8 != (clock - ticks) / 8)
            SDL_Log("[AIR] t %d air %u up %.2f %.2f %.2f roll %.3f", clock, unsigned(app->getMemory<uint16_t>(player + 0x164)),
                    value(app, player + 0xcc), value(app, player + 0xd0), value(app, player + 0xd4),
                    value(app, player + 0xe4) * value(app, player + 0xd8) + value(app, player + 0xe8) * value(app, player + 0xdc)
                        + value(app, player + 0xec) * value(app, player + 0xe0));
    }
    if (player)
    {
        airRoll(app, ticks);
        slipstream(app, ticks);
    }
    if (!player || ticks <= 0 || setting() <= 0.0f || app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return;
    static const int every = [] {
        const char* text = SDL_getenv("NFS_SURFACE_TRACE");
        return text ? SDL_max(SDL_atoi(text) > 1 ? SDL_atoi(text) : 32, 1) : 0;
    }();
    const bool trace = every && (clock / every) != ((clock - ticks) / every);
    // The race's cars, the player's among them: the list at [0x5efc48], [0x5efda0] of them.
    const x86::reg32 count = SDL_min(word(app, 0x5efda0), x86::reg32(16));
    bool playerDone = false;
    for (x86::reg32 k = 0; k < count; ++k)
    {
        const x86::reg32 car = word(app, 0x5efc48 + 4 * k);
        if (car < 0x5e1120 || car >= 0x5e1120 + 16 * 0x9ac || (car - 0x5e1120) % 0x9ac != 0)
            continue;
        const bool isPlayer = car == player;
        if (!isPlayer && !onOpponents())
            continue;
        playerDone |= isPlayer;
        feelGround(app, car, ticks, slips[(car - 0x5e1120) / 0x9ac], trace && isPlayer, clock);
    }
    if (trace)
        SDL_Log("[SURFACE] cars %u, the others %s", count, onOpponents() ? "too" : "not");
    if (!playerDone && player >= 0x5e1120 && player < 0x5e1120 + 16 * 0x9ac)
        feelGround(app, player, ticks, slips[(player - 0x5e1120) / 0x9ac], trace, clock);
}

}  // namespace nfs3hp
