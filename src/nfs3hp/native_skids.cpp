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
 *     bounds at 0x4c6dfc and 0x4c6e16): one ring, shared by every car and
 *     written over in turn, so it has to hold what the opponents and the cops
 *     lay as well as the player's -- eight thousand pieces, a couple of minutes
 *     of a whole field sliding, where the 256 it had first went in seconds;
 *   - each piece, as it is laid (skidLaid, at sub_4c6cb0's end), is darker and
 *     a little wider the faster the car laying it goes (the nearest car to the
 *     piece: kWidthSlow to kWidthFast of its width about its middle, kDarkSlow
 *     to kDarkFast times its alpha -- the game's was 28 of 255, hardly there, full at kFullSpeed), and uneven along its
 *     length -- each piece's darkness a little different, now and then a
 *     faint one, so the mark has gaps -- starting where the last one ended;
 *   - and every frame (skidFrame) a piece older than NFS_TYRE_MARKS_LIFE
 *     seconds (60) fades over its last kFadeSeconds and is gone;
 *   - and every piece laid is a tyre scrubbing, so the port's smoke comes off
 *     where the mark is (tyreMarkSmoke, native_particles.cpp) -- whether the
 *     port's own marks are on or not.
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
constexpr x86::reg32 kPieces = 0xc, kCorners = 0x10, kAlphas = 0x320, kTypes = 0x310;
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
constexpr float kBrakeHard = 0.5f, kBrakeSpeed = 3.0f;                        // a brake held on
constexpr x86::sreg32 kBrakeAfter = 4;                   // ticks of it first: a sixteenth of a second, not a tap
constexpr float kStep = 0.3f;                            // m between two of its pieces
constexpr x86::sreg32 kSpinEvery = 6;                    // ticks between two pieces laid on the spot
constexpr float kMarkHalfWidth = 0.11f;                  // m
constexpr x86::reg8 kHandbrakeBit = 0x04;                // in kActionBits (Space, seen in a trace)
constexpr x86::reg8 kOwnAlpha = 48;                      // before skidLaid's shading, as the game's are 28

/* What each kind of ground leaves (groundAt's 0 to 5): on, the rut's own
 * colour (RRGGBB -- the piece's per-vertex RGB, which the renderer turns the
 * mark's black picture into), and how much of the asphalt's alpha, width and
 * life.  A loose surface takes a lesser, narrower mark that fades sooner. */
struct SurfaceMark
{
    bool on;
    x86::reg32 tint;
    float alpha, width, life;
};
constexpr SurfaceMark kSurfaceMarks[6] = {
    { true, 0x000000, 1.0f, 1.0f, 1.0f },    // pavement: the black rubber it is
    { true, 0x223d18, 0.3f, 0.8f, 0.33f },   // grass: a dark green flattened strip
    { true, 0x6a3d1c, 0.5f, 0.8f, 0.5f },    // sand, dirt, gravel: burnt orange-brown
    { true, 0xb9c6cf, 0.2f, 0.8f, 0.15f },   // snow: a pale line, briefly
    { true, 0x2e2114, 0.7f, 0.9f, 0.4f },    // mud: a wet, nearly black brown
    { true, 0x4a3a2a, 0.4f, 0.8f, 0.5f },    // other ground: a faint brown-grey
};

}  // namespace

/* The ring's size (tools/apply_tyre_marks.py's patches read it).  The game's
 * 64 blocks held a few seconds of a whole field dragging its tyres; the ring is
 * one for every car and is written over first-in-first-out, so the opponents'
 * and the cops' churn filled the port's 256 in seconds too, and their marks
 * went long before the 60 their life allows.  8192 blocks -- some 6.5 MB of the
 * guest's memory, freed at the end of a race -- hold two minutes of four cars
 * sliding at racing speed, so the life is what ends a mark now. */
constexpr x86::reg32 kBlocks = 8192;

namespace
{

struct Piece
{
    x86::sreg32 laid = -1;
    std::uint8_t alpha[2] = {};   // as the port made it
    std::uint8_t tint = 0;        // kSurfaceMarks' kind (the piece's +0x310 high bits)
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

/* From native_particles.cpp: a tyre mark has been laid, so the tyre is being
 * scrubbed and its smoke comes off there (at, and the car laying it). */
void tyreMarkSmoke(win32::WinApplication* app, x86::reg32 car, const float at[3], float speed);
/* And what the ground is at a point: 0 pavement, 1 grass, 2 sand/dirt/gravel,
 * 3 snow, 4 mud, 5 other, -1 for no wheel near -- the surface mark's kind. */
int groundAt(win32::WinApplication* app, const float at[3]);

bool tyreMarksOn()
{
    static const bool on = settingOf("NFS_TYRE_MARKS", 1.0f) > 0.0f;
    return on;
}

x86::reg32 tyreMarkBlocks()
{
    return tyreMarksOn() ? kBlocks : 64;
}

/* The RGB the mark's picture is drawn in, for the piece sub_4c6f00 is drawing
 * now: frame is that function's ebp, whose [ebp-0xc] is the block's +0x310
 * plus the piece -- the byte this port put `kind << 2 | shape` into.  Patched
 * in at 0x4c7068 and 0x4c708a (tools/apply_tyre_marks.py). */
x86::reg32 markTint(win32::WinApplication* app, x86::sreg32 frame)
{
    const x86::reg32 type = app->getMemory<x86::reg32>(frame - 0xc);
    const x86::reg8 byte = app->getMemory<x86::reg8>(type);
    const x86::reg32 kind = x86::reg32(byte >> 2);
    return kSurfaceMarks[kind < 6 ? kind : 0].tint;
}

/* At sub_4c6cb0's end, after a piece is laid (its count just raised). */
void skidLaid(win32::WinApplication* app)
{
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
    /* Smoke off the scrubbing tyre, where the mark is (tyreMarkSmoke): the
     * port marks a sliding wheel of every car (skidPedals) and the game its
     * own, so this is what makes the smoke follow the marks -- and the squeak
     * -- and not only the first player's pedals. */
    if (nearestCar)
        tyreMarkSmoke(app, nearestCar, c[3], speed);
    if (!tyreMarksOn())
        return;
    const int atKind = groundAt(app, c[3]);
    const int kind = atKind < 0 ? 0 : atKind;
    const SurfaceMark& mark = kSurfaceMarks[kind];
    const float fast = clamp01(speed / kFullSpeed);
    // A little wider the faster, about each edge pair's middle; a rut a little
    // narrower than a tyre's black smear.
    const float width = (kWidthSlow + (kWidthFast - kWidthSlow) * fast) * mark.width;
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
    float shade = (kDarkSlow + (kDarkFast - kDarkSlow) * fast) * (1.0f - kUneven * random01()) * mark.alpha;
    // The first player's handbrake: much darker.
    if (nearestCar == app->getMemory<x86::reg32>(kPlayerCar) && (app->getMemory<x86::reg8>(kActionBits) & kHandbrakeBit))
        shade *= kHandbrakeDark;
    if (app->getMemory<x86::reg32>(kWeather) != 0)
        shade *= kWetDark;
    if (random01() < kGapChance)
        shade *= kGapShare;
    /* The rut's colour: the surface's kind in the six high bits of the piece's
     * +0x310 byte, whose low two are which skd picture to draw (markTint reads
     * it back at 0x4c7068). */
    const x86::reg32 types = block + kTypes + piece;
    const x86::reg8 shape = x86::reg8(app->getMemory<x86::reg8>(types) & 3);
    app->getMemory<x86::reg8>(types) = x86::reg8(shape | (x86::reg8(kind) << 2));
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
    p.tint = std::uint8_t(kind);
    p.laid = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    app->getMemory<x86::reg8>(alphas) = p.alpha[0];
    app->getMemory<x86::reg8>(alphas + 1) = p.alpha[1];
    s_lastEnd = shade;
    for (int i = 0; i < 3; ++i)
        s_lastCorner[i] = c[3][i];
    static const bool traced = SDL_getenv("NFS_TYRE_MARKS_TRACE") != nullptr;
    if (traced)
        SDL_Log("[SKID] t %d block %u piece %u speed %.1f width %.2f alpha %u-%u (was %.0f-%.0f) ground %d tint %06x%s",
                int(p.laid), unsigned(index), unsigned(piece), speed, width, unsigned(p.alpha[0]), unsigned(p.alpha[1]),
                was0, was1, kind, unsigned(mark.tint), carriesOn ? " on" : "");
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
            const float pieceLife = life * kSurfaceMarks[p.tint < 6 ? p.tint : 0].life;
            if (age < pieceLife - fade)
                continue;
            const float keep = clamp01((pieceLife - age) / fade);
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
Wheel s_wheels[16][4];           // the first player's pedals, and every car's slide
x86::sreg32 s_braking[16] = {};  // since when the brake has been held on, a car
constexpr float kSlideSpeed = 3.0f;      // m/s: a car slower than this is not sliding
constexpr float kSlideLateral = 1.2f;    // m/s across the car: the tyres are dragged

/* A piece of the port's own mark for one wheel, from where its last one ended
 * to where it is now -- 30 cm, or a short first one.  hold: lay the same patch
 * again where the wheel spins on the spot.  False when the game stopped in it. */
bool layOwnPiece(win32::WinApplication* app, x86::CPU& cpu, Wheel& w, const float right[3], const float forward[3],
                 const float at[3], bool hold, x86::sreg32 clock)
{
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
    else if (hold && clock - w.at >= kSpinEvery)
    {
        // On the spot: the same patch laid again, a little darker each time.
        for (int i = 0; i < 3; ++i)
        {
            from[i] = pair[i] - forward[i] * kStep;
            from[3 + i] = pair[3 + i] - forward[i] * kStep;
        }
    }
    else
        return true;   // not far enough yet
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
    return true;
}
}  // namespace

/* Marks the game lays none of (from collisionEffects, each car, each frame).
 * For the first player's car: a wheel spun up on the spot -- throttle down, in
 * gear, revs up and the car slow -- marks the road under the driven wheels, a
 * short dark patch laid again and again where it stands; the brake past half
 * while the car moves marks it under all four, and the handbrake under the rear
 * two.  And every car, the opponents' and the cops' too, **sliding** -- its
 * velocity across it over a metre a second -- marks the road under each wheel
 * on any ground the table below allows: the game lays the AI's slides nowhere,
 * so without this their tyres left no trace at all.  Each piece is the game's
 * own (sub_4c6cb0), shaded by skidLaid as every piece is.  False when the game
 * stopped in it. */
bool skidPedals(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 car)
{
    if (!tyreMarksOn() || app->getMemory<x86::reg32>(0x6fd3b0) == 1)
        return true;
    if (car < 0x5e1120 || car >= 0x5e1120 + 16 * 0x9ac || (car - x86::reg32(0x5e1120)) % 0x9ac)
        return true;
    const x86::reg32 slot = (car - x86::reg32(0x5e1120)) / 0x9ac;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    static x86::sreg32 s_lastClock = 0;
    if (clock < s_lastClock)
    {
        for (auto& wheels : s_wheels)
            for (Wheel& w : wheels)
                w = Wheel{};
        for (x86::sreg32& braking : s_braking)
            braking = 0;
        s_lastClock = clock;
    }
    auto f = [app](x86::reg32 a) { return float(app->getMemory<float>(a)); };
    const bool player = car == app->getMemory<x86::reg32>(0x5efa48);
    const bool grounded = app->getMemory<uint16_t>(car + kCarAir) == 0;
    const float velocity[3] = { f(car + kCarVelocity), f(car + kCarVelocity + 4), f(car + kCarVelocity + 8) };
    const float speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    float right[3], forward[3], centre[3];
    for (int i = 0; i < 3; ++i)
    {
        right[i] = f(car + kCarRight + 4 * x86::reg32(i));
        forward[i] = f(car + kCarForward + 4 * x86::reg32(i));
        centre[i] = f(car + kCarPosition + 4 * x86::reg32(i));
    }
    if (!player)
    {
        /* An opponent's or a cop's slide: the velocity across the car over a
         * metre a second means the tyres are dragged over the road, and every
         * wheel on a ground with a mark lays it. */
        const float lateral = SDL_fabsf(velocity[0] * right[0] + velocity[1] * right[1] + velocity[2] * right[2]);
        if (!grounded || speed < kSlideSpeed || lateral < kSlideLateral)
        {
            for (Wheel& w : s_wheels[slot])
                w.laid = false;
            return true;
        }
        for (int n = 0; n < 4; ++n)
        {
            const int kind = groundUnder(app, car, n);
            if (!kSurfaceMarks[kind].on)
                continue;   // a pavement slide, or a ground with no mark of its own
            float at[3];
            wheelContact(app, car, n, at);
            if (!layOwnPiece(app, cpu, s_wheels[slot][n], right, forward, at, false, clock))
                return false;
        }
        return true;
    }
    const float throttle = float(app->getMemory<x86::reg8>(kThrottle)) / 255.0f;
    const float brake = float(app->getMemory<x86::reg8>(kBrake)) / 255.0f;
    static const bool traced = SDL_getenv("NFS_TYRE_MARKS_TRACE") != nullptr;
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
    /* The brake past half, or the handbrake (which locks the rear wheels
     * alone), the car moving: marks almost at once (2026-11-17: after a second
     * of it, they were hardly ever laid). */
    const bool handbrake = (app->getMemory<x86::reg8>(kActionBits) & kHandbrakeBit) != 0;
    const bool pedal = brake > kBrakeHard;
    if (grounded && (pedal || handbrake) && speed > kBrakeSpeed)
    {
        if (!s_braking[slot])
            s_braking[slot] = clock;
    }
    else
        s_braking[slot] = 0;
    const bool braking = s_braking[slot] && clock - s_braking[slot] >= kBrakeAfter;
    if (!spinning && !braking)
    {
        for (Wheel& w : s_wheels[slot])
            w.laid = false;
        return true;
    }
    for (int n = 0; n < 4; ++n)
    {
        const int kind = groundUnder(app, car, n);
        if (!kSurfaceMarks[kind].on)
            continue;   // a ground with no mark of its own
        float at[3];
        wheelContact(app, car, n, at);
        const float ahead = (at[0] - centre[0]) * forward[0] + (at[1] - centre[1]) * forward[1] + (at[2] - centre[2]) * forward[2];
        if (spinning && !braking && ahead > 0.0f)
            continue;   // a spin marks under the driven end (the rear)
        if (braking && !pedal && !spinning && ahead > 0.0f)
            continue;   // the handbrake alone locks the rear wheels
        if (!layOwnPiece(app, cpu, s_wheels[slot][n], right, forward, at, spinning, clock))
            return false;
    }
    return true;
}

}  // namespace nfs3hp
