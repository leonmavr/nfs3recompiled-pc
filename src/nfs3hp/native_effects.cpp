#include <nfs3hp.h>
#include "native_guest.h"
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

bool collisionEffects(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!crashHit(app, cpu, cpu.eax))
        return true;  // the game stopped in the port's crash sound
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
        s_pull = 0.0f;  // a new race, or a long stop
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
    if (chase && s_pull >= 0.001f)
        for (int k = 0; k < 3; ++k)
            place[k] = place[k] - forward[k] * s_pull + up[k] * s_pull * 0.25f;
    // A crash's shake, in any of the player's own views (native_crash.cpp).
    crashShake(app, place, right, up);
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
