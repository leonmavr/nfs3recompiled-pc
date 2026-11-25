#include <nfs3hp.h>
#include "native_guest.h"
#include "native_nitro.h"
#include <SDL3/SDL.h>

/* Port: the race's effects, made more of (tools/apply_native_effects.py).
 *
 * Sparks.  sub_42ac40 runs every frame for each car and makes what its
 * collisions throw up.  What the car touches is in [car+0x170]: 0x40000 in its
 * high word for a wall or a barrier, its material in the low byte; the speed it
 * hit at is [+0x168], its own [+0xb8], how far it is sliding [+0x15c], where
 * [+0x178..+0x180].  A material's particles are the table at 0x553740 --
 * 3 sparks (1, 2 and 4: metal, and the stone walls of Atlantica and the
 * desert's canyons are 4), 2 dust (3, 5, 7), 0 nothing (0, 6).  But the game
 * threw a burst of sparks (sub_4cac20, thirty of particle kind 3 at full
 * strength) only while scraping at over 22 m/s, and a hit made one spark
 * (sub_4cae00 of kind 3) -- so against a wall, nearly never.  Now:
 *
 * - a hit harder than 5 m/s throws a burst as strong as the hit (full at
 *   10 m/s);
 * - scraping a wall from 6 m/s throws them too, as strong as the game's own
 *   above 22 (half the car's speed);
 * - and stone and concrete that the game only dusted (2) spark as well.
 *
 * And every burst has NFS_SPARKS times as many (2 by default: the thirty at
 * [0x560ce4], kind 3's count, made sixty; 1 the game's own; 0 none of the
 * port's added bursts either).  The particles live in the game's pool of 600
 * (0x7ddfd0) and are drawn as its own are. */
namespace nfs3hp
{

namespace
{

constexpr x86::reg32 kSparkCount = 0x560ce4;   // particle kind 3's burst
constexpr x86::reg32 kSparkBurst = 0x4cac20;   // car in eax; x, y, z, strength; stdcall
constexpr x86::reg32 kMaterialParticles = 0x553740;

float sparkSetting()
{
    static const float setting = [] {
        const char* value = SDL_getenv("NFS_SPARKS");
        if (value && (SDL_strcmp(value, "off") == 0))
            return 0.0f;
        const float parsed = value && *value ? float(SDL_atof(value)) : 2.0f;
        return parsed >= 0.0f && parsed <= 6.0f ? parsed : 2.0f;
    }();
    return setting;
}

x86::reg32 asBits(float value)
{
    x86::reg32 bits;
    SDL_memcpy(&bits, &value, sizeof bits);
    return bits;
}

}  // namespace

bool crashHit(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 car);   // native_crash.cpp

/* Walls a car slides along instead of sticking to (NFS_WALL_SLIDE).
 *
 * A wall is tested for each car by sub_4304b0: the first corner of its box in
 * a wall is handed to sub_42ee10, which pushes the car out, takes the corner's
 * speed into the wall away (no bounce: the impulse only stops it) and then
 * calls sub_42e9c0 with the wall's normal.  That last one is what glued cars
 * to walls.  In the car's own frame it keeps the speed and the spin along the
 * normal and, every tick of the race's 64 a second that the car touches,
 * multiplies the rest of the spin by kGameDamping (0.958) -- the yaw included,
 * so a car on a wall all but cannot turn off it -- and every other component
 * of its velocity over kGameFast (22 m/s) by the same, a 4% bleed of the speed
 * a tick: from 40 m/s to the 22 in under half a second of scraping.
 *
 * Here, for a wall only -- the car sub_4304b0 is testing, a level normal
 * (sub_4304b0 zeroes its height) and the car upright against it, the game's
 * other branch being a surface under the car -- that step is the port's:
 * the same, but each damping takes off (1 - NFS_WALL_SLIDE) of what the
 * game's did: 0.8 by default, so the spin keeps 0.992 a tick instead of 0.958
 * and a long scrape still costs some speed.  And the car is eased off the
 * wall rather than left resting on it: kGive of the speed its centre was going
 * into the wall before the hit, at most kGiveMost m/s and nothing under
 * kGiveLeast -- a nudge, never a rebound, larger the squarer the hit.  Pushing
 * on into the wall each tick, slower than kGiveLeast, adds none, so a car
 * steered into a wall keeps against it.  0/off: the game's own.  Not in a
 * network race, where every machine must drive alike.  NFS_WALL_TRACE logs
 * each hit's nudge. */
namespace
{

constexpr x86::reg32 kRaceKind = 0x6fd3b0;   // 2: a network race
constexpr x86::reg32 kCarVelocity = 0xa4;    // three floats, the world's
constexpr x86::reg32 kCarMatrix = 0xc0;      // three rows: right, up, forward
constexpr x86::reg32 kCarSpin = 0xe4;        // three floats, the world's
constexpr float kGameDamping = 0.958f;       // [0x5537b4]
constexpr float kGameFast = 22.0f;           // [0x5537b8], m/s
constexpr float kGameUnder = 0.9f;           // [0x5373cc]: up . normal over this is a surface under the car
constexpr float kSlideDefault = 0.8f;        // NFS_WALL_SLIDE's
constexpr float kGive = 0.12f;               // of the speed into the wall it is eased off by
constexpr float kGiveMost = 1.2f;            // m/s, at most
constexpr float kGiveLeast = 1.5f;           // m/s into the wall under which nothing is given

float wallSlideSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_WALL_SLIDE");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return kSlideDefault;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 1.0f) : kSlideDefault;
    }();
    return value;
}

/* The car sub_4304b0 is testing, and its velocity before the wall acted. */
x86::reg32 s_wallCar = 0;
float s_wallBefore[3] = {};

}  // namespace

/* sub_4304b0 (eax: the car), its top: noted, and the game's test runs. */
bool wallTest(win32::WinApplication* app, x86::CPU& cpu)
{
    s_wallCar = 0;
    if (wallSlideSetting() <= 0.0f || app->getMemory<x86::reg32>(kRaceKind) == 2)
        return false;
    s_wallCar = cpu.eax;
    for (int k = 0; k < 3; ++k)
        s_wallBefore[k] = app->getMemory<float>(cpu.eax + kCarVelocity + 4 * k);
    return false;
}

/* sub_42e9c0 (eax: the car, edx: the normal, one float on the stack): for a
 * wall, the port's (above); anything else is the game's. */
bool wallFriction(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 car = cpu.eax;
    if (!car || car != s_wallCar)
        return false;
    s_wallCar = 0;
    const float slide = wallSlideSetting();
    float n[3], up[3];
    for (int k = 0; k < 3; ++k)
    {
        n[k] = app->getMemory<float>(cpu.edx + 4 * k);
        up[k] = app->getMemory<float>(car + kCarMatrix + 12 + 4 * k);
    }
    if (slide <= 0.0f || n[1] != 0.0f || n[0] * up[0] + n[1] * up[1] + n[2] * up[2] > kGameUnder)
        return false;
    float m[3][3], v[3], w[3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = app->getMemory<float>(car + kCarMatrix + 12 * r + 4 * c);
    for (int k = 0; k < 3; ++k)
    {
        v[k] = app->getMemory<float>(car + kCarVelocity + 4 * k);
        w[k] = app->getMemory<float>(car + kCarSpin + 4 * k);
    }
    // Into the car's frame, as the game does: the matrix times each.
    auto local = [&](const float a[3], float out[3]) {
        for (int r = 0; r < 3; ++r)
            out[r] = m[r][0] * a[0] + m[r][1] * a[1] + m[r][2] * a[2];
    };
    auto world = [&](const float a[3], float out[3]) {
        for (int c = 0; c < 3; ++c)
            out[c] = a[0] * m[0][c] + a[1] * m[1][c] + a[2] * m[2][c];
    };
    float vl[3], wl[3], nl[3];
    local(v, vl);
    local(w, wl);
    local(n, nl);
    const float damping = 1.0f - (1.0f - kGameDamping) * (1.0f - slide);
    const float vn = vl[0] * nl[0] + vl[1] * nl[1] + vl[2] * nl[2];
    const float wn = wl[0] * nl[0] + wl[1] * nl[1] + wl[2] * nl[2];
    for (int k = 0; k < 3; ++k)
    {
        float tangent = vl[k] - nl[k] * vn;
        if (SDL_fabsf(tangent) > kGameFast)
            tangent *= damping;
        vl[k] = tangent + nl[k] * vn;
        wl[k] = (wl[k] - nl[k] * wn) * damping + nl[k] * wn;
    }
    world(vl, v);
    world(wl, w);
    // Eased off the wall: by how fast its centre was going into it.
    const float into = -(s_wallBefore[0] * n[0] + s_wallBefore[1] * n[1] + s_wallBefore[2] * n[2]);
    if (into > kGiveLeast)
    {
        const float give = SDL_min(kGive * into, kGiveMost) * slide / kSlideDefault;
        const float away = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        const float add = give - SDL_clamp(away, 0.0f, give);
        if (add > 0.0f)
            for (int k = 0; k < 3; ++k)
                v[k] += n[k] * add;
        if (SDL_getenv("NFS_WALL_TRACE"))
            SDL_Log("[WALL] car %08x into the wall at %.1f m/s, eased off by %.2f", unsigned(car), double(into),
                    double(add));
    }
    for (int k = 0; k < 3; ++k)
    {
        app->getMemory<float>(car + kCarVelocity + 4 * k) = v[k];
        app->getMemory<float>(car + kCarSpin + 4 * k) = w[k];
    }
    cpu.esp += 4 + 4;   // ret 4: the return address and the hit's speed
    return true;
}

bool skidPedals(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 car);   // native_skids.cpp

bool collisionEffects(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!crashHit(app, cpu, cpu.eax))
        return true;  // the game stopped in the port's crash sound
    if (!skidPedals(app, cpu, cpu.eax))
        return true;  // the game stopped laying the port's tyre mark
    const float sparks = sparkSetting();
    {
        // The burst's size, once: kind 3's count as the game has it, times NFS_SPARKS.
        static x86::reg32 original = 0;
        const x86::reg32 count = app->getMemory<x86::reg32>(kSparkCount);
        if (!original && count > 0 && count < 1000)
            original = count;
        if (original)
        {
            const x86::reg32 wanted = x86::reg32(SDL_max(1.0f, float(original) * SDL_max(sparks, 1.0f)) + 0.5f);
            if (count != wanted)
                app->getMemory<x86::reg32>(kSparkCount) = wanted;
        }
    }
    if (sparks <= 0.0f)
        return false;
    const x86::reg32 car = cpu.eax;
    auto word = [&](x86::reg32 a) { return x86::reg32(app->getMemory<x86::reg32>(a)); };
    auto value = [&](x86::reg32 a) { return app->getMemory<float>(a); };
    const x86::reg32 touching = word(car + 0x170);
    if ((touching & 0xefff0000u) != 0x40000u)
        return false;
    const x86::reg32 material = touching & 0xff;
    if (material >= 8)
        return false;
    const x86::reg32 particles = word(kMaterialParticles + material * 4);
    if (particles != 3 && particles != 2)
        return false;
    const float hit = value(car + 0x168);
    const float speed = value(car + 0xb8);
    const float slide = value(car + 0x15c);
    if (!(hit > 0.0f))
        return false;
    float strength = 0.0f;
    if (hit > 5.0f)
        strength = SDL_min(hit * 2.0f, 20.0f);                       // a hit
    if (slide < 3.0f && speed >= 6.0f && (speed < 22.0f || particles != 3))
        strength = SDL_max(strength, speed * 0.5f);                  // scraping along it
    if (strength <= 0.0f)
        return false;
    /* The burst flies off with the car's velocity ([+0xa4], spread by up to
     * 3 m/s): into the wall, behind it, out of sight.  So for the port's own
     * bursts the car's velocity is bounced off the wall first -- the wall's
     * normal level, from the car's centre ([+0x98]) to where it touches --
     * and handed back after: what was going into the wall comes off it at
     * half the speed, what ran along it runs on, and a little up. */
    float velocity[3];
    for (int k = 0; k < 3; ++k)
        velocity[k] = value(car + 0xa4 + 4 * k);
    float normal[3] = { value(car + 0x178) - value(car + 0x98), 0.0f, value(car + 0x180) - value(car + 0xa0) };
    const float length = SDL_sqrtf(normal[0] * normal[0] + normal[2] * normal[2]);
    float bounced[3] = { velocity[0], velocity[1] + 2.0f, velocity[2] };
    if (length > 1e-3f)
    {
        normal[0] /= length;
        normal[2] /= length;
        const float into = velocity[0] * normal[0] + velocity[2] * normal[2];
        if (into > 0.0f)
        {
            bounced[0] -= 1.5f * into * normal[0];
            bounced[2] -= 1.5f * into * normal[2];
        }
    }
    for (int k = 0; k < 3; ++k)
        app->getMemory<float>(car + 0xa4 + 4 * k) = bounced[k];
    const x86::reg32 saved = cpu.eax;
    cpu.eax = car;
    const bool ran = guest::call(app, cpu, kSparkBurst,
                                 { word(car + 0x178), word(car + 0x17c), word(car + 0x180), asBits(strength) });
    for (int k = 0; k < 3; ++k)
        app->getMemory<float>(car + 0xa4 + 4 * k) = velocity[k];
    if (!ran)
        return true;  // the game stopped
    cpu.eax = saved;
    return false;
}

/* Speed you can feel, 1: the chase camera drawn back a little under the
 * throttle (NFS_CAMERA_PULL, how far: 1.2 m by default, 0 or off for none).
 *
 * sub_422850 turns a camera -- its place at eax, its axes at eax+0xc, a row
 * each: right, up and the way it looks -- into the view's matrix; before it
 * does, for the first player's main view ([view] 0, [view+4] 0, view at
 * eax-8) when the camera is a chase camera -- 3 to 15 m from the car it
 * follows ([0x5efa48], +0x98) and looking towards it -- its place is moved
 * back along the way it looks, and up a quarter of that.  How far eases
 * towards the throttle's share of NFS_CAMERA_PULL (the first player's
 * throttle byte, 0x79c339, 0 to 255), out over about a second and in over
 * half of one, by the race's own clock ([0x7d3684], 64 a second), so a
 * replay or a pause holds it still.  Everything drawn after is drawn from
 * there: the matrix, the culling and the copy of the view the game keeps. */
float pullSetting()
{
    static const float setting = [] {
        const char* value = SDL_getenv("NFS_CAMERA_PULL");
        if (value && SDL_strcmp(value, "off") == 0)
            return 0.0f;
        if (value && SDL_strcmp(value, "on") == 0)
            return 1.2f;
        const float parsed = value && *value ? float(SDL_atof(value)) : 1.2f;
        return parsed >= 0.0f && parsed <= 6.0f ? parsed : 1.2f;
    }();
    return setting;
}

void crashShake(win32::WinApplication* app, float place[3], const float right[3], const float up[3]);   // native_crash.cpp

bool raceIsRunning();   // nfs3hp_main.cpp

/* Speed you can feel, 3: the view widens at speed (NFS_SPEED_FOV, how much:
 * 1 by default, 0 or off for none, up to 3).
 *
 * From 150 km/h to 300 the first player's chase view opens up to a tenth wider
 * in the tangent of its angle -- about three degrees either side of a
 * 45-degree half angle -- times the switch, eased in over about 0.6 s and out
 * over 0.8 s by the race's clock, so a pause or a replay's stop holds it.
 * cameraToView works out how much (s_speedFov, a factor on the tangents);
 * projectionSet puts it into the float projection (sub_4bf260) as it is made
 * for that view, by both of its angles, so the picture keeps its proportions.
 * The in-car and bumper views ease back to none; the replay's television
 * cameras take the zoomed projection (no angle) and are never touched. */
float speedFovSetting()
{
    static const float setting = [] {
        const char* value = SDL_getenv("NFS_SPEED_FOV");
        if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(value));
        return parsed >= 0.0f && parsed <= 3.0f ? parsed : 1.0f;
    }();
    return setting;
}

namespace
{
constexpr float kSpeedFovFrom = 150.0f / 3.6f;   // m/s: none below
constexpr float kSpeedFovFull = 300.0f / 3.6f;   // m/s: the whole of it from here
constexpr float kSpeedFovMost = 0.10f;           // the tangents' share added at full speed, NFS_SPEED_FOV 1
float s_speedFov = 1.0f;                         // the factor on the tangents now
}

/* The share of the speed-widened view, 0 to 1: 150 km/h to 300, smoothstepped. */
static float speedFovShare(float speed)
{
    const float t = SDL_clamp((speed - kSpeedFovFrom) / (kSpeedFovFull - kSpeedFovFrom), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* sub_4bf260, the float projection made from a view's rectangle and its two
 * half angles in degrees (on the stack, after the return address: +4 and +8
 * the origin, +0xc and +0x10 the view's width and height, +0x14 and +0x18 the
 * horizontal and vertical half angles, +0x1c a flip).  For the race's main
 * view -- one player on the screen, a view nearly as wide as it, angles not
 * zero -- each angle becomes the one whose tangent is s_speedFov times its own. */
bool projectionSet(win32::WinApplication* app, x86::CPU& cpu)
{
    /* And the nitro's pull-back on top of the speed's widening (nitroPull,
     * native_nitro.cpp): kicked out as the bottle lights, held a little wide
     * while it burns, back to none after. */
    const float fov = s_speedFov * nfs3hp::nitroPull();
    if (fov <= 1.0001f || !raceIsRunning() || app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return false;
    const x86::reg32 args = cpu.esp;
    const float width = app->getMemory<float>(args + 0xc);
    const float screen = float(app->getMemory<x86::reg32>(0x7cdae0));
    if (!(width >= 0.75f * screen))
        return false;   // the mirror, or a view of its own
    for (const x86::reg32 at : { args + 0x14, args + 0x18 })
    {
        const float angle = app->getMemory<float>(at);
        if (!(angle > 1.0f && angle < 80.0f))
            return false;
    }
    for (const x86::reg32 at : { args + 0x14, args + 0x18 })
    {
        const float angle = app->getMemory<float>(at);
        const double wide = SDL_atan(SDL_tan(double(angle) * SDL_PI_D / 180.0) * double(fov)) * 180.0 / SDL_PI_D;
        app->getMemory<float>(at) = float(SDL_min(wide, 85.0));
    }
    return false;
}

/* Speed you can feel, 4: the camera's bumps at speed (NFS_SPEED_SHAKE, how
 * big: 2.5 by default, 0 or off for none, up to 5; NFS_SPEED_SHAKE_RATE, how
 * quick: 3.5, from 0.25 to 5 -- both picked by eye, 2026-11-19).
 *
 * Not a steady wobble: now and then -- a quiet gap of 0.5 to 1.6 s, then one
 * smooth bump, up and back down through one turn of a sine under a soft
 * window -- the way a fast car's body takes a seam in the road.  From 150 km/h
 * to 300 (held there) the bumps grow from 1.2 cm to twice that, and come 1.5
 * times as quick (shorter, with shorter gaps) -- each times its switch.  A shift up above 150 km/h
 * throws one at once, twice the size.  The first player's own views only (the
 * chase and the in-car), by the race's clock, so a pause or a replay's stop
 * holds it; the camera is moved along its own up. */
namespace speedshake
{
constexpr float kFrom = 150.0f / 3.6f;    // m/s: none below
constexpr float kFull = 300.0f / 3.6f;    // m/s: all of it from here
constexpr float kSize = 0.012f;           // m: a bump at 150 km/h, twice at 300
constexpr float kLength = 0.22f;          // s: a bump at 150 km/h, over 1.5 at 300
constexpr float kGapLeast = 0.5f;         // s: between two bumps, at 150 km/h
constexpr float kGapMost = 1.6f;
constexpr float kShift = 2.0f;            // a shift up's bump, times an ordinary one
}  // namespace speedshake

static float shakeSetting(const char* name, float otherwise, float least, float most)
{
    const char* value = SDL_getenv(name);
    if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
        return otherwise;
    if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
        return 0.0f;
    const float parsed = float(SDL_atof(value));
    return parsed >= least && parsed <= most ? parsed : otherwise;
}

/* How big a bump is at this car's speed, m: `size` times kSize, and half again
 * from kFrom to kFull.  The nitro's rumble is the same measure. */
static float shakeHeight(win32::WinApplication* app, x86::reg32 car, float size)
{
    const float speed = SDL_fabsf(app->getMemory<float>(car + 0xb8));
    const float share = speed == speed
                            ? SDL_clamp((speed - speedshake::kFrom) / (speedshake::kFull - speedshake::kFrom), 0.0f, 1.0f)
                            : 0.0f;
    return size * speedshake::kSize * (1.0f + share);
}

/* Speed you can feel, 4, continued: how far the camera is moved up (down when
 * less than 0) this frame; `passed` the race's ticks (64 a second) since the
 * last frame, < 0 or over 64 for a new race. */
static float speedShake(win32::WinApplication* app, x86::reg32 car, x86::sreg32 passed)
{
    static const float size = shakeSetting("NFS_SPEED_SHAKE", 2.5f, 0.0f, 5.0f);
    static const float rate = shakeSetting("NFS_SPEED_SHAKE_RATE", 3.5f, 0.25f, 5.0f);
    static float s_gap = 1.0f;      // s until the next bump
    static float s_age = -1.0f;     // s into the bump going, or < 0 for none
    static float s_height = 0.0f;   // its size, m
    static float s_length = 0.2f;   // and how long it lasts, s
    static x86::reg32 s_gear = 0;
    if (passed < 0 || passed > 64)
    {
        s_age = -1.0f;
        s_gap = 1.0f;
        s_gear = 0;
        return 0.0f;
    }
    if (size <= 0.0f || rate <= 0.0f)
        return 0.0f;
    const float dt = float(passed) / 64.0f;
    const float speed = SDL_fabsf(app->getMemory<float>(car + 0xb8));
    const float share = speed == speed ? SDL_clamp((speed - speedshake::kFrom) / (speedshake::kFull - speedshake::kFrom), 0.0f, 1.0f) : 0.0f;
    const bool fast = speed >= speedshake::kFrom;
    const float quick = rate * (1.0f + 0.5f * share);
    const float height = shakeHeight(app, car, size);
    const x86::reg32 gear = app->getMemory<x86::reg8>(car + 0x50b);
    const bool shifted = s_gear >= 1 && s_gear <= 0x63 && gear > s_gear && gear >= 2;
    s_gear = gear;
    if (fast && shifted)
    {
        s_age = 0.0f;   // at once, whatever was going
        s_height = height * speedshake::kShift;
        s_length = speedshake::kLength / quick * 1.2f;
    }
    else if (s_age < 0.0f && dt > 0.0f)
    {
        s_gap -= dt;
        if (s_gap <= 0.0f)
        {
            s_gap = (speedshake::kGapLeast + SDL_randf() * (speedshake::kGapMost - speedshake::kGapLeast)) / quick;
            if (fast)
            {
                s_age = 0.0f;
                s_height = height * (0.6f + 0.4f * SDL_randf());
                s_length = speedshake::kLength / quick;
            }
        }
    }
    if (s_age < 0.0f)
        return 0.0f;
    s_age += dt;
    const float u = s_age / s_length;
    if (u >= 1.0f)
    {
        s_age = -1.0f;
        return 0.0f;
    }
    const float window = SDL_sinf(SDL_PI_F * u);   // soft at both ends
    return s_height * SDL_sinf(2.0f * SDL_PI_F * u) * window;
}

/* Speed you can feel, 5: the bottle of nitro's rumble (NFS_NITRO_SHAKE, how
 * big: 1.5 -- against the bumps at speed, so a bottle burning shakes the camera
 * half again as hard as a 150 km/h car bumps it, and up to 5; 0 or off for
 * none.  NFS_NITRO_SHAKE_RATE, how quick the rumble is: 9 a second, from 1 to
 * 30 -- a shaking of the body rather than a bump).
 *
 * Not a bump now and then but a steady up and down for as long as it burns,
 * one sine a turn -- the engine's own vibration through the camera -- eased in
 * and out with the burn (nitroBurning), and by the race's clock, so a pause
 * holds it.  Its size follows the bumps' own by the speed of the car, so a car
 * already over 150 km/h shakes half again as much as one getting up to speed;
 * a standing car still rumbles, which is where the bottle is worth the most. */
namespace nitrorumble
{
constexpr float kSize = 1.5f;      // times the bumps at speed's own size, NFS_NITRO_SHAKE
constexpr float kRate = 9.0f;      // a second, NFS_NITRO_SHAKE_RATE
}  // namespace nitrorumble

/* How far the camera is moved up (down when less than 0) by the bottle this
 * frame; 0 when it is not burning. */
static float nitroRumble(win32::WinApplication* app, x86::reg32 car, x86::sreg32 tick)
{
    static const float size = shakeSetting("NFS_SPEED_SHAKE", 2.5f, 0.0f, 5.0f);   // the bumps' own
    static const float share = shakeSetting("NFS_NITRO_SHAKE", nitrorumble::kSize, 0.0f, 5.0f);
    static const float rate = shakeSetting("NFS_NITRO_SHAKE_RATE", nitrorumble::kRate, 1.0f, 30.0f);
    const float burning = nitroBurning();
    if (size <= 0.0f || share <= 0.0f || rate <= 0.0f || burning <= 0.001f || tick < 0)
        return 0.0f;
    const float height = shakeHeight(app, car, size) * share;
    const float phase = float(tick) / 64.0f * rate * 2.0f * SDL_PI_F;
    return height * burning * SDL_sinf(phase);
}

bool cameraToView(win32::WinApplication* app, x86::CPU& cpu)
{
    const float most = pullSetting();
    const x86::reg32 camera = cpu.eax;
    const x86::reg32 view = camera - 8;
    auto word = [&](x86::reg32 a) { return x86::reg32(app->getMemory<x86::reg32>(a)); };
    auto value = [&](x86::reg32 a) { return app->getMemory<float>(a); };
    if (word(view) != 0 || word(view + 4) != 0 || app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return false;
    const x86::reg32 car = word(0x5efa48);
    if (!car)
        return false;
    static float s_pull = 0.0f;
    static x86::sreg32 s_tick = 0;
    const x86::sreg32 tick = x86::sreg32(word(0x7d3684));
    const x86::sreg32 passed = tick - s_tick;
    s_tick = tick;
    if (passed < 0 || passed > 64)
    {
        s_pull = 0.0f;  // a new race, or a long stop
        s_speedFov = 1.0f;
    }
    else if (passed > 0)
    {
        const float throttle = float(app->getMemory<x86::reg8>(0x79c339)) / 255.0f;
        const float wanted = most * throttle;
        const float seconds = wanted > s_pull ? 0.35f : 0.2f;  // time constants: out ~1 s, in ~0.6 s
        const float step = 1.0f - SDL_expf(-float(passed) / (64.0f * seconds));
        s_pull += (wanted - s_pull) * step;
    }
    float place[3], right[3], forward[3], up[3], toCar[3];
    for (int k = 0; k < 3; ++k)
    {
        place[k] = value(camera + 4 * k);
        right[k] = value(camera + 0xc + 4 * k);
        up[k] = value(camera + 0xc + 0xc + 4 * k);
        forward[k] = value(camera + 0xc + 0x18 + 4 * k);
        toCar[k] = value(car + 0x98 + 4 * k) - place[k];
    }
    const float distance = SDL_sqrtf(toCar[0] * toCar[0] + toCar[1] * toCar[1] + toCar[2] * toCar[2]);
    const float ahead = toCar[0] * forward[0] + toCar[1] * forward[1] + toCar[2] * forward[2];
    if (distance > 15.0f || (distance >= 3.0f && ahead < 0.7f * distance))
        return false;  // a television camera of the replay
    const bool chase = distance >= 3.0f;   // not in the car or on its bumper
    if (passed > 0 && passed <= 64)
    {
        const float speed = SDL_fabsf(value(car + 0xb8));
        const float share = chase && speed == speed ? speedFovShare(speed) : 0.0f;
        const float wanted = 1.0f + kSpeedFovMost * speedFovSetting() * share;
        const float seconds = wanted > s_speedFov ? 0.2f : 0.27f;   // in ~0.6 s, out ~0.8 s
        s_speedFov += (wanted - s_speedFov) * (1.0f - SDL_expf(-float(passed) / (64.0f * seconds)));
    }
    if (chase && s_pull >= 0.001f)
        for (int k = 0; k < 3; ++k)
            place[k] = place[k] - forward[k] * s_pull + up[k] * s_pull * 0.25f;
    // A crash's shake, in any of the player's own views (native_crash.cpp).
    crashShake(app, place, right, up);
    // The bumps at speed (speedShake), and the bottle of nitro's rumble under them.
    const float rumble = nitroRumble(app, car, tick);
    /* NFS_NITRO_SHAKE_TRACE: how far the rumble has swung each second, and at
     * what speed -- what to tune the two switches by. */
    if (SDL_getenv("NFS_NITRO_SHAKE_TRACE"))
    {
        static float s_least = 0.0f, s_most = 0.0f;
        static x86::sreg32 s_said = -1000;
        s_least = SDL_min(s_least, rumble);
        s_most = SDL_max(s_most, rumble);
        if (tick - s_said >= 64 || tick < s_said)
        {
            s_said = tick;
            if (s_most > 0.0f || s_least < 0.0f)
                SDL_Log("[NITRO] the rumble %.1f to %.1f cm at %.0f m/s", double(s_least * 100.0f),
                        double(s_most * 100.0f), double(SDL_fabsf(app->getMemory<float>(car + 0xb8))));
            s_least = s_most = 0.0f;
        }
    }
    const float bump = speedShake(app, car, passed) + rumble;
    for (int k = 0; k < 3; ++k)
        place[k] += up[k] * bump;
    for (int k = 0; k < 3; ++k)
        app->getMemory<float>(camera + 4 * k) = place[k];
    return false;
}

// native_particles.cpp: dust and sparks of the port's own.
bool particleTakeOver(win32::WinApplication* app, x86::CPU& cpu);

/* sub_4ca850, a particle made: the kinds the port draws itself are handed to
 * native_particles.cpp (particleTakeOver), the rest made as the game makes them. */
bool particleSpawn(win32::WinApplication* app, x86::CPU& cpu)
{
    return particleTakeOver(app, cpu);
}

}  // namespace nfs3hp
