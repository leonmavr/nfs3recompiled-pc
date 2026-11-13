#include <nfs3hp.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

/* Port: the race's dust, grit and sparks, the port's own (NFS_PARTICLES).
 *
 * The game makes a particle with sub_4ca850 (its kind in eax, thirteen
 * arguments, ret 0x34: where, three floats; a scale; two directions of three;
 * how fast, three) into its pool of 600 (0x7ddfd0) and draws each as a
 * picture from gamedata/render/pc/sfx.fsh: kind 2 the dust and dirt thrown up
 * off the road (grey, the picture's colour), kind 3 a spark of a collision
 * (thirty to a burst).  particleSpawn (native_effects.cpp, hooked at its top)
 * hands those two kinds here instead, and the game's are not made:
 *
 * - The dirt becomes grains -- small clumps with ragged edges, thrown up and
 *   out, falling back under gravity, settling where they land -- and now and
 *   then a puff of fine dust that hangs, spreads and fades.  How many to each
 *   of the game's grows with the speed it was thrown at (1.5 and one more for
 *   every 4 m/s, times NFS_PARTICLES), and how high and far they go with it.
 *   Their colour is the ground's they came from: the renderer takes it from
 *   the picture where they were thrown (sand on a desert, snow on snow, mud).
 * - A spark is a short white-hot streak that cools through yellow and orange
 *   to red as it falls, thrown at the game's speed with a little scatter.
 *
 * Each frame (as the main view's sky is drawn, its view known: particlesFrame)
 * they are moved by the race's clock -- standing with it -- turned into the
 * view and handed to the renderer (glide2x::effectParticles), which draws them
 * over the world before its whole-picture pass (ThrashRenderer::drawParticles),
 * each one's shape made from its seed rather than a picture.  Not in split
 * screen, where there is more than one view, nor without that renderer: the
 * game's own then. */
namespace nfs3hp
{

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
    int type;              // EffectParticle::Type
};

constexpr std::size_t kMost = 2400;
std::vector<Particle> s_particles;
std::uint32_t s_random = 0x9e3779b9u;
x86::sreg32 s_clock = -1;

float random01()
{
    s_random ^= s_random << 13;
    s_random ^= s_random >> 17;
    s_random ^= s_random << 5;
    return float(s_random >> 8) / 16777216.0f;
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

float argument(win32::WinApplication* app, x86::CPU& cpu, int k)
{
    return app->getMemory<float>(cpu.esp + 4 + 4 * x86::reg32(k));
}

void add(const Particle& particle)
{
    if (s_particles.size() >= kMost)
        s_particles.erase(s_particles.begin());   // the oldest makes room
    s_particles.push_back(particle);
}

void throwDirt(const float at[3], const float velocity[3])
{
    const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    const float pace = SDL_min(speed / 30.0f, 1.5f);
    const int grains = int((2.0f + speed / 3.0f) * setting() + random01());
    for (int k = 0; k < SDL_min(grains, 18); ++k)
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
        g.size = randomRange(0.015f, 0.045f) * (random01() < 0.12f ? 1.8f : 1.0f);
        g.seed = random01();
        g.alpha = 1.0f;
        g.colour[0] = 0.62f; g.colour[1] = 0.52f; g.colour[2] = 0.40f;
        g.type = EffectParticle::Grain;
        add(g);
    }
    // Fine dust: more of it the faster.
    if (random01() < (0.45f + speed / 30.0f) * setting())
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
    for (Particle& q : s_particles)
    {
        q.age += dt;
        if (q.settled)
            continue;
        const float drag = q.type == EffectParticle::Puff ? 1.6f : q.type == EffectParticle::Spark ? 0.4f : 0.9f;
        const float gravity = q.type == EffectParticle::Puff ? -0.15f : 9.8f;
        const float keep = std::exp(-drag * dt);
        for (int i = 0; i < 3; ++i)
            q.v[i] *= keep;
        q.v[1] -= gravity * dt;
        for (int i = 0; i < 3; ++i)
            q.p[i] += q.v[i] * dt;
        q.size += q.grow * dt;
        if (q.type != EffectParticle::Puff && q.p[1] < q.floor && q.v[1] < 0.0f)
        {
            q.p[1] = q.floor;
            if (q.type == EffectParticle::Grain)
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
bool particleTakeOver(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 kind = cpu.eax;
    if ((kind != 2 && kind != 3) || setting() <= 0.0f || !win32::glide2x::effectParticlesDrawn())
        return false;
    if (app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return false;   // split screen: the game's own
    const float at[3] = { argument(app, cpu, 0), argument(app, cpu, 1), argument(app, cpu, 2) };
    const float velocity[3] = { argument(app, cpu, 10), argument(app, cpu, 11), argument(app, cpu, 12) };
    if (kind == 2)
        throwDirt(at, velocity);
    else
        throwSpark(at, velocity);
    cpu.eax = 0;
    cpu.esp += 4 + 0x34;
    return true;
}

/* Each frame, the main view's (view: its camera at +0x38, matrix at +0x44, as
 * sub_492370 turns a point into it; the projection at [0x56009c..a8]). */
void particlesFrame(win32::WinApplication* app, x86::reg32 view)
{
    if (setting() <= 0.0f)
        return;
    auto f = [app](x86::reg32 address) { return app->getMemory<float>(address); };
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(0x7d3684));
    if (s_clock < 0 || clock < s_clock || clock - s_clock > 64)
        s_clock = clock;   // a new race, or a jump: nothing is moved by it
    if (clock < 2)
        s_particles.clear();
    const float dt = float(clock - s_clock) / 64.0f;
    s_clock = clock;
    if (dt > 0.0f)
        move(dt);

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
    std::vector<EffectParticle> out, sparks;
    out.reserve(s_particles.size());
    const float scale = std::fabs(k1);
    for (Particle& q : s_particles)
    {
        EffectParticle e{};
        float z = 0.0f;
        if (!project(q.p, e.x, e.y, z))
        {
            q.hasLast = false;
            continue;
        }
        e.depth = z;
        e.seed = q.seed;
        e.type = float(q.type);
        const float left = 1.0f - q.age / q.life;
        if (q.type == EffectParticle::Spark)
        {
            // White hot to yellow, orange and a dull red as it cools.
            const float heat = left;
            e.r = 1.0f;
            e.g = SDL_min(1.0f, 0.25f + 0.85f * heat);
            e.b = SDL_max(0.0f, heat * heat * 0.8f - 0.1f);
            e.a = SDL_min(1.0f, left * 1.6f);
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
            sparks.push_back(e);
            continue;
        }
        e.size = SDL_min(scale * q.size / z, 400.0f);
        if (e.size < 0.4f)
            continue;
        e.r = q.colour[0];
        e.g = q.colour[1];
        e.b = q.colour[2];
        const float in = SDL_min(1.0f, q.age / 0.08f);
        e.a = q.alpha * in * (q.type == EffectParticle::Puff ? left * left : SDL_min(1.0f, left * 3.0f));
        float oz;
        if (!project(q.origin, e.tx, e.ty, oz))
            e.tx = e.ty = -1.0f;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const EffectParticle& a, const EffectParticle& b) { return a.depth > b.depth; });
    out.insert(out.end(), sparks.begin(), sparks.end());
    win32::glide2x::effectParticles(std::move(out));
}

}  // namespace nfs3hp
