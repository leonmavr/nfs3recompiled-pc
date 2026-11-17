#include <nfs3hp.h>
#include "native_guest.h"
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>

/* Port: tyre marks that last, and look laid by a tyre (NFS_TYRE_MARKS).
 *
 * How the game keeps them.  Every piece of a tyre mark is a quad laid by
 * sub_4c6cb0 (sub_4db750 splits a wheel's step into pieces): kept in a ring of
 * blocks at [0x560110] (0x340 bytes each, [0x7d5758] of them in use, the one
 * being filled at [0x7d5754]), sixteen pieces a block -- +0xc how many, the
 * four corners of each from +0x10 (12 bytes each; the last step's two edges,
 * then this step's two in the other order), from +0x310 a byte each saying which
 * of skd0-skd3 it shows, from +0x320 two bytes each, the alpha at its start and
 * at its end.  sub_4c6f00 draws them.  Nothing ever fades: a piece is there
 * until its block is taken again, and the game had 64 blocks -- a few hard
 * corners' worth.
 *
 * Now (tools/apply_tyre_marks.py):
 *   - the ring is kBlocks blocks (the allocation at 0x4c70f5 and the two
 *     bounds at 0x4c6dfc and 0x4c6e16), enough for a lap of sliding;
 *   - each piece, as it is laid (skidLaid, at sub_4c6cb0's end), is darker and
 *     a little wider the faster the car laying it goes (the nearest car to the
 *     piece: kWidthSlow to kWidthFast of its width about its middle, kDarkSlow
 *     to kDarkFast times its alpha -- the game's was 28 of 255, hardly there, full at kFullSpeed), and uneven along its
 *     length -- each piece's darkness a little different, now and then a
 *     faint one, so the mark has gaps -- starting where the last one ended;
 *   - and every frame (skidFrame) a piece older than NFS_TYRE_MARKS_LIFE
 *     seconds (60) fades over its last kFadeSeconds and is gone.
 * NFS_TYRE_MARKS (on; 0/off: the game's, as they were). */
namespace nfs3hp
{

namespace
{

// The game's.
constexpr x86::reg32 kRing = 0x560110;          // the blocks
constexpr x86::reg32 kInUse = 0x7d5758;         // how many are in use
constexpr x86::reg32 kFilling = 0x7d5754;       // the one being filled
constexpr x86::reg32 kBlockSize = 0x340;
constexpr x86::reg32 kPieces = 0xc, kCorners = 0x10, kAlphas = 0x320;
constexpr x86::reg32 kPiecesABlock = 16;
constexpr x86::reg32 kRaceClock = 0x7d3684;      // 64 a second
constexpr x86::reg32 kCarList = 0x5efc48, kCarCount = 0x5efda0, kMaxCars = 16;
constexpr x86::reg32 kCarPosition = 0x98, kCarVelocity = 0xa4;

// The port's.
constexpr float kWidthSlow = 0.9f, kWidthFast = 1.15f;   // of the game's width
constexpr float kDarkSlow = 1.6f, kDarkFast = 3.4f;      // of the game's alpha (28 of 255: hardly seen)
constexpr float kFullSpeed = 40.0f;                      // m/s
constexpr float kUneven = 0.3f;                          // a piece as much lighter as this, at random
constexpr float kGapChance = 0.08f, kGapShare = 0.15f;   // now and then a faint piece
constexpr float kFadeSeconds = 6.0f;
constexpr float kLifeSeconds = 60.0f;                    // NFS_TYRE_MARKS_LIFE
constexpr float kHandbrakeDark = 1.7f;                   // a handbrake's marks this much darker
constexpr float kWetDark = 1.6f;                         // and every mark on a wet road (it darkens the road)
constexpr x86::reg32 kWeather = 0x6fd4cc;                // rain or snow
constexpr x86::reg32 kPlayerCar = 0x5efa48;

// The port's own marks, laid where the game lays none (skidPedals).
constexpr x86::reg32 kLayPiece = 0x4c6cb0;               // eax, edx the last and this pair; bl, cl the alphas; type pushed
constexpr x86::reg32 kLift = 0x540218;                   // what the game lifts a mark off the road by (it adds it itself)
constexpr x86::reg32 kThrottle = 0x79c339, kBrake = 0x79c33a, kActionBits = 0x79c33b;
constexpr x86::reg32 kCarRight = 0xc0, kCarForward = 0xd8, kCarAir = 0x164, kCarRevs = 0x590, kCarGear = 0x50b;
constexpr float kSpinThrottle = 0.6f, kSpinSpeed = 12.0f, kSpinRevs = 0.5f;   // a wheel spun up
constexpr float kLaunchSpeed = 8.0f;                     // m/s: a launch in first marks up to this
constexpr x86::reg8 kFirstGear = 2;                      // car+0x50b: 0 reverse, 1 neutral
constexpr float kBrakeHard = 0.55f, kBrakeSpeed = 3.0f;                       // a brake held on
constexpr x86::sreg32 kBrakeAfter = 64;                  // ticks of it first: a second
constexpr float kStep = 0.3f;                            // m between two of its pieces
constexpr x86::sreg32 kSpinEvery = 6;                    // ticks between two pieces laid on the spot
constexpr float kMarkHalfWidth = 0.11f;                  // m
constexpr x86::reg8 kHandbrakeBit = 0x04;                // in kActionBits (Space, seen in a trace)
constexpr x86::reg8 kOwnAlpha = 48;                      // before skidLaid's shading, as the game's are 28

}  // namespace

// The ring's size (tools/apply_tyre_marks.py's patches read it).
constexpr x86::reg32 kBlocks = 256;

namespace
{

struct Piece
{
    x86::sreg32 laid = -1;
    std::uint8_t alpha[2] = {};   // as the port made it
};
Piece s_pieces[kBlocks][kPiecesABlock];
float s_lastEnd = -1.0f;          // the last piece's end darkness, 0 to 1
float s_lastCorner[3] = {};       // where it ended (its fourth corner)
std::uint32_t s_seed = 0x9e3779b9u;

float random01()
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return float(s_seed & 0xffffff) / float(0x1000000);
}

float settingOf(const char* name, float otherwise)
{
    const char* v = SDL_getenv(name);
    if (!v || !*v || !SDL_strcasecmp(v, "on"))
        return otherwise;
    if (!SDL_strcasecmp(v, "off"))
        return 0.0f;
    return SDL_max(0.0f, float(SDL_atof(v)));
}

float lifeTicks()
{
    static const float life = settingOf("NFS_TYRE_MARKS_LIFE", kLifeSeconds) * 64.0f;
    return life;
}

float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

}  // namespace

bool tyreMarksOn()
{
    static const bool on = settingOf("NFS_TYRE_MARKS", 1.0f) > 0.0f;
    return on;
}

x86::reg32 tyreMarkBlocks()
{
    return tyreMarksOn() ? kBlocks : 64;
}

/* At sub_4c6cb0's end, after a piece is laid (its count just raised). */
void skidLaid(win32::WinApplication* app)
{
    if (!tyreMarksOn())
        return;
    const x86::reg32 ring = app->getMemory<x86::reg32>(kRing);
    const x86::reg32 index = app->getMemory<x86::reg32>(kFilling);
    if (!ring || index >= kBlocks)
        return;
    const x86::reg32 block = ring + index * kBlockSize;
    const x86::reg32 count = app->getMemory<x86::reg32>(block + kPieces);
    if (count == 0 || count > kPiecesABlock)
        return;
    const x86::reg32 piece = count - 1;
    const x86::reg32 corners = block + kCorners + piece * 48;
    float c[4][3];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            c[k][i] = app->getMemory<float>(corners + x86::reg32(k * 12 + i * 4));
    // The car laying it: the nearest to its new end.
    float speed = 0.0f, nearest = 1e9f;
    x86::reg32 nearestCar = 0;
    const x86::reg32 cars = SDL_min(app->getMemory<x86::reg32>(kCarCount), kMaxCars);
    for (x86::reg32 n = 0; n < cars; ++n)
    {
        const x86::reg32 car = app->getMemory<x86::reg32>(kCarList + 4 * n);
        if (!car)
            continue;
        float d2 = 0.0f, v2 = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            const float d = app->getMemory<float>(car + kCarPosition + 4 * x86::reg32(i)) - c[3][i];
            const float v = app->getMemory<float>(car + kCarVelocity + 4 * x86::reg32(i));
            d2 += d * d;
            v2 += v * v;
        }
        if (d2 < nearest)
        {
            nearest = d2;
            speed = std::sqrt(v2);
            nearestCar = car;
        }
    }
    const float fast = clamp01(speed / kFullSpeed);
    // A little wider the faster, about each edge pair's middle.
    const float width = kWidthSlow + (kWidthFast - kWidthSlow) * fast;
    for (int pair = 0; pair < 2; ++pair)
    {
        float* a = c[pair * 2];
        float* b = c[pair * 2 + 1];
        for (int i = 0; i < 3; ++i)
        {
            const float mid = 0.5f * (a[i] + b[i]), half = 0.5f * (b[i] - a[i]);
            a[i] = mid - half * width;
            b[i] = mid + half * width;
        }
    }
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 3; ++i)
            app->getMemory<float>(corners + x86::reg32(k * 12 + i * 4)) = c[k][i];
    // Darker the faster; uneven along its length, now and then all but a gap.
    float shade = (kDarkSlow + (kDarkFast - kDarkSlow) * fast) * (1.0f - kUneven * random01());
    // The first player's handbrake: much darker.
    if (nearestCar == app->getMemory<x86::reg32>(kPlayerCar) && (app->getMemory<x86::reg8>(kActionBits) & kHandbrakeBit))
        shade *= kHandbrakeDark;
    if (app->getMemory<x86::reg32>(kWeather) != 0)
        shade *= kWetDark;
    if (random01() < kGapChance)
        shade *= kGapShare;
    // Starting where the last one ended, if it carries on from it.
    float jump = 0.0f;
    for (int i = 0; i < 3; ++i)
        jump += (c[0][i] - s_lastCorner[i]) * (c[0][i] - s_lastCorner[i]);
    const bool carriesOn = s_lastEnd >= 0.0f && jump < 0.04f;   // within 20 cm: the same wheel's
    const x86::reg32 alphas = block + kAlphas + piece * 2;
    const float was0 = float(app->getMemory<x86::reg8>(alphas)), was1 = float(app->getMemory<x86::reg8>(alphas + 1));
    const float start = carriesOn ? s_lastEnd : shade;
    Piece& p = s_pieces[index][piece];
    p.alpha[0] = std::uint8_t(std::lround(SDL_min(255.0f, was0 * start)));
    p.alpha[1] = std::uint8_t(std::lround(SDL_min(255.0f, was1 * shade)));
    p.laid = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    app->getMemory<x86::reg8>(alphas) = p.alpha[0];
    app->getMemory<x86::reg8>(alphas + 1) = p.alpha[1];
    s_lastEnd = shade;
    for (int i = 0; i < 3; ++i)
        s_lastCorner[i] = c[3][i];
    static const bool traced = SDL_getenv("NFS_TYRE_MARKS_TRACE") != nullptr;
    if (traced)
        SDL_Log("[SKID] t %d block %u piece %u speed %.1f width %.2f alpha %u-%u (was %.0f-%.0f)%s", int(p.laid), unsigned(index),
                unsigned(piece), speed, width, unsigned(p.alpha[0]), unsigned(p.alpha[1]), was0, was1, carriesOn ? " on" : "");
}

/* Each frame: the pieces past their life fade and go. */
void skidFrame(win32::WinApplication* app)
{
    if (!tyreMarksOn())
        return;
    const x86::reg32 ring = app->getMemory<x86::reg32>(kRing);
    if (!ring)
        return;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    static x86::sreg32 last = 0;
    if (clock < last)
        for (auto& block : s_pieces)
            for (Piece& p : block)
                p.laid = -1;   // a new race
    last = clock;
    const float life = lifeTicks();
    if (life <= 0.0f)
        return;
    const float fade = kFadeSeconds * 64.0f;
    const x86::reg32 inUse = SDL_min(app->getMemory<x86::reg32>(kInUse), kBlocks);
    for (x86::reg32 b = 0; b < inUse; ++b)
    {
        const x86::reg32 block = ring + b * kBlockSize;
        const x86::reg32 count = SDL_min(app->getMemory<x86::reg32>(block + kPieces), kPiecesABlock);
        for (x86::reg32 k = 0; k < count; ++k)
        {
            Piece& p = s_pieces[b][k];
            if (p.laid < 0)
                continue;
            const float age = float(clock - p.laid);
            if (age < life - fade)
                continue;
            const float keep = clamp01((life - age) / fade);
            const x86::reg32 alphas = block + kAlphas + k * 2;
            app->getMemory<x86::reg8>(alphas) = x86::reg8(std::lround(float(p.alpha[0]) * keep));
            app->getMemory<x86::reg8>(alphas + 1) = x86::reg8(std::lround(float(p.alpha[1]) * keep));
        }
    }
}


void wheelContact(win32::WinApplication* app, x86::reg32 car, int wheel, float at[3]);  // native_particles.cpp
int groundUnder(win32::WinApplication* app, x86::reg32 car, int wheel);            // native_surface.cpp

namespace
{
/* The engine's redline in rpm, as native_particles.cpp's redline reads it:
 * the car's data table at [car+0x528], +0x1c4; 0 where that is no table. */
x86::reg32 redline(win32::WinApplication* app, x86::reg32 car)
{
    const x86::reg32 table = app->getMemory<x86::reg32>(car + 0x528);
    if (table < 0x400000 || table >= 0x11000000)
        return 0;
    const x86::sreg32 rpm = app->getMemory<x86::sreg32>(table + 0x1c4);
    return (rpm >= 2000 && rpm <= 20000) ? x86::reg32(rpm) : 0;
}

struct Wheel
{
    bool laid = false;
    float pair[6] = {};          // the last piece's end: its two edges
    x86::sreg32 at = -1000;      // when
};
Wheel s_wheels[4];
x86::sreg32 s_braking = -1;      // since when the brake has been held on
x86::reg32 s_pedalCar = 0;
}  // namespace

/* Marks the game lays none of (from collisionEffects, each car, each frame):
 * for the first player's car, a wheel spun up on the spot -- throttle down, in
 * gear, revs up and the car slow -- marks the road under the driven wheels, a
 * short dark patch laid again and again where it stands; and the brake held on
 * for a second while the car moves marks it under all four.  Each piece is the
 * game's own (sub_4c6cb0), shaded by skidLaid as every piece is.  False when
 * the game stopped in it. */
bool skidPedals(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 car)
{
    if (!tyreMarksOn() || car != app->getMemory<x86::reg32>(0x5efa48) || app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return true;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    if (car != s_pedalCar || clock < s_braking)
    {
        s_pedalCar = car;
        for (Wheel& w : s_wheels)
            w = Wheel{};
        s_braking = -1;
    }
    auto f = [app](x86::reg32 a) { return float(app->getMemory<float>(a)); };
    const float throttle = float(app->getMemory<x86::reg8>(kThrottle)) / 255.0f;
    const float brake = float(app->getMemory<x86::reg8>(kBrake)) / 255.0f;
    float speed = 0.0f;
    for (int i = 0; i < 3; ++i)
        speed += f(car + kCarVelocity + 4 * x86::reg32(i)) * f(car + kCarVelocity + 4 * x86::reg32(i));
    speed = std::sqrt(speed);
    static const bool traced = SDL_getenv("NFS_TYRE_MARKS_TRACE") != nullptr;
    const bool grounded = app->getMemory<uint16_t>(car + kCarAir) == 0;
    const x86::reg32 limit = redline(app, car);
    if (traced && clock % 16 == 0)
        SDL_Log("[SKIDPEDAL] t %d throttle %.2f brake %.2f bits %02x speed %.1f gear %u revs %d of %u", int(clock),
                throttle, brake, unsigned(app->getMemory<x86::reg8>(kActionBits)), speed,
                unsigned(app->getMemory<x86::reg8>(car + kCarGear)), int(app->getMemory<x86::sreg32>(car + kCarRevs)),
                unsigned(limit));
    /* Spinning: revved up and hardly moving -- or pulling away in first on
     * full throttle, which the game's revs follow the speed for (a launch's
     * own marks, 2026-11-17). */
    const x86::reg8 gear = app->getMemory<x86::reg8>(car + kCarGear);
    const bool revved = gear >= kFirstGear && speed < kSpinSpeed && limit
                        && float(app->getMemory<x86::sreg32>(car + kCarRevs)) > kSpinRevs * float(limit);
    const bool launching = gear == kFirstGear && speed < kLaunchSpeed;
    const bool spinning = grounded && throttle > kSpinThrottle && (revved || launching);
    if (grounded && brake > kBrakeHard && speed > kBrakeSpeed)
    {
        if (s_braking < 0)
            s_braking = clock;
    }
    else
        s_braking = -1;
    const bool braking = s_braking >= 0 && clock - s_braking >= kBrakeAfter;
    if (!spinning && !braking)
    {
        for (Wheel& w : s_wheels)
            w.laid = false;
        return true;
    }
    float right[3], forward[3], centre[3];
    for (int i = 0; i < 3; ++i)
    {
        right[i] = f(car + kCarRight + 4 * x86::reg32(i));
        forward[i] = f(car + kCarForward + 4 * x86::reg32(i));
        centre[i] = f(car + kCarPosition + 4 * x86::reg32(i));
    }
    for (int n = 0; n < 4; ++n)
    {
        if (groundUnder(app, car, n) != 0)
            continue;   // pavement only
        float at[3];
        wheelContact(app, car, n, at);
        const float ahead = (at[0] - centre[0]) * forward[0] + (at[1] - centre[1]) * forward[1] + (at[2] - centre[2]) * forward[2];
        if (spinning && !braking && ahead > 0.0f)
            continue;   // a spin marks under the driven end (the rear)
        Wheel& w = s_wheels[n];
        float pair[6];
        for (int i = 0; i < 3; ++i)
        {
            pair[i] = at[i] - right[i] * kMarkHalfWidth;
            pair[3 + i] = at[i] + right[i] * kMarkHalfWidth;
        }
        float moved = 0.0f;
        for (int i = 0; i < 3; ++i)
            moved += (pair[i] - w.pair[i]) * (pair[i] - w.pair[i]);
        moved = std::sqrt(moved);
        float from[6];
        if (!w.laid || moved > 3.0f)
        {
            // A first piece: a short one about where it is.
            for (int i = 0; i < 3; ++i)
            {
                from[i] = pair[i] - forward[i] * kStep;
                from[3 + i] = pair[3 + i] - forward[i] * kStep;
            }
        }
        else if (moved >= kStep)
            SDL_memcpy(from, w.pair, sizeof from);
        else if (spinning && clock - w.at >= kSpinEvery)
        {
            // On the spot: the same patch laid again, a little darker each time.
            for (int i = 0; i < 3; ++i)
            {
                from[i] = pair[i] - forward[i] * kStep;
                from[3 + i] = pair[3 + i] - forward[i] * kStep;
            }
        }
        else
            continue;
        const x86::reg32 scratch = cpu.esp - 0x400;
        for (int i = 0; i < 6; ++i)
        {
            app->getMemory<float>(scratch + 4 * x86::reg32(i)) = from[i];
            app->getMemory<float>(scratch + 0x18 + 4 * x86::reg32(i)) = pair[i];
        }
        const x86::reg32 saved[] = { cpu.eax, cpu.ebx, cpu.ecx, cpu.edx, cpu.esi, cpu.edi, cpu.ebp };
        cpu.eax = scratch;
        cpu.edx = scratch + 0x18;
        cpu.ebx = kOwnAlpha;
        cpu.ecx = kOwnAlpha;
        if (!guest::call(app, cpu, kLayPiece, { 1 }))
            return false;
        cpu.eax = saved[0]; cpu.ebx = saved[1]; cpu.ecx = saved[2]; cpu.edx = saved[3];
        cpu.esi = saved[4]; cpu.edi = saved[5]; cpu.ebp = saved[6];
        w.laid = true;
        w.at = clock;
        SDL_memcpy(w.pair, pair, sizeof pair);
    }
    return true;
}

}  // namespace nfs3hp
