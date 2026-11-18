#include <nfs3hp.h>
#include "native_guest.h"
#include "native_exhausts.h"
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

/* Port: the race's dust, grit and sparks, the port's own (NFS_PARTICLES).
 *
 * The game makes a particle with sub_4ca850 (its kind in eax, thirteen
 * arguments, ret 0x34: where, three floats; a scale; two directions of three;
 * how fast, three) into its pool of 600 (0x7ddfd0) and draws each as a
 * picture from gamedata/render/pc/sfx.fsh: kind 2 the dust and dirt thrown up
 * off the road (grey, the picture's colour), kind 5 what comes off grass,
 * kind 3 a spark of a collision (thirty to a burst).  particleSpawn
 * (native_effects.cpp, hooked at its top) hands those kinds -- and kind 10,
 * the spray off a wet road -- here instead,
 * and the game's are not made:
 *
 * - The dirt becomes grains -- small clumps with ragged edges, thrown up and
 *   out, falling back under gravity, settling where they land -- and now and
 *   then a puff of fine dust that hangs, spreads and fades.  How many to each
 *   of the game's grows with the speed it was thrown at (1.5 and one more for
 *   every 4 m/s, times NFS_PARTICLES), and how high and far they go with it.
 *   Their colour is the ground's they came from: the renderer takes it from
 *   the picture where they were thrown (sand on a desert, snow on snow, mud).
 * - Off grass the grains are blades: long, thin and a little bent, tumbling
 *   as they flutter down (EffectParticle::turn), a greener shade of the
 *   ground -- and grains after all where that ground is not green.
 * - Spray off a wet road (kind 10) becomes a pale mist billowing out behind
 *   the wheels and fine drops thrown up, both more the faster (throwSpray).
 * - The smoke off a wheel spinning or locked (kind 6) becomes the port's own
 *   (throwSmoke, NFS_TYRE_SMOKE): dark clouds of its own colour that rise and
 *   spread -- no gravity, no ground to settle on -- and are gone in a second or
 *   two.  Off a wheel on grass that kind is still grass thrown up instead.
 * - A spark is a short white-hot streak that cools through yellow and orange
 *   to red as it falls, thrown at the game's speed with a little scatter.
 *
 * Each frame (as the main view's sky is drawn, its view known: particlesFrame)
 * they are moved by the race's clock -- standing with it -- turned into the
 * view and handed to the renderer (glide2x::effectParticles), which draws them
 * over the world before its whole-picture pass (ThrashRenderer::drawParticles),
 * each one's shape made from its seed rather than a picture.  Not in split
 * screen, where there is more than one view, nor without that renderer: the
 * game's own then.
 *
 * And the exhaust flames (NFS_EXHAUST_FLAMES, native_exhausts.cpp): a car that
 * shifts up throws one out of each exhaust it has marked, a second long -- grown
 * to its size in a fifth of it, trembling there, then out -- in the colour the
 * car's exhausts.json names, #f2380a where it names none.  A car revving at a
 * standstill -- at the start, throttle down, the engine at the top of its revs
 * -- pops them out one exhaust at a time instead, every quarter of a second or
 * so.  A hard downshift, and lifting off at the top of the revs, pop half a
 * one instead (popFlame) -- a racing car's overrun crackle.  Every car of the
 * race, the opponents' too, the player's; a car with no exhausts marked has
 * none.  Each throws a pool of its light on the road under the car
 * (EffectParticle::Glow, NFS_FLAME_LIGHT), coming up and going out with it. */
namespace nfs3hp
{

/* Whether the track it names rains or snows (nfs3hp_main.cpp, the weather's
 * "type" in its 3trNNw.hrz: 1 rain, 2 snow). */
bool trackRains(x86::reg32 track);
void wheelContact(win32::WinApplication* app, x86::reg32 car, int wheel, float at[3]);   // below

namespace
{

struct Particle
{
    float p[3], v[3];
    float origin[3];       // where it was thrown from, for the ground's colour
    float floor;           // the height it settles at
    float age, life;
    float size, grow;      // metres; per second
    float seed;
    float alpha;
    float colour[3];
    float last[2];
    bool hasLast;
    bool settled;
    bool noGround;         // its own colour, not the ground's (spray)
    int type;              // EffectParticle::Type
};

constexpr std::size_t kMost = 3600;
std::vector<Particle> s_particles;
/* How many flakes and drops are in the air: a storm makes a great many, and
 * they are not to crowd the dust, the smoke and the sparks out of the pool. */
std::size_t s_weatherCount = 0;
constexpr std::size_t kMostWeather = 2200;   // enough for the heaviest shower (NFS_WEATHER_MAX)

/* An exhaust flame: which of the car's exhausts it burns at, in the car's own
 * space, and how far through its second of life it is.  It does not move --
 * not on the car, and not with the road either: it stays at the pipe's mouth it
 * was lit at as the car goes, growing, trembling and going out there. */
struct Flame
{
    x86::reg32 car;    // the car it burns on, its exhausts moving with it
    float at[3];       // the mouth of the exhaust in that car's space, a little out of it
    float age, life;   // seconds; a second
    float size;        // the radius it grows to, metres
    float light;       // how much light it throws on the road, 0 to 1
    float seed;
    float colour[3];
};
constexpr std::size_t kMostFlames = 96;
std::vector<Flame> s_flames;
x86::reg32 s_gear[16];   // each car's gear, as last seen: a shift up is a flame
float s_pop[16];         // seconds until a car revving at rest pops its next flame
float s_crackle[16];     // seconds until an engine on the overrun pops its next
float s_revs[16];        // each car's revs a frame ago: rising fast is being revved
bool s_revsKnown[16];
float s_saidRevs[16];    // the revs NFS_EXHAUSTS_TRACE last said of a car at rest
unsigned s_traced[16];   // what NFS_EXHAUSTS_TRACE has said of each: 1 a shift up, 2 a rev
std::uint32_t s_random = 0x9e3779b9u;
x86::sreg32 s_clock = -1;

float random01()
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return float(s_random >> 8) / 16777216.0f;
}

float smoothStep(float edge0, float edge1, float x)
{
    const float t = SDL_max(0.0f, SDL_min(1.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

float randomRange(float lo, float hi)
{
    return lo + (hi - lo) * random01();
}

float setting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_PARTICLES");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

/* The smoke off a wheel spinning or locked (the game's kind 6, throwSmoke):
 * NFS_TYRE_SMOKE, on; a number up to 3 for more and thicker, 0 or off for the
 * game's. */
float smokeSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_TYRE_SMOKE");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

/* The weather's own flakes and drops (kinds 7 and 8, throwWeather): what falls
 * is the port's, NFS_WEATHER_PARTICLES on; the game's pictures there are squares
 * cut out of nothing (TODOs.md), a translucent box round every flake. */
bool weatherSetting()
{
    static const bool value = [] {
        const char* text = SDL_getenv("NFS_WEATHER_PARTICLES");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return value;
}

/* The wheels' spray off a wet road (kind 10, throwSpray): NFS_WHEEL_SPRAY, on;
 * 0 or off for none.  It was off while the game's own spray still showed as
 * turned squares; the game's sprites are no longer drawn at all
 * (gameSprites()), so the port's is what a wet road throws. */
bool spraySetting()
{
    static const bool value = [] {
        const char* text = SDL_getenv("NFS_WHEEL_SPRAY");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return value;
}

float argument(win32::WinApplication* app, x86::CPU& cpu, int k)
{
    return app->getMemory<float>(cpu.esp + 4 + 4 * x86::reg32(k));
}

/* NFS_PERF_TRACE: what the particles cost, counted over a second. */
struct ParticleCost
{
    unsigned frames = 0, added = 0, replaced = 0;
    std::size_t most = 0;
    double frameMs = 0.0, mirrorMs = 0.0;
} s_cost;

bool perfTracing()
{
    static const bool on = SDL_getenv("NFS_PERF_TRACE") != nullptr;
    return on;
}

/* A particle into the pool; a full one gives up a particle in turn, in place
 * (2026-11-17: the oldest was erased from the front, every particle after it
 * moved down a place -- 300 KB moved for each one added once the rain's spray
 * filled the pool, hundreds a frame: the rain's lag). */
std::size_t s_replace = 0;   // the next to give up its place in a full pool

void add(const Particle& particle)
{
    ++s_cost.added;
    if (s_particles.size() >= kMost)
    {
        ++s_cost.replaced;
        s_replace %= s_particles.size();
        s_particles[s_replace++] = particle;
        return;
    }
    s_particles.reserve(kMost);
    s_particles.push_back(particle);
}

void throwDirt(const float at[3], const float velocity[3], bool grass, int most = 24, bool dust = true)
{
    const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    const float pace = SDL_min(speed / 30.0f, 1.5f);
    const int grains = int((2.0f + speed / 3.0f) * (grass ? 1.5f : 1.0f) * setting() + random01());
    for (int k = 0; k < SDL_min(grains, most); ++k)
    {
        Particle g{};
        for (int i = 0; i < 3; ++i)
        {
            g.p[i] = at[i] + randomRange(-0.25f, 0.25f);
            g.origin[i] = at[i];
        }
        g.p[1] = at[1] + randomRange(0.0f, 0.15f);
        const float out = randomRange(0.6f, 2.2f) * (0.5f + pace);
        const float turn = randomRange(0.0f, 6.2832f);
        g.v[0] = velocity[0] * randomRange(0.35f, 0.7f) + std::cos(turn) * out;
        g.v[2] = velocity[2] * randomRange(0.35f, 0.7f) + std::sin(turn) * out;
        g.v[1] = randomRange(1.2f, 3.5f) * (0.6f + pace);
        g.floor = at[1] - 0.05f;
        g.life = randomRange(0.7f, 1.5f);
        g.size = grass ? randomRange(0.022f, 0.045f) : randomRange(0.015f, 0.045f) * (random01() < 0.12f ? 1.8f : 1.0f);
        g.seed = random01();
        g.alpha = 1.0f;
        g.colour[0] = 0.62f; g.colour[1] = 0.52f; g.colour[2] = 0.40f;
        g.type = grass ? EffectParticle::Blade : EffectParticle::Grain;
        add(g);
    }
    // Fine dust: more of it the faster.
    if (dust && random01() < (grass ? 0.1f + speed / 120.0f : 0.45f + speed / 30.0f) * setting())
    {
        Particle d{};
        for (int i = 0; i < 3; ++i)
        {
            d.p[i] = at[i] + randomRange(-0.3f, 0.3f);
            d.origin[i] = at[i];
        }
        d.p[1] = at[1] + 0.12f;
        d.v[0] = velocity[0] * randomRange(0.2f, 0.45f) + randomRange(-0.6f, 0.6f);
        d.v[2] = velocity[2] * randomRange(0.2f, 0.45f) + randomRange(-0.6f, 0.6f);
        d.v[1] = randomRange(0.1f, 0.55f) * (0.5f + pace);
        d.floor = at[1] - 0.05f;
        d.life = randomRange(1.2f, 2.4f);
        d.size = randomRange(0.35f, 0.65f);
        d.grow = randomRange(0.9f, 1.6f) * (0.6f + pace);
        d.seed = random01();
        d.alpha = randomRange(0.35f, 0.6f);
        d.colour[0] = 0.7f; d.colour[1] = 0.63f; d.colour[2] = 0.52f;
        d.type = EffectParticle::Puff;
        add(d);
    }
}

/* Spray off a wet road (kind 10): a pale mist that billows out behind the
 * wheel and hangs a moment, and fine drops thrown up and falling back; more
 * of both the faster the car it comes from (the nearest one, of the 16 at
 * 0x5e1120) goes.  Its colour is its own -- water, not the ground's (tx -1). */
void throwSpray(win32::WinApplication* app, const float at[3], const float velocity[3])
{
    float speed = 0.0f, carVelocity[3] = { velocity[0], velocity[1], velocity[2] };
    float nearest = 4.0f * 4.0f;
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        const float d[3] = { app->getMemory<float>(car + 0x98) - at[0], app->getMemory<float>(car + 0x9c) - at[1],
                             app->getMemory<float>(car + 0xa0) - at[2] };
        const float distance = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (distance < nearest)
        {
            nearest = distance;
            for (int i = 0; i < 3; ++i)
                carVelocity[i] = app->getMemory<float>(car + 0xa4 + x86::reg32(i) * 4);
            speed = std::sqrt(carVelocity[0] * carVelocity[0] + carVelocity[1] * carVelocity[1] + carVelocity[2] * carVelocity[2]);
        }
    }
    if (speed < 4.0f)
        return;
    const float pace = SDL_min(speed / 40.0f, 1.5f);
    if (random01() < (0.35f + pace * 0.6f) * setting())
    {
        Particle m{};
        for (int i = 0; i < 3; ++i)
            m.p[i] = at[i] + randomRange(-0.25f, 0.25f);
        m.p[1] = at[1] + randomRange(0.05f, 0.25f);
        for (int i = 0; i < 3; i += 2)
            m.v[i] = carVelocity[i] * randomRange(0.45f, 0.75f) + randomRange(-1.2f, 1.2f) * (0.5f + pace);
        m.v[1] = randomRange(0.3f, 1.2f) * (0.4f + pace);
        m.floor = at[1] - 0.05f;
        m.life = randomRange(0.7f, 1.4f);
        m.size = randomRange(0.3f, 0.55f);
        m.grow = randomRange(1.4f, 2.4f) * (0.5f + pace);
        m.seed = random01();
        m.alpha = randomRange(0.15f, 0.28f) * (0.6f + 0.4f * SDL_min(pace, 1.0f));
        m.colour[0] = 0.7f; m.colour[1] = 0.72f; m.colour[2] = 0.76f;
        m.noGround = true;
        m.type = EffectParticle::Puff;
        add(m);
    }
    const int drops = int((1.0f + speed / 12.0f) * setting() + random01());
    for (int k = 0; k < SDL_min(drops, 8); ++k)
    {
        Particle d{};
        for (int i = 0; i < 3; ++i)
            d.p[i] = at[i] + randomRange(-0.2f, 0.2f);
        for (int i = 0; i < 3; i += 2)
            d.v[i] = carVelocity[i] * randomRange(0.5f, 0.8f) + randomRange(-1.5f, 1.5f);
        d.v[1] = randomRange(1.0f, 3.0f) * (0.5f + pace);
        d.floor = at[1] - 0.05f;
        d.life = randomRange(0.35f, 0.7f);
        d.size = randomRange(0.008f, 0.018f);
        d.seed = random01();
        d.alpha = randomRange(0.35f, 0.6f);
        d.colour[0] = 0.82f; d.colour[1] = 0.85f; d.colour[2] = 0.9f;
        d.noGround = true;
        d.type = EffectParticle::Grain;
        add(d);
    }
}

/* Smoke off a wheel spinning or locked -- the game's kind 6, taken over: a dark
 * cloud that comes off the tread, flies up and spreads, and is gone in a second
 * or two.  No gravity and no ground to settle on (it rises, thinning as it
 * grows), more of it and thicker the faster the wheel was going. */
void throwSmoke(const float at[3], const float velocity[3])
{
    const float smoke = smokeSetting();
    if (smoke <= 0.0f)
        return;
    const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    const float pace = SDL_min(speed / 30.0f, 1.2f);
    const int clouds = int((1.0f + speed / 10.0f) * smoke + random01());
    for (int k = 0; k < SDL_min(clouds, 3); ++k)
    {
        Particle m{};
        for (int i = 0; i < 3; ++i)
            m.p[i] = at[i] + randomRange(-0.2f, 0.2f);
        m.p[1] = at[1] + randomRange(0.02f, 0.2f);
        for (int i = 0; i < 3; i += 2)
            m.v[i] = velocity[i] * randomRange(0.2f, 0.45f) + randomRange(-0.9f, 0.9f) * (0.5f + pace);
        m.v[1] = randomRange(0.6f, 1.7f) * (0.5f + pace);
        m.life = randomRange(1.1f, 2.1f);
        m.size = randomRange(0.2f, 0.4f) * (0.7f + 0.5f * pace);
        m.grow = randomRange(1.3f, 2.5f) * (0.6f + pace);
        m.seed = random01();
        m.alpha = randomRange(0.22f, 0.42f) * SDL_min(1.0f, 0.55f + pace);
        const float grey = randomRange(0.14f, 0.21f);
        for (int i = 0; i < 3; ++i)
            m.colour[i] = grey;
        m.noGround = true;
        m.type = EffectParticle::Smoke;
        add(m);
    }
}

/* What falls out of the sky, off the game's own flakes and drops (kinds 7 and
 * 8): a raindrop is a short bright streak, a snowflake a small soft speck, both
 * where the game made its own and moving as it had it -- its speed and its size
 * as it gave them.  Rain or snow is the track's (the "type" in its
 * 3trNNw.hrz, 1 rain and 2 snow).  The game's pictures of them are squares with
 * nothing cutting them out, so a flake that came near the lens was a box of
 * dimmed picture over the view. */
void throwWeather(win32::WinApplication* app, const float at[3], const float velocity[3], float scale)
{
    if (s_weatherCount >= kMostWeather)
        return;   // as many as the pool keeps of them
    ++s_weatherCount;
    bool rains = true;   // without a track of its own, the weather is the rain's
    if (const x86::reg32 track = app->getMemory<x86::reg32>(0x6fd4ac))
        rains = trackRains(track);
    Particle w{};
    for (int i = 0; i < 3; ++i)
    {
        w.p[i] = at[i];
        w.origin[i] = at[i];
        w.v[i] = velocity[i];
    }
    w.seed = random01();
    w.noGround = true;
    /* Its floor is its caller's to say (fallWeather); left at 0, a drop on a
     * track lying below the world's 0 was under it as soon as it was made, and
     * was put up there and bounced: the rain gone from those tracks. */
    w.floor = at[1] - 40.0f;
    w.alpha = rains ? randomRange(0.5f, 0.85f) : randomRange(0.6f, 0.95f);
    if (rains)
    {
        w.life = randomRange(0.35f, 0.7f);
        w.size = SDL_clamp(scale, 0.004f, 0.03f);
        w.colour[0] = 0.72f;
        w.colour[1] = 0.78f;
        w.colour[2] = 0.9f;
        w.type = EffectParticle::Droplet;
    }
    else
    {
        w.life = randomRange(1.2f, 2.6f);
        w.size = SDL_clamp(scale, 0.01f, 0.06f);
        w.colour[0] = 0.94f;
        w.colour[1] = 0.96f;
        w.colour[2] = 1.0f;
        w.type = EffectParticle::Flake;
    }
    add(w);
}


x86::reg32 playerCar(win32::WinApplication* app);   // below

/* The weather the port makes itself (fallWeather): rain or snow falling round
 * the camera, as much on average as the game threw -- the game's own drops and
 * flakes (kinds 7 and 8) are counted, not drawn, and their rate is the mean --
 * and no longer the same all race long: a slow noise brings showers and lulls,
 * from a little of it up to NFS_WEATHER_MAX times the mean (2.5).  Where the
 * game throws none to measure, kFallbackRain or kFallbackSnow a second is the
 * mean.  Each starts somewhere in a cylinder round where the camera will be in
 * a moment and falls until it reaches the ground under the player's car. */
namespace weather
{
constexpr float kMaxDefault = 2.5f;          // NFS_WEATHER_MAX: the heaviest, against the mean
constexpr float kNoiseMean = 0.3124f;        // the mean of the squared noise below, worked out
constexpr float kNoiseSeconds = 9.0f;        // between two of the noise's knots: a shower over half a minute or so
constexpr float kMeasureSeconds = 3.0f;      // the game's rate, averaged over
constexpr float kFewest = 20.0f;             // a second: the game throwing fewer than this is not a rate
constexpr float kFallbackRain = 700.0f;      // drops a second, where the game throws none
constexpr float kFallbackSnow = 120.0f;      // flakes a second
/* The game's rate, as many in the air at once as the game's had: its drops
 * lived about half a second, as these take to reach the ground; its flakes
 * under two seconds, where these drift down for six. */
constexpr float kRainMatch = 1.0f, kSnowMatch = 0.3f;
constexpr float kRadius = 14.0f;             // m round the camera they start in
constexpr float kAhead = 0.5f;               // s along the camera's way the cylinder is centred
constexpr float kLowest = -1.0f, kHighest = 7.0f;   // m: where they start, against the camera's height
constexpr float kRainFall = 9.0f, kSnowFall = 1.0f; // m/s down as they start
constexpr float kRainSize = 0.012f, kSnowSize = 0.03f;
constexpr float kRainLife = 1.6f, kSnowLife = 9.0f; // s at most, if no ground stops them first
}  // namespace weather

float s_gameWeather = 0.0f;       // the game's drops and flakes thrown since the last frame
float s_gameWeatherRate = 0.0f;   // and a second, averaged
float s_weatherOwed = 0.0f;       // this frame's share of one not yet made
float s_camera[3] = {};           // where the main view's camera is (particlesFrame)
bool s_cameraKnown = false;

/* NFS_WEATHER_MAX: the heaviest the weather comes, against the mean (2.5); 1
 * keeps it as the game had it, steady. */
float weatherMax()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_WEATHER_MAX");
        const float parsed = text && *text ? float(SDL_atof(text)) : weather::kMaxDefault;
        return parsed >= 1.0f && parsed <= 10.0f ? parsed : weather::kMaxDefault;
    }();
    return value;
}

/* How much is falling now, against the mean: lo + (max - lo) n^2, n a smooth
 * noise from 0 to 1 over the race's time, lo chosen so the mean is 1. */
float weatherDensity(float seconds)
{
    auto knot = [](int k) {
        std::uint32_t h = std::uint32_t(k) * 0x9e3779b1u;
        h ^= h >> 15;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        return float(h & 0xffffff) / float(0x1000000);
    };
    const float at = seconds / weather::kNoiseSeconds;
    const int k = int(std::floor(at));
    const float u = smoothStep(0.0f, 1.0f, at - float(k));
    const float n = knot(k) + (knot(k + 1) - knot(k)) * u;
    const float most = weatherMax();
    const float least = SDL_max(0.0f, (1.0f - most * weather::kNoiseMean) / (1.0f - weather::kNoiseMean));
    return least + (most - least) * n * n;
}

bool weatherTracing()
{
    static const bool on = SDL_getenv("NFS_WEATHER_TRACE") != nullptr;
    return on;
}

void fallWeather(win32::WinApplication* app, float dt)
{
    const float measured = s_gameWeather / dt;
    s_gameWeather = 0.0f;
    s_gameWeatherRate += (measured - s_gameWeatherRate) * (1.0f - std::exp(-dt / weather::kMeasureSeconds));
    if (!s_cameraKnown || app->getMemory<x86::reg32>(0x6fd4cc) == 0)
        return;   // no weather, or no view to make it round
    bool rains = true;
    if (const x86::reg32 track = app->getMemory<x86::reg32>(0x6fd4ac))
        rains = trackRains(track);
    const float mean = s_gameWeatherRate >= weather::kFewest
                           ? s_gameWeatherRate * (rains ? weather::kRainMatch : weather::kSnowMatch)
                       : rains ? weather::kFallbackRain
                               : weather::kFallbackSnow;
    const float seconds = float(app->getMemory<x86::reg32>(0x7d3684)) / 64.0f;
    const float density = weatherDensity(seconds);
    s_weatherOwed += mean * density * dt;
    // The ground: the lowest of the player's wheels; and where the camera is going.
    const x86::reg32 car = playerCar(app);
    float ground = s_camera[1] - 3.0f, ahead[3] = { 0.0f, 0.0f, 0.0f };
    if (car)
    {
        float low = 1e9f;
        for (int w = 0; w < 4; ++w)
        {
            float contact[3];
            wheelContact(app, car, w, contact);
            low = SDL_min(low, contact[1]);
        }
        if (low < 1e8f)
            ground = low;
        for (int i = 0; i < 3; ++i)
            ahead[i] = app->getMemory<float>(car + 0xa4 + 4 * x86::reg32(i)) * weather::kAhead;
    }
    int made = 0;
    for (; s_weatherOwed >= 1.0f && s_weatherCount < kMostWeather; s_weatherOwed -= 1.0f, ++made)
    {
        const float angle = randomRange(0.0f, 6.2832f);
        const float r = weather::kRadius * std::sqrt(random01());   // even over the disc
        const float at[3] = { s_camera[0] + ahead[0] + r * std::cos(angle),
                              s_camera[1] + randomRange(weather::kLowest, weather::kHighest),
                              s_camera[2] + ahead[2] + r * std::sin(angle) };
        const float fall = rains ? weather::kRainFall : weather::kSnowFall;
        const float velocity[3] = { randomRange(-0.3f, 0.3f), -fall * randomRange(0.85f, 1.15f), randomRange(-0.3f, 0.3f) };
        throwWeather(app, at, velocity, rains ? weather::kRainSize : weather::kSnowSize);
        Particle& w = s_particles.back();
        w.floor = ground;
        w.life = rains ? randomRange(0.6f, 1.0f) * weather::kRainLife : randomRange(0.6f, 1.0f) * weather::kSnowLife;
    }
    s_weatherOwed = SDL_min(s_weatherOwed, 1.0f);   // what the pool had no room for is not owed
    if (weatherTracing())
    {
        static float said = 0.0f;
        if (seconds - said >= 1.0f || seconds < said)
        {
            said = seconds;
            SDL_Log("[WEATHER] %s: the game throws %.0f a second%s, now %.2f of the mean (most %.1f): %d this "
                    "frame, %zu in the air",
                    rains ? "rain" : "snow", double(s_gameWeatherRate),
                    s_gameWeatherRate >= weather::kFewest ? "" : " (too few: the port's own mean)", double(density),
                    double(weatherMax()), made, s_weatherCount);
        }
    }
}

/* One flame out of one of a car's exhausts, where the marking tool put it, in
 * the model's own space -- x to the car's right, y up, z forward -- which is the
 * car's own space here: its centre is at +0x98 and its axes at +0xc0 (right),
 * +0xcc (up) and +0xd8 (forward).  A hand's width behind the pipe, so it is out
 * of the body and shows from behind.  Which tip is the caller's.  A pop of the
 * overrun is half a flame (`scale` 0.5), and throws half the light. */
void throwFlame(const CarExhausts& exhausts, x86::reg32 car, std::size_t tip, float scale = 1.0f)
{
    Flame flame{};
    flame.car = car;
    flame.at[0] = exhausts.tips[tip].x;
    flame.at[1] = exhausts.tips[tip].y;
    flame.at[2] = exhausts.tips[tip].z - 0.06f;
    flame.life = 1.0f;
    flame.size = SDL_clamp(exhausts.tips[tip].radius * 2.6f, 0.05f, 0.16f) * scale;
    flame.light = scale;
    flame.seed = random01();
    for (int i = 0; i < 3; ++i)
        flame.colour[i] = exhausts.flame[i];
    if (s_flames.size() >= kMostFlames)
        s_flames.erase(s_flames.begin());   // the oldest makes room
    s_flames.push_back(flame);
}

/* A shift up: a flame out of each of the car's exhausts at once. */
void throwFlames(const CarExhausts& exhausts, x86::reg32 car)
{
    for (std::size_t tip = 0; tip < exhausts.tips.size(); ++tip)
        throwFlame(exhausts, car, tip);
}

/* A pop of the overrun -- a downshift, or the throttle lifted at the top of the
 * revs: one exhaust, half the size, a crackle rather than a flame. */
void popFlame(const CarExhausts& exhausts, x86::reg32 car)
{
    const std::size_t tip = std::size_t(random01() * float(exhausts.tips.size())) % exhausts.tips.size();
    throwFlame(exhausts, car, tip, 0.5f);
}

/* The engine's redline in rpm: the car's own data table (at [car+0x528], the one
 * the AI's pull comes from) keeps it at +0x1c4, and the drivetrain drives to it
 * -- redline less 200 rpm at full demand (0x406200, its `fild [eax+0x1c4]`), the
 * rev counter's needle at the dial's end.  Zero where the car has no table yet. */
x86::reg32 redline(win32::WinApplication* app, x86::reg32 car)
{
    const x86::reg32 table = app->getMemory<x86::reg32>(car + 0x528);
    /* Where the port maps guest memory: the image's sections from 0x400000, then
     * the simulated 128 MB above them (memmap.cpp, s_addressOffset), no further
     * than 0x11000000.  Anything else is not a table, and reading it would fault.
     * The game's own drivetrain subscribes to the same pointer for the same
     * number (sub_406160), so a car in the race has one -- the car's data, set
     * when it was loaded. */
    if (table < 0x400000 || table >= 0x11000000)
        return 0;
    const x86::sreg32 rpm = app->getMemory<x86::sreg32>(table + 0x1c4);
    return (rpm >= 2000 && rpm <= 20000) ? x86::reg32(rpm) : 0;   // if it is what we think it is
}

/* How much light the exhaust flames throw on the road (NFS_FLAME_LIGHT, on;
 * a number up to 3 for more, 0 or off for none). */
float flameLightSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_FLAME_LIGHT");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

bool tracing()
{
    static const bool on = SDL_getenv("NFS_EXHAUSTS_TRACE") != nullptr;
    return on;
}

/* The car the game's first view follows -- the first player's -- as every hook
 * that needs to know finds it: [0x5e10b0] is the view, its car eight bytes in
 * (sub_422bd0).  Zero where there is no view yet. */
x86::reg32 playerCar(win32::WinApplication* app)
{
    const x86::reg32 view = app->getMemory<x86::reg32>(0x5e10b0);
    return view ? x86::reg32(app->getMemory<x86::reg32>(view + 8)) : 0;
}

/* What the cars' engines are doing, each frame, for a flame out of the exhausts:
 * a car that has just shifted up (the game's 1 is neutral, 2 the first forward
 * gear, and 0x64 "none yet") throws them all; a car held at the top of its revs
 * with the throttle down and no speed (the start line) pops one now and then,
 * an exhaust at a time, as a limiter does.  Every car of the race is looked at
 * -- the opponents' as much as the player's -- a car in it being one with a
 * record of its own. */
void engineFlames(win32::WinApplication* app, float dt)
{
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const bool trace = tracing();
    const x86::reg32 player = playerCar(app);
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        const x86::reg32 record = app->getMemory<x86::reg32>(car + 0x220);
        if (record < 0x6fd52c || record >= 0x6fd52c + 16 * 0x6c || (record - 0x6fd52c) % 0x6c)
        {
            s_gear[k] = 0;   // not in the race: nothing remembered of it
            s_pop[k] = 0.0f;
            s_crackle[k] = 0.0f;
            s_revsKnown[k] = false;
            s_saidRevs[k] = 0.0f;
            s_traced[k] = 0;
            continue;
        }
        const CarExhausts* exhausts = carExhausts(app, car);
        if (!exhausts)
            continue;   // this car has none marked, and so shows no flames
        const x86::reg32 gear = app->getMemory<x86::reg8>(car + 0x50b);
        const x86::reg32 was = s_gear[k];
        s_gear[k] = gear;
        const float vx = f(car + 0xa4), vy = f(car + 0xa8), vz = f(car + 0xac);
        const float speed = SDL_sqrtf(vx * vx + vy * vy + vz * vz);
        const x86::reg32 limit = redline(app, car);
        const float revs = float(app->getMemory<x86::sreg32>(car + 0x590));
        if (was >= 1 && was <= 0x63 && gear > was && gear >= 2)
        {
            throwFlames(*exhausts, car);
            if (trace && !(s_traced[k] & 1))
            {
                s_traced[k] |= 1;
                SDL_Log("[EXHAUST] slot %u \"%s\": shifted up into %u -- %zu flames", unsigned(k),
                        exhausts->folder.c_str(), unsigned(gear), exhausts->tips.size());
            }
        }
        /* A hard downshift at speed: the overrun pops out of the exhausts as the
         * engine is dragged up to the lower gear's revs -- one exhaust at a
         * time, half a flame each, a crackle rather than a jet.  Real racing
         * cars do it on the overrun, and a downshift is the hardest overrun
         * there is. */
        if (was >= 2 && was <= 0x63 && gear >= 2 && gear < was && speed > 4.0f)
        {
            const int pops = random01() < 0.5f ? 2 : 1;
            for (int n = 0; n < pops; ++n)
                popFlame(*exhausts, car);
            if (trace && !(s_traced[k] & 4))
            {
                s_traced[k] |= 4;
                SDL_Log("[EXHAUST] slot %u \"%s\": downshifted from %u into %u at %.0f m/s -- %d pops",
                        unsigned(k), exhausts->folder.c_str(), unsigned(was), unsigned(gear), speed, pops);
            }
        }
        /* Revved at a standstill -- the start line.  A car going nowhere has no
         * throttle of its own to ask about, and what the game makes of its revs
         * there is the game's to decide, so there are three ways to be revving
         * and any will do: the player's throttle down (below), the revs pinned at
         * the redline (the top seventh, where its pull is), or climbing by 60 rpm
         * a frame, a blip no car over a walking pace can be doing without the
         * speed to go with it.  The next pop is a quarter of a second off, more
         * or less; the timer only runs while the engine is held there, so a
         * limiter's bounce cannot turn into a stream of them. */
        const bool known = s_revsKnown[k];
        const bool climbing = known && revs > s_revs[k] + 60.0f;
        s_revs[k] = revs;
        s_revsKnown[k] = true;
        const bool pinned = limit && revs >= 0.85f * float(limit);
        /* The first player's own throttle, 0 to 255, at 0x79c339 (the camera's
         * pull is off the same byte): a car does not keep one, and what a car
         * standing still does with its revs is the game's to decide -- so it is
         * asked directly, of the player's car, and the revs are only a second and
         * third opinion (native_effects.cpp's cameraToView reads it too). */
        const bool throttle = car == player && app->getMemory<x86::reg8>(0x79c339) > 0xa0;   // over five eighths
        if (speed < 3.0f && (pinned || climbing || throttle))
        {
            s_pop[k] -= dt;
            if (s_pop[k] <= 0.0f)
            {
                throwFlame(*exhausts, car, std::size_t(random01() * float(exhausts->tips.size()))
                                              % exhausts->tips.size());
                s_pop[k] = randomRange(0.15f, 0.35f);
                if (trace && !(s_traced[k] & 2))
                {
                    s_traced[k] |= 2;
                    SDL_Log("[EXHAUST] slot %u \"%s\": revving at a standstill (%s), %.0f of %u rpm -- %zu tips",
                            unsigned(k), exhausts->folder.c_str(),
                            pinned ? "at the redline" : climbing ? "climbing" : "throttle down", revs,
                            unsigned(limit), exhausts->tips.size());
                }
            }
        }
        /* Lifting off at the top of the revs on the move: the other overrun, the
         * one a driver hears as a crackle out of the exhausts for as long as the
         * engine is dragged down by the car -- a pop every eighth to a third of
         * a second, half a flame each, quieter than a gear change's.  Only the
         * player's throttle is a byte the port can read (0x79c339), so only the
         * player's car crackles here. */
        if (car == player && speed > 12.0f && limit && revs > 0.7f * float(limit)
            && app->getMemory<x86::reg8>(0x79c339) < 0x30)
        {
            s_crackle[k] = SDL_max(0.0f, s_crackle[k] - dt);
            if (s_crackle[k] <= 0.0f)
            {
                popFlame(*exhausts, car);
                s_crackle[k] = randomRange(0.1f, 0.35f);
                if (trace && !(s_traced[k] & 8))
                {
                    s_traced[k] |= 8;
                    SDL_Log("[EXHAUST] slot %u \"%s\": crackling on the overrun at %.1f m/s, %.0f of %u rpm",
                            unsigned(k), exhausts->folder.c_str(), speed, revs, unsigned(limit));
                }
            }
        }
        if (trace && speed < 3.0f && revs > s_saidRevs[k] + 1000.0f)
        {
            s_saidRevs[k] = revs;   // the revs it has got to, as they climb: what to tune from
            SDL_Log("[EXHAUST] slot %u \"%s\": at rest, %.0f rpm (redline %u), %.2f m/s", unsigned(k),
                    exhausts->folder.c_str(), revs, unsigned(limit), speed);
        }
    }
}

/* Brake discs that glow at night (NFS_BRAKE_GLOW).  Each car has a glow, 0 to
 * 1: while it brakes hard it comes up to how fast the car is going (none
 * below kFromSpeed, all of it from kFullSpeed) in kRiseSeconds, and once it
 * stops braking it goes out over kFadeSeconds.  The first player's car brakes
 * hard when its brake byte (0x79c33a, as the tyre marks read it) is past
 * kPedal; any other car -- no car's pedal is a byte the port can read but the
 * first player's -- when it slows harder than kDecelMin, past what drag and
 * the engine do, and not so hard that it is a hit (kDecelMost).  Drawn at
 * night only ([0x6fd4c8]), at each wheel: a pool of orange light on the road
 * where the wheel touches it (EffectParticle::Glow, the flames' light) and a
 * glow at the hub (EffectParticle::BrakeGlow); the fronts brighter than the
 * rears, as the front brakes do most of the work. */
namespace brakes
{
constexpr float kPedal = 0.55f;             // the first player's brake byte, of 255, that is braking hard
constexpr float kDecelMin = 6.0f;           // m/s^2: another car slowing harder than this is braking hard
constexpr float kDecelMost = 18.0f;         // m/s^2: slowing harder than this is a hit
constexpr float kFromSpeed = 8.0f;          // m/s: no glow braking slower than this
constexpr float kFullSpeed = 30.0f;         // m/s: braking from this fast or faster, all of it
constexpr float kRiseSeconds = 0.35f;       // from out to full while braking
constexpr float kFadeSeconds = 1.25f;       // from full to out once the brake is off
constexpr float kRearShare = 0.6f;          // a rear wheel's, against a front's
constexpr float kHubHeight = 0.3f;          // m: the hub above where the wheel touches the road (its radius)
constexpr float kHubOut = 0.05f;            // m: out from the wheel's middle, towards its face
constexpr float kDiscRadius = 0.22f;        // m: the hub glow's radius
constexpr float kFlatMost = 0.3f;           // a disc seen edge on is drawn this wide of round, at the least
constexpr float kHubAlpha = 0.8f;           // how bright the hub glow is added, at full
constexpr float kPoolRadius = 0.55f;        // m: the pool on the road
constexpr float kPoolAlpha = 0.35f;         // and how bright, at full
constexpr float kCool[3] = { 0.85f, 0.16f, 0.03f };   // dull red, as it starts to show
constexpr float kHot[3] = { 1.0f, 0.42f, 0.1f };      // orange, at full
constexpr x86::reg32 kNight = 0x6fd4c8;
constexpr x86::reg32 kBrakeByte = 0x79c33a;
}  // namespace brakes

float s_brakeGlow[16];          // each car's, 0 to 1
float s_brakeVelocity[16][3];   // each car's velocity a frame ago
bool s_brakeKnown[16];

/* NFS_BRAKE_GLOW: on (1); a number up to 3 for brighter, 0 or off for none. */
float brakeGlowSetting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_BRAKE_GLOW");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

/* Each car's brake glow brought up while it brakes hard and let go out after,
 * once a frame. */
void brakeGlowFrame(win32::WinApplication* app, float dt)
{
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const x86::reg32 player = playerCar(app);
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        const x86::reg32 record = app->getMemory<x86::reg32>(car + 0x220);
        if (record < 0x6fd52c || record >= 0x6fd52c + 16 * 0x6c || (record - 0x6fd52c) % 0x6c)
        {
            s_brakeGlow[k] = 0.0f;   // not in the race
            s_brakeKnown[k] = false;
            continue;
        }
        const float v[3] = { f(car + 0xa4), f(car + 0xa8), f(car + 0xac) };
        const float speed = SDL_sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        bool braking = false;
        if (app->getMemory<uint16_t>(car + 0x164) == 0 && speed > brakes::kFromSpeed)
        {
            if (car == player)
                braking = float(app->getMemory<x86::reg8>(brakes::kBrakeByte)) > brakes::kPedal * 255.0f;
            else if (s_brakeKnown[k])
            {
                float shed = 0.0f;   // speed lost along the way it goes, m/s
                for (int i = 0; i < 3; ++i)
                    shed += (s_brakeVelocity[k][i] - v[i]) * v[i] / speed;
                const float decel = shed / dt;
                braking = decel > brakes::kDecelMin && decel < brakes::kDecelMost;
            }
        }
        for (int i = 0; i < 3; ++i)
            s_brakeVelocity[k][i] = v[i];
        s_brakeKnown[k] = true;
        float& glow = s_brakeGlow[k];
        const float target = braking ? smoothStep(brakes::kFromSpeed, brakes::kFullSpeed, speed) : 0.0f;
        if (glow < target)
            glow = SDL_min(target, glow + dt / brakes::kRiseSeconds);
        else
            glow = SDL_max(target, glow - dt / brakes::kFadeSeconds);
    }
}

void throwSpark(const float at[3], const float velocity[3])
{
    const int count = random01() < 0.35f * setting() ? 2 : 1;
    for (int k = 0; k < count; ++k)
    {
        Particle s{};
        for (int i = 0; i < 3; ++i)
        {
            s.p[i] = at[i];
            s.origin[i] = at[i];
            s.v[i] = velocity[i] * randomRange(0.75f, 1.25f) + randomRange(-1.5f, 1.5f);
        }
        s.v[1] += randomRange(0.5f, 2.5f);
        s.floor = at[1] - 1.0f;
        s.life = randomRange(0.25f, 0.7f);
        s.size = randomRange(0.012f, 0.025f);
        s.seed = random01();
        s.alpha = 1.0f;
        s.type = EffectParticle::Spark;
        add(s);
    }
}

void move(float dt)
{
    s_weatherCount = 0;
    for (const Particle& q : s_particles)
        s_weatherCount += q.type == EffectParticle::Droplet || q.type == EffectParticle::Flake ? 1 : 0;
    for (Flame& flame : s_flames)
        flame.age += dt;
    s_flames.erase(std::remove_if(s_flames.begin(), s_flames.end(),
                                  [](const Flame& flame) { return flame.age >= flame.life; }),
                   s_flames.end());
    for (Particle& q : s_particles)
    {
        q.age += dt;
        if (q.settled)
            continue;
        const float drag = q.type == EffectParticle::Puff    ? 1.6f
                           : q.type == EffectParticle::Spark ? 0.4f
                           : q.type == EffectParticle::Blade ? 1.8f   // a blade flutters down
                           : q.type == EffectParticle::Smoke ? 1.15f
                           : q.type == EffectParticle::Droplet ? 0.2f   // it falls as it was thrown
                           : q.type == EffectParticle::Flake ? 1.2f     // a flake drifts
                                                             : 0.9f;
        /* Smoke has no ground to fall to and nothing weighing it down: it rises,
         * slowing and spreading, until it is gone. */
        const float gravity = q.type == EffectParticle::Puff     ? -0.15f
                              : q.type == EffectParticle::Smoke  ? -0.5f
                              : q.type == EffectParticle::Droplet ? 9.8f
                              : q.type == EffectParticle::Flake  ? 0.8f
                                                                 : 9.8f;
        const float keep = std::exp(-drag * dt);
        for (int i = 0; i < 3; ++i)
            q.v[i] *= keep;
        q.v[1] -= gravity * dt;
        for (int i = 0; i < 3; ++i)
            q.p[i] += q.v[i] * dt;
        q.size += q.grow * dt;
        if (q.type != EffectParticle::Puff && q.type != EffectParticle::Smoke && q.p[1] < q.floor
            && q.v[1] < 0.0f)
        {
            q.p[1] = q.floor;
            if (q.type == EffectParticle::Droplet || q.type == EffectParticle::Flake)
                q.life = q.age;   // a drop or a flake on the ground is gone
            else if (q.type == EffectParticle::Grain || q.type == EffectParticle::Blade)
            {
                // Down: it lies there a moment, then is gone.
                q.settled = true;
                q.life = SDL_min(q.life, q.age + 0.35f);
            }
            else
            {
                q.v[1] = -q.v[1] * 0.3f;   // a spark skips off the ground
                q.v[0] *= 0.6f;
                q.v[2] *= 0.6f;
            }
        }
    }
    s_particles.erase(std::remove_if(s_particles.begin(), s_particles.end(),
                                     [](const Particle& q) { return q.age >= q.life; }),
                      s_particles.end());
}

}  // namespace

/* sub_4ca850's kinds 2 and 3, taken over (above): true when this made them
 * instead, the game's call then returned from with none made (eax 0). */
int groundUnder(win32::WinApplication* app, x86::reg32 car, int wheel);   // native_surface.cpp

/* Where a car's wheel (0 to 3) touches the road: the suspension record at
 * car+0x300, 0x78 bytes each, keeps it at +4 as three 16.16 fixed-point ints --
 * whole units of 1/65536, which the game itself turns into floats that way
 * (`sub_4ca160`, the dust its own wheels throw, `fild`s each and times it by
 * 1/65536).  Read as floats they are nonsense, and a particle thrown there
 * lands at the world's origin. */
void wheelContact(win32::WinApplication* app, x86::reg32 car, int wheel, float at[3])
{
    const x86::reg32 record = car + 0x300 + x86::reg32(wheel) * 0x78;
    for (int i = 0; i < 3; ++i)
        at[i] = float(app->getMemory<x86::sreg32>(record + 4 + x86::reg32(i) * 4)) * (1.0f / 65536.0f);
}

/* The kind of ground under the wheel nearest a point (within 1.2 m): as
 * groundUnder, or -1 for none near. */
int groundAt(win32::WinApplication* app, const float at[3])
{
    float nearest = 1.2f * 1.2f;
    int ground = -1;
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        for (int w = 0; w < 4; ++w)
        {
            float wheel[3];
            wheelContact(app, car, w, wheel);
            const float d[3] = { wheel[0] - at[0], wheel[1] - at[1], wheel[2] - at[2] };
            const float distance = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (distance < nearest)
            {
                nearest = distance;
                ground = groundUnder(app, car, w);
            }
        }
    }
    return ground;
}

/* Grass under a moving car's wheels throws blades -- the game throws nothing
 * there while a car only drives over it (its kind 5 comes off some grass, not
 * all: Atlantica's and Hometown's give none).  Each frame, from each wheel on
 * grass of each car going faster than 6 m/s: at most three, more often the
 * faster, from where the wheel touches. */
void grassFromWheels(win32::WinApplication* app, float dt)
{
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        const float speed = std::fabs(float(app->getMemory<float>(car + 0xb8)));
        if (!(speed > 6.0f) || app->getMemory<uint16_t>(car + 0x164) != 0)
            continue;
        const float velocity[3] = { app->getMemory<float>(car + 0xa4), app->getMemory<float>(car + 0xa8),
                                    app->getMemory<float>(car + 0xac) };
        for (int w = 0; w < 4; ++w)
        {
            if (random01() >= SDL_min(speed / 25.0f, 1.2f) * 60.0f * dt * setting() || groundUnder(app, car, w) != 1)
                continue;
            float at[3];
            wheelContact(app, car, w, at);
            throwDirt(at, velocity, true, 1 + int(random01() * 2.5f), random01() < 0.05f);
        }
    }
}

/* What makes a tyre smoke: a wheel scrubbed sideways -- cornering at the limit,
 * or sliding -- a wheel spun up from low speed, a wheel locked under braking.
 * That is when the game lays its tyre marks, so it is when smoke comes off them
 * (the game's own kind 6, throwSmoke, only ever came off some ground).  Each
 * frame, each car of the race, from one of the wheels that is scrubbing, on
 * pavement: more often and thicker the harder it scrubs. */
void tyreSmoke(win32::WinApplication* app, float dt)
{
    const float smoke = smokeSetting();
    if (smoke <= 0.0f || dt <= 0.0f)
        return;
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const x86::reg32 player = playerCar(app);
    for (x86::reg32 k = 0; k < 16; ++k)
    {
        const x86::reg32 car = 0x5e1120 + k * 0x9ac;
        const x86::reg32 record = app->getMemory<x86::reg32>(car + 0x220);
        if (record < 0x6fd52c || record >= 0x6fd52c + 16 * 0x6c || (record - 0x6fd52c) % 0x6c)
            continue;   // not in the race
        if (app->getMemory<uint16_t>(car + 0x164) != 0)
            continue;   // in the air, nothing on the road
        const float velocity[3] = { f(car + 0xa4), f(car + 0xa8), f(car + 0xac) };
        const float speed = SDL_sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]
                                      + velocity[2] * velocity[2]);
        if (!(speed > 4.0f))
            continue;
        /* Sideways is the velocity across the car (+0xc0 is its right): over a
         * metre a second the tyres are being dragged over a road they no longer
         * roll along. */
        const float right[3] = { f(car + 0xc0), f(car + 0xc4), f(car + 0xc8) };
        const float lateral = SDL_fabsf(velocity[0] * right[0] + velocity[1] * right[1] + velocity[2] * right[2]);
        float scrub = lateral > 1.2f ? SDL_min(1.0f, (lateral - 1.2f) / 3.0f) : 0.0f;
        bool driven = false;   // the smoke comes off the wheels that are driven
        if (car == player)
        {
            /* The first player's own pedals, 0 to 255: the throttle at 0x79c339
             * and the brake at 0x79c33a (after the steering at 0x79c338). */
            const float throttle = float(app->getMemory<x86::reg8>(0x79c339)) / 255.0f;
            const float brake = float(app->getMemory<x86::reg8>(0x79c33a)) / 255.0f;
            const x86::reg32 limit = redline(app, car);
            const float revs = float(app->getMemory<x86::sreg32>(car + 0x590));
            const x86::reg32 gear = app->getMemory<x86::reg8>(car + 0x50b);
            // Spun up from a standstill: throttle down, in gear, revs up, the car
            // barely moving -- the wheels are going round faster than the road.
            if (throttle > 0.6f && gear >= 2 && speed < 12.0f && limit && revs > 0.5f * float(limit))
            {
                scrub = SDL_max(scrub, throttle);
                driven = true;
            }
            // Locked: the brakes hard on with the car still going.
            if (brake > 0.6f && speed > 8.0f)
                scrub = SDL_max(scrub, brake);
        }
        if (!(scrub > 0.05f))
            continue;
        const float forward[3] = { f(car + 0xd8), f(car + 0xdc), f(car + 0xe0) };
        for (int w = 0; w < 4; ++w)
        {
            if (groundUnder(app, car, w) != 0)
                continue;   // pavement: grass, dirt and snow have dust of their own
            float at[3];
            wheelContact(app, car, w, at);
            // Which end of the car it is: the contact point, along its forward axis.
            const float ahead = (at[0] - f(car + 0x98)) * forward[0] + (at[1] - f(car + 0x9c)) * forward[1]
                                + (at[2] - f(car + 0xa0)) * forward[2];
            if (driven && ahead > 0.0f)
                continue;   // a wheel spinning smokes at the driven end, not both
            if (random01() >= scrub * 6.0f * dt * smoke)
                continue;
            throwSmoke(at, velocity);
            break;   // one wheel a frame is enough to see
        }
    }
}

bool particleTakeOver(win32::WinApplication* app, x86::CPU& cpu)
{
    static bool s_inner = false;   // the game's own call, made from here (below)
    x86::reg32 kind = cpu.eax;
    /* The game's smoke (6) -- off a wheel spinning or locked -- is the port's
     * own: on grass, grass thrown up instead of a dark cloud; anywhere else,
     * smoke blacker than the game's, flying up and spreading (throwSmoke). */
    if (kind == 6 && !s_inner && setting() > 0.0f && win32::glide2x::effectParticlesDrawn()
        && app->getMemory<x86::reg32>(0x6fd3b0) != 1)
    {
        const float here[3] = { argument(app, cpu, 0), argument(app, cpu, 1), argument(app, cpu, 2) };
        if (groundAt(app, here) == 1)
            kind = 5;
    }
    if (kind == 7 || kind == 8)
    {
        if (!weatherSetting())
            return false;   // the game's own pictures
    }
    if (s_inner
        || (kind != 2 && kind != 3 && kind != 5 && kind != 6 && kind != 7 && kind != 8 && kind != 10)
        || setting() <= 0.0f || (kind == 6 && smokeSetting() <= 0.0f)
        || !win32::glide2x::effectParticlesDrawn() || (kind == 10 && !spraySetting()))
        return false;
    if (app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return false;   // split screen: the game's own
    const float at[3] = { argument(app, cpu, 0), argument(app, cpu, 1), argument(app, cpu, 2) };
    const float velocity[3] = { argument(app, cpu, 10), argument(app, cpu, 11), argument(app, cpu, 12) };
    /* The game's particle is still made, then let go at once (+0 0, not
     * live): what sub_4ca850 sets up as it makes the first of a kind -- the
     * pictures the weather's flakes and drops share with them -- is then set
     * up as before.  Skipping it left the snow and rain drawn as bare squares. */
    {
        x86::reg32 arguments[13];
        for (int k = 0; k < 13; ++k)
            arguments[k] = app->getMemory<x86::reg32>(cpu.esp + 4 + 4 * x86::reg32(k));
        const x86::reg32 esp = cpu.esp;
        s_inner = true;
        const bool ran = guest::call(app, cpu, 0x4ca850,
                                     { arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5],
                                       arguments[6], arguments[7], arguments[8], arguments[9], arguments[10], arguments[11],
                                       arguments[12] });
        s_inner = false;
        if (!ran)
            return true;   // the game stopped
        cpu.esp = esp;
        if (cpu.eax)
            app->getMemory<x86::reg32>(cpu.eax) = 0;
    }
    if (kind == 10)
        throwSpray(app, at, velocity);
    else if (kind == 7 || kind == 8)
        s_gameWeather += 1.0f;   // counted for the mean; what falls is fallWeather's
    else if (kind == 6)
        throwSmoke(at, velocity);
    else if (kind == 2 || kind == 5)
        throwDirt(at, velocity, kind == 5);
    else
        throwSpark(at, velocity);
    cpu.eax = 0;
    cpu.esp += 4 + 0x34;
    return true;
}

/* Each frame, the main view's (view: its camera at +0x38, matrix at +0x44, as
 * sub_492370 turns a point into it; the projection at [0x56009c..a8]). */
void pursuitFrame(win32::WinApplication* app, std::vector<std::array<float, 3>>& teeth);  // native_pursuit.cpp
float slipstreamOf(win32::WinApplication* app, x86::reg32 car);                           // native_surface.cpp

namespace
{

/* The air trails round the player's car while it slipstreams
 * (NFS_SLIPSTREAM_TRAILS, on; 0 or off for none), drawn as the air lines of a
 * cartoon: a few thin white strands beside it, three a side and three behind
 * it, each at its own place -- a little further out, higher or lower, shorter
 * or longer -- following the way the car went, narrow at their front and wider
 * towards their tail, fading as they go.  Each lives 0.6 to 1.3 s: it comes up
 * quickly, fades out briefly, and a new one starts near its place, a little
 * off it.  As strong as the slipstream.  Their places are the car's own
 * (right, up, forward, metres from its centre); where each was, every frame. */
struct TrailPoint
{
    float p[3];
    float strength;
    x86::sreg32 clock;
};
struct Strand
{
    float right, up, forward;   // its place now, by the car
    float width;                // metres at its widest
    x86::sreg32 born = -1, life = 0;
    std::vector<TrailPoint> points;
};
struct Slot
{
    float right, up, forward, width;
};
// Three a side, out from the body and staggered; three behind, wider.
constexpr Slot kSlots[] = {
    { -1.02f, 0.05f, 0.4f, 0.022f }, { -1.12f, 0.32f, -0.4f, 0.018f }, { -1.25f, -0.12f, -1.0f, 0.026f },
    { 1.02f, 0.05f, 0.4f, 0.022f },  { 1.12f, 0.32f, -0.4f, 0.018f },  { 1.25f, -0.12f, -1.0f, 0.026f },
    { -0.55f, 0.28f, -2.2f, 0.034f }, { 0.08f, 0.45f, -2.3f, 0.04f },  { 0.6f, 0.2f, -2.2f, 0.034f },
};
constexpr int kStrands = int(sizeof kSlots / sizeof kSlots[0]);
Strand s_strands[kStrands];
constexpr x86::sreg32 kTrailTicks = 18;   // how long a point of a strand stays: 0.36 s

bool trailsOn()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_SLIPSTREAM_TRAILS");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return on;
}

void noteTrail(win32::WinApplication* app, x86::sreg32 clock)
{
    const x86::reg32 car = app->getMemory<x86::reg32>(0x5efa48);
    const float strength = car ? slipstreamOf(app, car) : 0.0f;
    auto f = [app](x86::reg32 address) { return float(app->getMemory<float>(address)); };
    for (int n = 0; n < kStrands; ++n)
    {
        Strand& strand = s_strands[n];
        auto& points = strand.points;
        while (!points.empty() && (clock - points.front().clock > kTrailTicks || clock < points.front().clock))
            points.erase(points.begin());
        if (clock < strand.born)
            strand.born = -1;   // a new race
        const bool alive = strand.born >= 0 && clock - strand.born < strand.life;
        if (!alive)
        {
            if (strength < 0.05f || !car || !points.empty())
                continue;   // a new one only once the last has gone
            // A new one, near its place, a little off it; staggered so they do not come and go together.
            const Slot& slot = kSlots[n];
            strand.right = slot.right + randomRange(-0.18f, 0.18f) * (slot.right < 0.0f ? -1.0f : 1.0f);
            strand.up = slot.up + randomRange(-0.1f, 0.1f);
            strand.forward = slot.forward + randomRange(-0.35f, 0.35f);
            strand.width = slot.width * randomRange(0.75f, 1.25f);
            strand.life = x86::sreg32(randomRange(38.0f, 84.0f));
            strand.born = clock - x86::sreg32(random01() * 6.0f);
        }
        if (!car || (!points.empty() && points.back().clock == clock))
            continue;
        TrailPoint t{};
        t.clock = clock;
        for (int i = 0; i < 3; ++i)
        {
            const x86::reg32 k = 4 * x86::reg32(i);
            t.p[i] = f(car + 0x98 + k) + f(car + 0xc0 + k) * strand.right + f(car + 0xcc + k) * strand.up
                     + f(car + 0xd8 + k) * strand.forward;
        }
        // Its life: up in the first sixth, down in the last quarter.
        const float lived = float(clock - strand.born) / float(SDL_max(strand.life, x86::sreg32(1)));
        t.strength = strength * SDL_min(1.0f, lived * 6.0f) * SDL_min(1.0f, (1.0f - lived) * 4.0f);
        points.push_back(t);
    }
}

/* The frame's particles, for every view of it -- the main one and, behind the
 * car, the rear-view mirror's: the alpha-blended ones (grains, puffs, smoke, a
 * strip's spikes) into the first, in the order drawParticles draws them, and
 * the added ones (sparks, streaks, flames and the light they throw) into the
 * second. */
std::vector<EffectParticle> s_out;
std::vector<EffectParticle> s_sparks;
std::vector<std::array<float, 3>> s_teeth;   // Hot Pursuit's spike strips, drawn with them
bool s_showTrails = false;                  // this frame's slipstream air lines
bool s_showFlames = false;                  // and its exhaust flames
bool s_showBrakes = false;                  // and its brake discs' glow
bool s_frameOn = false;                     // anything to draw at all

bool deeper(const EffectParticle& a, const EffectParticle& b)
{
    return a.depth > b.depth;   // back to front
}

/* The frame's particles moved on and made: once a frame, before any view of it
 * is turned into the picture. */
void frameParticles(win32::WinApplication* app)
{
    const bool trails = s_showTrails;
    const bool flames = s_showFlames;
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(0x7d3684));
    if (s_clock < 0 || clock < s_clock || clock - s_clock > 64)
    {
        for (x86::reg32& gear : s_gear)
            gear = 0;      // a new race, or a jump: no shift since the last frame
        for (float& pop : s_pop)
            pop = 0.0f;
        for (float& crackle : s_crackle)
            crackle = 0.0f;
        for (bool& known : s_revsKnown)
            known = false;
        for (float& said : s_saidRevs)
            said = 0.0f;
        for (unsigned& traced : s_traced)
            traced = 0;
        for (x86::reg32 k = 0; k < 16; ++k)
        {
            s_brakeGlow[k] = 0.0f;   // out
            s_brakeKnown[k] = false;
        }
        s_clock = clock;   // and nothing is moved by it
    }
    if (clock < 2)
    {
        s_particles.clear();
        s_flames.clear();
    }
    const float dt = float(clock - s_clock) / 64.0f;
    s_clock = clock;
    if (trails)
        noteTrail(app, clock);
    if (flames)
        engineFlames(app, dt);
    if (s_showBrakes && dt > 0.0f)
        brakeGlowFrame(app, dt);
    if (dt > 0.0f)
    {
        move(dt);
        if (setting() > 0.0f && weatherSetting() && app->getMemory<x86::reg32>(0x6fd3b0) != 1)
            fallWeather(app, dt);
        if (setting() > 0.0f && app->getMemory<x86::reg32>(0x6fd3b0) != 1)
        {
            grassFromWheels(app, dt);
            tyreSmoke(app, dt);
        }
    }
}

/* The frame's particles and flames in one view: its camera (at +0x38 of the
 * view's record) and its matrix (+0x44) turn where each one is into that view's
 * space, and the projection ([0x56009c..a8], set for the pass) into the
 * picture.  The alpha-blended ones go to s_out, the added ones to s_sparks. */
void projectParticles(win32::WinApplication* app, x86::reg32 view)
{
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const bool trails = s_showTrails;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(0x7d3684));
    const float* m = &app->getMemory<float>(view + 0x44);
    const float t[3] = { f(view + 0x38), f(view + 0x3c), f(view + 0x40) };
    const float k1 = f(0x56009c), k2 = f(0x5600a0), c1 = f(0x5600a4), c2 = f(0x5600a8);
    auto project = [&](const float p[3], float& sx, float& sy, float& z) {
        const float x = p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + t[0];
        const float y = p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + t[1];
        z = p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + t[2];
        if (!(z > 0.3f))
            return false;
        sx = k1 * x / z + c1;
        sy = k2 * y / z + c2;
        return true;
    };
    s_out.reserve(s_out.size() + s_particles.size());
    const float scale = std::fabs(k1);
    for (Particle& q : s_particles)
    {
        EffectParticle e{};
        float z = 0.0f;
        /* Not on the lens: one within 1.5 m of the camera is left out, and
         * fades in from 3.5 m -- drawn there, a quad hundreds of pixels across
         * showed its edges (the game splashes its own on the view instead,
         * sub_4ca500, and kills them). */
        if (!project(q.p, e.x, e.y, z) || z < 1.5f)
        {
            q.hasLast = false;
            continue;
        }
        const float near = smoothStep(1.5f, 3.5f, z);
        e.depth = z;
        e.seed = q.seed;
        e.type = float(q.type);
        const float left = 1.0f - q.age / q.life;
        if (q.type == EffectParticle::Droplet)
        {
            // A drop is a short bright streak from where it was a frame ago.
            e.r = q.colour[0];
            e.g = q.colour[1];
            e.b = q.colour[2];
            e.a = q.alpha * near;
            e.size = SDL_max(0.8f, scale * q.size / z);
            if (q.hasLast)
            {
                e.tx = q.last[0];
                e.ty = q.last[1];
            }
            else
            {
                const float back[3] = { q.p[0] - q.v[0] * 0.03f, q.p[1] - q.v[1] * 0.03f, q.p[2] - q.v[2] * 0.03f };
                float bz;
                if (!project(back, e.tx, e.ty, bz))
                {
                    e.tx = e.x;
                    e.ty = e.y;
                }
            }
            q.last[0] = e.x;
            q.last[1] = e.y;
            q.hasLast = true;
            s_sparks.push_back(e);
            continue;
        }
        if (q.type == EffectParticle::Spark)
        {
            // White hot to yellow, orange and a dull red as it cools.
            const float heat = left;
            e.r = 1.0f;
            e.g = SDL_min(1.0f, 0.25f + 0.85f * heat);
            e.b = SDL_max(0.0f, heat * heat * 0.8f - 0.1f);
            e.a = SDL_min(1.0f, left * 1.6f) * near;
            e.size = SDL_max(0.8f, scale * q.size / z);
            if (q.hasLast)
            {
                e.tx = q.last[0];
                e.ty = q.last[1];
            }
            else
            {
                const float back[3] = { q.p[0] - q.v[0] * 0.025f, q.p[1] - q.v[1] * 0.025f, q.p[2] - q.v[2] * 0.025f };
                float bz;
                if (!project(back, e.tx, e.ty, bz))
                {
                    e.tx = e.x;
                    e.ty = e.y;
                }
            }
            q.last[0] = e.x;
            q.last[1] = e.y;
            q.hasLast = true;
            s_sparks.push_back(e);
            continue;
        }
        // A grain's quad is twice it: on grass the renderer draws a blade in it.
        const bool small = q.type == EffectParticle::Grain || q.type == EffectParticle::Blade;
        e.size = SDL_min(scale * q.size / z * (small ? 2.0f : 1.0f), small ? 120.0f : 300.0f);
        e.turn = q.seed * 6.2832f + q.age * (q.seed - 0.5f) * 14.0f;
        if (e.size < 0.6f)
            continue;
        e.r = q.colour[0];
        e.g = q.colour[1];
        e.b = q.colour[2];
        const float in = SDL_min(1.0f, q.age / 0.08f) * near;
        const bool puffing = q.type == EffectParticle::Puff || q.type == EffectParticle::Smoke;
        e.a = q.alpha * in * (puffing ? left * left : SDL_min(1.0f, left * 3.0f));
        float oz;
        if (q.noGround || !project(q.origin, e.tx, e.ty, oz))
            e.tx = e.ty = -1.0f;
        s_out.push_back(e);
    }
    /* The road under each car's wheels, for the light its flames throw on it: a
     * wheel's contact point is at +0x304, 0x78 bytes each, and the lowest of the
     * four is the ground the car stands on.  Worked out once a frame, for the
     * cars that light something. */
    float groundY[16];
    bool groundKnown[16] = {};
    auto groundOf = [&](x86::reg32 car) {
        const x86::reg32 slot = (car - x86::reg32(0x5e1120)) / 0x9ac;
        if (car < 0x5e1120 || slot >= 16 || (car - x86::reg32(0x5e1120)) % 0x9ac)
            return f(car + 0x9c) - 0.6f;   // not one of the race's cars: under its centre
        if (!groundKnown[slot])
        {
            float low = 1e9f, at[3];
            for (int w = 0; w < 4; ++w)
            {
                wheelContact(app, car, w, at);
                low = SDL_min(low, at[1]);
            }
            groundY[slot] = low;
            groundKnown[slot] = true;
        }
        return groundY[slot];
    };
    const float light = flameLightSetting();
    /* The exhaust flames: each a ball that grows to its size in a fifth of its
     * second, holds there -- trembling, a little of the car's movement in it --
     * and is out by the end of it (native_exhausts.cpp lit them).  Added to the
     * picture rather than blended into it: light, not paint.  Each throws a pool
     * of its own light on the road under it (EffectParticle::Glow), as wide as
     * the ball is big and as bright as it burns. */
    for (const Flame& flame : s_flames)
    {
        EffectParticle e{};
        float z = 0.0f;
        /* Where it burns: the car's own space, turned into the world as the car
         * is now, so it stays at the pipe's mouth while the car goes. */
        float at[3];
        for (int i = 0; i < 3; ++i)
        {
            const x86::reg32 k = 4 * x86::reg32(i);
            at[i] = f(flame.car + 0x98 + k) + f(flame.car + 0xc0 + k) * flame.at[0]
                    + f(flame.car + 0xcc + k) * flame.at[1] + f(flame.car + 0xd8 + k) * flame.at[2];
        }
        if (!project(at, e.x, e.y, z) || z < 1.5f)
            continue;
        constexpr float grow = 0.18f;   // seconds to its size
        constexpr float hold = 0.80f;   // at its size, trembling, until this
        const float out = 1.0f - smoothStep(hold, flame.life, flame.age);   // and out
        const float size = flame.size * std::sqrt(SDL_min(1.0f, flame.age / grow)) * out;
        const float shake = scale * size * 0.12f / z;
        e.x += std::sin(flame.age * 47.0f + flame.seed * 39.0f) * shake;
        e.y += std::cos(flame.age * 59.0f + flame.seed * 23.0f) * shake;
        e.size = SDL_min(SDL_max(1.0f, scale * size / z), 400.0f);
        e.depth = z;
        e.r = flame.colour[0];
        e.g = flame.colour[1];
        e.b = flame.colour[2];
        e.a = SDL_min(1.0f, flame.age / 0.05f) * out * smoothStep(1.5f, 3.5f, z);
        e.seed = flame.seed;
        e.type = float(EffectParticle::Flame);
        e.tx = e.ty = -1.0f;
        s_sparks.push_back(e);
        if (!(light > 0.0f))
            continue;
        const float pool[3] = { at[0], groundOf(flame.car) + 0.04f, at[2] };
        EffectParticle glow{};
        float poolZ = 0.0f;
        if (!project(pool, glow.x, glow.y, poolZ) || poolZ < 1.5f)
            continue;
        glow.depth = poolZ;
        glow.size = SDL_min(scale * (0.35f + size * 5.0f) / poolZ, 400.0f);
        glow.r = flame.colour[0];
        glow.g = flame.colour[1];
        glow.b = flame.colour[2];
        /* Faint on purpose: light spilling out of a pipe onto the road is a
         * glow, not a puddle of paint -- a third of what the flame itself is
         * worth, so it reads as a tint under the car and nothing more. */
        glow.a = SDL_min(1.0f, flame.age / 0.08f) * out * SDL_min(light, 2.0f) * flame.light * 0.15f
                 * smoothStep(1.5f, 3.5f, poolZ);
        glow.seed = flame.seed;
        glow.type = float(EffectParticle::Glow);
        glow.tx = glow.ty = -1.0f;
        s_sparks.push_back(glow);
    }
    /* The brakes, at night (brakeGlowFrame): at each wheel a pool of their
     * light on the road where it touches (EffectParticle::Glow, as a flame's)
     * and a glow at the hub on the wheel's outer side -- round where the disc
     * faces the view and narrowed as it turns edge on, the renderer told how
     * much in tx -- dull red as it comes up, orange at full. */
    const float brakeLight = brakeGlowSetting();
    if (s_showBrakes && app->getMemory<x86::reg32>(brakes::kNight) != 0)
    {
        for (x86::reg32 k = 0; k < 16; ++k)
        {
            const float glow = s_brakeGlow[k];
            if (!(glow > 0.01f))
                continue;
            const x86::reg32 car = 0x5e1120 + k * 0x9ac;
            float centre[3], right[3], up[3], forward[3];
            for (int i = 0; i < 3; ++i)
            {
                const x86::reg32 o = 4 * x86::reg32(i);
                centre[i] = f(car + 0x98 + o);
                right[i] = f(car + 0xc0 + o);
                up[i] = f(car + 0xcc + o);
                forward[i] = f(car + 0xd8 + o);
            }
            auto heated = [glow](int i) { return brakes::kCool[i] + (brakes::kHot[i] - brakes::kCool[i]) * glow; };
            // The disc's face, the car's right, in the view's axes (no translation).
            const float face[3] = { right[0] * m[0] + right[1] * m[3] + right[2] * m[6],
                                    right[0] * m[1] + right[1] * m[4] + right[2] * m[7],
                                    right[0] * m[2] + right[1] * m[5] + right[2] * m[8] };
            for (int w = 0; w < 4; ++w)
            {
                float at[3];
                wheelContact(app, car, w, at);
                float ahead = 0.0f, side = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    ahead += (at[i] - centre[i]) * forward[i];
                    side += (at[i] - centre[i]) * right[i];
                }
                const float share = (ahead > 0.0f ? 1.0f : brakes::kRearShare) * glow * brakeLight;
                // The pool on the road.
                const float ground[3] = { at[0], at[1] + 0.04f, at[2] };
                EffectParticle pool{};
                float poolZ = 0.0f;
                if (project(ground, pool.x, pool.y, poolZ) && poolZ >= 1.5f)
                {
                    pool.depth = poolZ;
                    pool.size = SDL_min(SDL_max(1.0f, scale * brakes::kPoolRadius / poolZ), 300.0f);
                    pool.r = heated(0);
                    pool.g = heated(1);
                    pool.b = heated(2);
                    pool.a = SDL_min(1.0f, share * brakes::kPoolAlpha) * smoothStep(1.5f, 3.5f, poolZ);
                    pool.seed = float(w) * 0.25f;
                    pool.type = float(EffectParticle::Glow);
                    pool.tx = pool.ty = -1.0f;
                    s_sparks.push_back(pool);
                }
                // The hub.
                const float out = side < 0.0f ? -brakes::kHubOut : brakes::kHubOut;
                float hub[3];
                for (int i = 0; i < 3; ++i)
                    hub[i] = at[i] + up[i] * brakes::kHubHeight + right[i] * out;
                EffectParticle e{};
                float z = 0.0f;
                if (!project(hub, e.x, e.y, z) || z < 1.5f)
                    continue;
                // How round it is seen: its face against the way from the camera to it.
                const float view[3] = { hub[0] * m[0] + hub[1] * m[3] + hub[2] * m[6] + t[0],
                                        hub[0] * m[1] + hub[1] * m[4] + hub[2] * m[7] + t[1], z };
                const float length = SDL_sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                const float facing = SDL_fabsf(face[0] * view[0] + face[1] * view[1] + face[2] * view[2])
                                     / SDL_max(length, 1e-3f);
                e.depth = z;
                e.size = SDL_min(SDL_max(1.0f, scale * brakes::kDiscRadius / z), 200.0f);
                e.r = heated(0);
                e.g = heated(1);
                e.b = heated(2);
                e.a = SDL_min(1.0f, share * brakes::kHubAlpha) * smoothStep(1.5f, 3.5f, z);
                e.seed = float(w) * 0.25f;
                e.type = float(EffectParticle::BrakeGlow);
                e.tx = SDL_max(brakes::kFlatMost, facing);
                e.ty = 0.0f;
                s_sparks.push_back(e);
            }
        }
    }
    for (std::size_t k = 0; k < s_teeth.size(); ++k)
    {
        EffectParticle e{};
        float z = 0.0f;
        if (!project(s_teeth[k].data(), e.x, e.y, z))
            continue;
        // A spike and its belt, 12 cm, its quad round its middle, a little up.
        e.depth = z;
        e.size = SDL_max(1.0f, scale * 0.12f / z);
        e.y -= e.size * 0.5f;
        e.r = e.g = e.b = 1.0f;
        e.a = 1.0f;
        e.seed = float(k % 7) / 7.0f;
        e.type = float(EffectParticle::Spike);
        e.tx = e.ty = -1.0f;
        s_out.push_back(e);
    }
    /* The air trails: each strand's pieces between two frames' points a thin
     * streak, narrow at its newest end and wider towards its oldest, fading
     * with its age and its life. */
    for (int n = 0; trails && n < kStrands; ++n)
    {
        const Strand& strand = s_strands[n];
        const auto& points = strand.points;
        for (std::size_t k = 1; k < points.size(); ++k)
        {
            const TrailPoint& a = points[k - 1];
            const TrailPoint& b = points[k];
            const float age = float(clock - b.clock) / float(kTrailTicks);
            const float alpha = 0.6f * SDL_min(a.strength, b.strength) * (1.0f - age) * (1.0f - 0.5f * age);
            if (alpha < 0.01f || b.clock - a.clock > 20)
                continue;
            EffectParticle e{};
            float za = 0.0f, zb = 0.0f;
            if (!project(b.p, e.x, e.y, zb) || !project(a.p, e.tx, e.ty, za) || SDL_min(za, zb) < 1.5f)
                continue;
            e.depth = SDL_min(za, zb);
            constexpr float thick_overall = 2.0, thick_head = 0.3, thick_tail = 1.4;
            e.size = SDL_max(0.6f, scale * strand.width * 
                             thick_overall * (thick_head + thick_tail * age) / e.depth);
            e.r = e.g = e.b = 1.0f;
            e.a = alpha * smoothStep(1.5f, 3.0f, e.depth);
            e.type = float(EffectParticle::Streak);
            s_sparks.push_back(e);
        }
    }
}

/* Both views' particles to the renderer: the blended ones first, back to front
 * in each view, then the added ones. */
void handOver()
{
    std::vector<EffectParticle> all;
    all.reserve(s_out.size() + s_sparks.size());
    all.insert(all.end(), s_out.begin(), s_out.end());
    all.insert(all.end(), s_sparks.begin(), s_sparks.end());
    win32::glide2x::effectParticles(std::move(all));
}

}  // namespace

/* Whether the port draws the weather's flakes and drops itself (NFS_WEATHER_PARTICLES):
 * native_vertices.cpp leaves a particle that comes onto the camera out of the
 * game's splash when it does. */
bool ownWeather()
{
    return weatherSetting();
}

/* Whether the game's own particle sprites are drawn at all (NFS_GAME_SPRITES):
 * off by default, and native_vertices.cpp then draws none of them -- neither
 * the pool's quads (particlesNative) nor the drops on the view (dropsNative).
 * Every picture in the game's particle set is a 565 texel with no alpha of its
 * own and nothing cuts it out (TECHNICAL.md), and the pool turns each quad to
 * its particle's spin (+0x38, +0x3c), so each one was a turned square of
 * picture over the view, brighter or dimmer than what was behind it, the
 * flakes and drops that come onto the camera worst of all.  Nothing the port
 * needs goes through that path: the dust, the grass, the spray, the smoke, the
 * exhaust flames and the weather are all made and drawn by this file, and with
 * the game's kinds taken over (particleTakeOver) it has none of its own to draw.
 *
 * They are drawn anyway wherever the port leaves a kind to the game -- its own
 * particles off, one of their switches off, or split screen, where every kind
 * is the game's -- since something is better than nothing, the squares and all.
 * `NFS_GAME_SPRITES=1`/`on` brings them back everywhere, for a comparison. */
bool gameSprites(win32::WinApplication* app)
{
    static const bool forced = [] {
        const char* text = SDL_getenv("NFS_GAME_SPRITES");
        return text && (SDL_strcmp(text, "1") == 0 || SDL_strcasecmp(text, "on") == 0);
    }();
    return forced || setting() <= 0.0f || smokeSetting() <= 0.0f || !weatherSetting() || !spraySetting()
        || app->getMemory<x86::reg32>(0x6fd3b0) == 1;
}

void particlesFrame(win32::WinApplication* app, x86::reg32 view)
{
    s_frameOn = false;
    s_out.clear();
    s_sparks.clear();
    s_teeth.clear();
    /* The drops the game puts on the view (0x7f0bd0, 200 a player, drawn by
     * sub_4cb750): while the game's sprites are not drawn, one is a turned
     * rectangle of blurred picture over the view and nothing may show it, so
     * they are cleared once a frame and the splash that made them splashes
     * nothing -- whatever path drew them, the weather on the lens is the port's
     * own particles (throwWeather). */
    if (!gameSprites(app))
    {
        x86::reg32 live = 0;
        for (int player = 0; player < 2; ++player)
        {
            for (int drop = 0; drop < 200; ++drop)
            {
                const x86::reg32 address = 0x7f0bd0 + x86::reg32(player) * 0x3200 + x86::reg32(drop) * 0x40;
                if (app->getMemory<x86::reg32>(address) != 0)
                    ++live;
                app->getMemory<x86::reg32>(address) = 0;
            }
        }
        /* NFS_SPRITE_TRACE: how many the game had made, once each time there
         * are any -- the splash (sub_4ca500), and so the game's own particle
         * drawing, is what makes them. */
        static const bool traced = SDL_getenv("NFS_SPRITE_TRACE") != nullptr;
        static x86::reg32 s_wasLive = 0;
        if (traced && live != 0 && s_wasLive == 0)
            SDL_Log("[SPRITE] the drops on the view: %u live, cleared", unsigned(live));
        s_wasLive = live;
    }
    pursuitFrame(app, s_teeth);   // Hot Pursuit's spike strips, their spikes drawn with them
    s_showTrails = trailsOn() && app->getMemory<x86::reg32>(0x6fd3b0) != 1;
    s_showFlames = exhaustFlamesOn();
    s_showBrakes = brakeGlowSetting() > 0.0f;
    if (setting() <= 0.0f && s_teeth.empty() && !s_showTrails && !s_showFlames && !s_showBrakes)
        return;
    /* Where the camera is, for the weather to fall round: the view turns a
     * point p into p M + t (M at +0x44, t at +0x38), so the camera, at 0 in
     * the view, is -t M^T. */
    if (view)
    {
        const float* m = &app->getMemory<float>(view + 0x44);
        const float t[3] = { app->getMemory<float>(view + 0x38), app->getMemory<float>(view + 0x3c),
                             app->getMemory<float>(view + 0x40) };
        for (int i = 0; i < 3; ++i)
            s_camera[i] = -(t[0] * m[i * 3] + t[1] * m[i * 3 + 1] + t[2] * m[i * 3 + 2]);
        s_cameraKnown = true;
    }
    // The clock's state and what the cars and the engines make, once a frame.
    const Uint64 began = perfTracing() ? SDL_GetTicksNS() : 0;
    frameParticles(app);
    s_frameOn = true;
    projectParticles(app, view);
    std::sort(s_out.begin(), s_out.end(), deeper);
    handOver();
    if (perfTracing())
    {
        s_cost.frameMs += double(SDL_GetTicksNS() - began) * 1e-6;
        s_cost.most = SDL_max(s_cost.most, s_particles.size());
        static Uint64 said = 0;
        if (++s_cost.frames >= 30 && SDL_GetTicksNS() - said >= SDL_NS_PER_SECOND)
        {
            said = SDL_GetTicksNS();
            const double n = double(s_cost.frames);
            SDL_Log("[PERF] particles: %.2f ms a frame, the mirror's %.2f; %zu at most (%zu weather), %.0f added a "
                    "frame, %.0f of them in a full pool",
                    s_cost.frameMs / n, s_cost.mirrorMs / n, s_cost.most, s_weatherCount, double(s_cost.added) / n,
                    double(s_cost.replaced) / n);
            s_cost = ParticleCost{};
        }
    }
}

/* The rear-view mirror: the same particles, in the mirror's own view, so that
 * what burns or smokes behind the car is behind it in the mirror too.  Called
 * for a frame's views after the first (the mirror's record is the main one's
 * +0x54), once the main view has made them. */
void particlesMirrorFrame(win32::WinApplication* app, x86::reg32 view)
{
    if (!s_frameOn || !view)
        return;
    const Uint64 began = perfTracing() ? SDL_GetTicksNS() : 0;
    const std::size_t from = s_out.size();
    projectParticles(app, view);
    std::sort(s_out.begin() + std::ptrdiff_t(from), s_out.end(), deeper);
    handOver();
    if (perfTracing())
        s_cost.mirrorMs += double(SDL_GetTicksNS() - began) * 1e-6;
}

}  // namespace nfs3hp
