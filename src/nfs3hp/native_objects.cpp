#include <nfs3hp.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <vector>

/* Port: trees and lamp posts knocked down like the signs (NFS_KNOCK_TREES).
 *
 * How the game knocks a sign down.  A car's touch of a track object
 * (sub_430da0) asks sub_41d350(car, index) what to do; index is the object's
 * place among its block's REFXOBJ records ([[car+0xc]+0x5a4], 20 bytes each:
 * the point it stands on at +0, its global number at +0xe, its kind at +0x13).
 * Each global number has three bytes at 0x5dd230: 0xff standing, 0xfe lying
 * (the way it fell in the second byte, its yaw in the third), anything else
 * the slot of the animation it is flying through.  Kind 2 is a sign: standing
 * and hit at 11 (|vx| + |vz|, m/s; a cop at any speed) or more, it is given an
 * animation slot (sub_40f3b0: 24 bytes from 0x56f234 -- the animation, its
 * start on the race clock, its flags), keyframed, the slow one (2) below 25
 * and the fast one (3) above; and the answer is -1, nothing to bounce off.
 * Below that speed, and for kind 1 -- everything else solid, the trees and
 * lamp posts among it -- the answer is 1: the car bounces off.  The game never
 * stands a sign up again until the next race (sub_41de40).
 *
 * Here a kind-1 object whose model (its XOBJ, type 4, crossno = index, in one
 * of its block's four lists at +0x564) is a lamp post -- tall and thin -- or a
 * tree -- 2.5 to 25 m tall, within 10 m of its foot, one to two quads (a
 * billboard or two crossed: Redrock's and Hometown's trees, Atlantica's palms,
 * Country Woods' pines; nearly every kind-1 object is one) -- is taken
 * as a sign for that one question (its kind 2 while sub_41d350 runs), so it is
 * knocked down at the same speeds.  Then:
 *   - it always flies the slow animation, and slower than a sign (kSpin), the
 *     heavier the slower: it is played at that share of the clock;
 *   - the car loses what an inelastic hit with that mass takes away from its
 *     speed on the ground: v * m / (m + mass), m kCarMass;
 *   - after NFS_KNOCK_RESPAWN seconds (10) it stands again, once no car is within kClearance.
 * NFS_KNOCK_TREES (on; 0 or off: they stand as the game has them),
 * NFS_TREE_MASS and NFS_LAMP_MASS (1: times 150 and 70 kg; 0 for no slowing),
 * NFS_KNOCK_TRACE logs every touch. */
namespace nfs3hp
{

namespace
{

// The game's.
constexpr x86::reg32 kRaceClock = 0x7d3684;          // 64 ticks a second
constexpr x86::reg32 kObjectStates = 0x5dd230;       // 3 bytes a global number
constexpr x86::reg32 kObjectStateCount = 0x200;
constexpr x86::reg32 kAnimSlots = 0x56f234;          // 24 bytes a slot
constexpr x86::reg32 kAnimSlotSize = 0x18;
constexpr x86::reg32 kAnimSlotStart = 4;             // its start on the race clock
constexpr x86::reg32 kAnimations = 0x570a34;         // the knock-down animations, a pointer each
constexpr x86::reg32 kSlowKnock = 2;                 // the animation below 25 m/s
constexpr x86::reg32 kCarBlock = 0xc;                // car: its track block
constexpr x86::reg32 kCarPosition = 0x98, kCarVelocity = 0xa4, kCarForward = 0xd8;
constexpr x86::reg32 kPlayerCar = 0x5efa48;
constexpr x86::reg32 kCarList = 0x5efc48, kCarCount = 0x5efda0;   // the race's cars, a pointer each
constexpr x86::reg32 kMaxCars = 16;
constexpr x86::reg32 kBlocks = 0x571370, kBlockSize = 0x5c0, kBlockCount = 0x5dd83c;   // the track's blocks; the last one's number
constexpr x86::reg32 kBlockRefs = 0x5a4;             // block: its REFXOBJ records
constexpr x86::reg32 kBlockRefCount = 0x5a0;
constexpr x86::reg32 kBlockObjectLists = 0x564;      // block: four object lists, 8 bytes each
constexpr x86::reg32 kRefSize = 20, kRefGlobal = 0xe, kRefKind = 0x13;
constexpr x86::reg8 kKindSolid = 1, kKindSign = 2;
constexpr x86::reg8 kStanding = 0xff, kLying = 0xfe;
constexpr x86::reg32 kXobjSize = 0x34, kXobjType = 0, kXobjCross = 4, kXobjPoint = 0xc, kXobjVerts = 0x20, kXobjVertList = 0x24;
constexpr x86::reg32 kXobjStatic = 4;

// The port's.
struct Heavy
{
    float mass;    // kg, times its NFS_ setting
    float spin;    // the share of the clock its fall is played at
    const char* setting;
};
constexpr Heavy kTree{ 150.0f, 0.5f, "NFS_TREE_MASS" };
constexpr Heavy kLamp{ 70.0f, 0.65f, "NFS_LAMP_MASS" };
constexpr float kCarMass = 1400.0f;                  // kg, every car
constexpr float kRespawnSeconds = 10.0f;             // NFS_KNOCK_RESPAWN
constexpr float kClearance = 5.0f;                   // m: no car this near when it stands again

struct Shape
{
    float lampHeight = 4.0f, lampRadius = 0.6f;               // a lamp post: taller, thinner
    float treeLow = 2.5f, treeHigh = 25.0f, treeRadius = 10.0f; // a tree: a billboard or crossed ones
    x86::reg32 treeVerts = 8;
};
constexpr Shape kShape{};

enum class Kind { Other, Tree, Lamp };

struct Fallen
{
    x86::reg32 ref;          // its REFXOBJ record
    x86::reg32 global;
    x86::sreg32 since;
    Kind kind;
    float owed = 0.0f;       // clock ticks its animation is still to be held back
    x86::sreg32 seen = 0;
};

struct Model
{
    Kind kind = Kind::Other;
    float at[3] = {};        // where it stands (its XOBJ's point)
};
std::unordered_map<x86::reg32, Model> s_kinds;   // by REFXOBJ record
std::vector<Fallen> s_fallen;
x86::reg32 s_ref = 0, s_car = 0;                // the question being asked
Kind s_asked = Kind::Other;

float settingOf(const char* name, float otherwise)
{
    const char* v = SDL_getenv(name);
    if (!v || !*v)
        return otherwise;
    if (!SDL_strcasecmp(v, "off"))
        return 0.0f;
    return SDL_max(0.0f, float(SDL_atof(v)));
}

bool on()
{
    static const bool enabled = settingOf("NFS_KNOCK_TREES", 1.0f) > 0.0f;
    return enabled;
}

bool tracing()
{
    static const bool trace = SDL_getenv("NFS_KNOCK_TRACE") != nullptr;
    return trace;
}

const Heavy& heavyOf(Kind kind) { return kind == Kind::Lamp ? kLamp : kTree; }

float massOf(Kind kind)
{
    static const float tree = settingOf(kTree.setting, 1.0f), lamp = settingOf(kLamp.setting, 1.0f);
    return heavyOf(kind).mass * (kind == Kind::Lamp ? lamp : tree);
}

// What the model of the object at `index` of `block` is.
Kind kindOf(win32::WinApplication* app, x86::reg32 block, x86::reg32 index, x86::reg32 ref)
{
    if (const auto known = s_kinds.find(ref); known != s_kinds.end())
        return known->second.kind;
    Model model;
    auto word = [app](x86::reg32 a) { return app->getMemory<x86::reg32>(a); };
    Kind kind = Kind::Other;
    for (x86::reg32 list = 0; list < 4; ++list)
    {
        const x86::reg32 count = word(block + kBlockObjectLists + 8 * list);
        const x86::reg32 objects = word(block + kBlockObjectLists + 8 * list + 4);
        for (x86::reg32 i = 0; objects && i < count && i < 4096; ++i)
        {
            const x86::reg32 object = objects + i * kXobjSize;
            if (word(object + kXobjType) != kXobjStatic || word(object + kXobjCross) != index)
                continue;
            const x86::reg32 verts = word(object + kXobjVerts);
            const x86::reg32 at = word(object + kXobjVertList);
            float low = 1e9f, high = -1e9f, radius = 0.0f;
            for (x86::reg32 v = 0; at && v < verts; ++v)
            {
                const float x = app->getMemory<float>(at + v * 12), y = app->getMemory<float>(at + v * 12 + 4),
                            z = app->getMemory<float>(at + v * 12 + 8);
                low = SDL_min(low, y);
                high = SDL_max(high, y);
                radius = SDL_max(radius, std::sqrt(x * x + z * z));
            }
            const float height = high - low;
            for (x86::reg32 k = 0; k < 3; ++k)
                model.at[k] = app->getMemory<float>(object + kXobjPoint + 4 * k);
            if (height >= kShape.lampHeight && radius <= kShape.lampRadius)
                kind = Kind::Lamp;
            else if (height >= kShape.treeLow && height <= kShape.treeHigh && radius <= kShape.treeRadius
                     && verts <= kShape.treeVerts)
                kind = Kind::Tree;
            if (tracing())
                SDL_Log("[KNOCK] model of ref %x: %u verts, %.2f m tall, %.2f m round: %s", unsigned(ref), unsigned(verts),
                        height, radius, kind == Kind::Lamp ? "lamp" : kind == Kind::Tree ? "tree" : "other");
            list = 4;
            break;
        }
    }
    model.kind = kind;
    s_kinds[ref] = model;
    return kind;
}

}  // namespace

/* Whether a lamp post that is down -- falling or lying, till it stands again --
 * stands under a light at (x, y, z), world metres: the lamp's light is the
 * track's own glow (sub_491190, native_render.cpp's `glow`), not part of the
 * post's model, and it hung on in the air over the fallen post at night
 * (2026-11-22).  Under it is within kLampReach of its foot across the ground,
 * an arm over the road and all, and kLampLow to kLampHigh above it. */
constexpr float kLampReach = 4.5f, kLampLow = 2.0f, kLampHigh = 15.0f;   // m

bool lampDownNear(float x, float y, float z)
{
    for (const Fallen& f : s_fallen)
    {
        if (f.kind != Kind::Lamp)
            continue;
        const auto known = s_kinds.find(f.ref);
        if (known == s_kinds.end())
            continue;
        const float* foot = known->second.at;
        const float dx = x - foot[0], dz = z - foot[2], up = y - foot[1];
        if (dx * dx + dz * dz <= kLampReach * kLampReach && up >= kLampLow && up <= kLampHigh)
            return true;
    }
    return false;
}

/* At 0x430f02, before sub_41d350(car eax, index edx). */
void objectHitBefore(win32::WinApplication* app, x86::reg32 car, x86::reg32 index)
{
    s_ref = 0;
    s_asked = Kind::Other;
    if (!on())
        return;
    const x86::reg32 block = app->getMemory<x86::reg32>(car + kCarBlock);
    const x86::reg32 refs = block ? x86::reg32(app->getMemory<x86::reg32>(block + kBlockRefs)) : 0u;
    if (!refs)
        return;
    const x86::reg32 ref = refs + index * kRefSize;
    const x86::reg8 kind = app->getMemory<x86::reg8>(ref + kRefKind);
    if (tracing())
        SDL_Log("[KNOCK] car %x touches ref %x (index %u) kind %u", unsigned(car), unsigned(ref), unsigned(index), unsigned(kind));
    if (kind != kKindSolid)
        return;
    const Kind what = kindOf(app, block, index, ref);
    if (what == Kind::Other)
        return;
    const x86::reg32 global = app->getMemory<uint16_t>(ref + kRefGlobal);
    if (global >= kObjectStateCount)
        return;
    // Standing, it is knocked down as a sign is; down, it is driven over as a fallen sign is (answer 0).
    app->getMemory<x86::reg8>(ref + kRefKind) = kKindSign;
    s_ref = ref;
    s_car = car;
    s_asked = what;
}

/* After it, its answer in eax: -1 knocked down. */
void objectHitAfter(win32::WinApplication* app, x86::reg32 answer)
{
    if (!s_ref)
        return;
    const x86::reg32 ref = s_ref;
    s_ref = 0;
    app->getMemory<x86::reg8>(ref + kRefKind) = kKindSolid;
    if (answer != 0xffffffffu)
        return;
    const x86::reg32 global = app->getMemory<uint16_t>(ref + kRefGlobal);
    const x86::reg8 state = app->getMemory<x86::reg8>(kObjectStates + global * 3);
    if (state < 0x80)
    {
        // The slow fall, always.
        const x86::reg32 slot = kAnimSlots + x86::reg32(state) * kAnimSlotSize;
        if (const x86::reg32 slow = app->getMemory<x86::reg32>(kAnimations + kSlowKnock * 4))
            app->getMemory<x86::reg32>(slot) = slow;
    }
    // The car slowed by what the hit took.
    const float mass = massOf(s_asked);
    const float keep = kCarMass / (kCarMass + mass);
    float speed = 0.0f;
    for (int k = 0; k < 3; k += 2)
    {
        const x86::reg32 at = s_car + kCarVelocity + 4 * x86::reg32(k);
        const float v = app->getMemory<float>(at);
        speed += v * v;
        app->getMemory<float>(at) = v * keep;
    }
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    s_fallen.push_back({ ref, global, clock, s_asked, 0.0f, clock });
    if (tracing())
        SDL_Log("[KNOCK] t %d %s %u down at %.1f m/s, the car keeps %.0f%%", int(clock),
                s_asked == Kind::Lamp ? "lamp" : "tree", unsigned(global), std::sqrt(speed), keep * 100.0f);
}

/* Each frame: the falls slowed, and the fallen stood up again. */
void objectsFrame(win32::WinApplication* app)
{
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    static x86::sreg32 last = 0;
    if (clock < last)
        s_kinds.clear();   // a new race, maybe a new track
    last = clock;
    if (tracing())
    {
        // Once a race: every block's objects of kind 1 and 2, and their models.
        static bool listed = false;
        if (clock < 64)
            listed = false;
        if (!listed && clock > 64)
        {
            listed = true;
            const x86::reg32 blocks = app->getMemory<x86::reg32>(kBlockCount);
            for (x86::reg32 b = 0; b <= blocks && b < 1000; ++b)
            {
                const x86::reg32 block = kBlocks + b * kBlockSize;
                const x86::reg32 refs = app->getMemory<x86::reg32>(block + kBlockRefs);
                const x86::reg32 count = app->getMemory<x86::reg32>(block + kBlockRefCount);
                for (x86::reg32 n = 0; refs && n < count && n < 256; ++n)
                {
                    const x86::reg32 r = refs + n * kRefSize;
                    const x86::reg8 k = app->getMemory<x86::reg8>(r + kRefKind);
                    if (k == kKindSolid || k == kKindSign)
                    {
                        s_kinds.erase(r);
                        kindOf(app, block, n, r);
                    }
                }
            }
        }
    }
    if (SDL_getenv("NFS_KNOCK_TEST"))
    {
        /* Test only: 4 s on the clock the player's car is put 12 m short of the
         * nearest tree or lamp post of its block, at 30 m/s straight at it. */
        static bool put = false;
        if (clock < 64)
            put = false;
        const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCar);
        const x86::reg32 block = car ? x86::reg32(app->getMemory<x86::reg32>(car + kCarBlock)) : 0u;
        const x86::reg32 refs = block ? x86::reg32(app->getMemory<x86::reg32>(block + kBlockRefs)) : 0u;
        const bool lamp = SDL_strcmp(SDL_getenv("NFS_KNOCK_TEST"), "lamp") == 0;
        const bool sign = SDL_strcmp(SDL_getenv("NFS_KNOCK_TEST"), "sign") == 0;
        for (x86::reg32 n = 0; !put && refs && clock > 4 * 64 && n < 64; ++n)
        {
            const x86::reg32 r = refs + n * kRefSize;
            if (app->getMemory<x86::reg8>(r + kRefKind) != (sign ? kKindSign : kKindSolid))
                continue;
            if (kindOf(app, block, n, r) != (sign ? Kind::Other : lamp ? Kind::Lamp : Kind::Tree))
                continue;
            put = true;
            const Model& m = s_kinds[r];
            float d[3] = { app->getMemory<float>(car + kCarForward), 0.0f, app->getMemory<float>(car + kCarForward + 8) };
            const float len = SDL_max(0.1f, std::sqrt(d[0] * d[0] + d[2] * d[2]));
            d[0] /= len;
            d[2] /= len;
            app->getMemory<float>(car + kCarPosition) = m.at[0] - d[0] * 12.0f;
            app->getMemory<float>(car + kCarPosition + 4) = m.at[1] + 0.5f;
            app->getMemory<float>(car + kCarPosition + 8) = m.at[2] - d[2] * 12.0f;
            app->getMemory<float>(car + kCarVelocity) = d[0] * 30.0f;
            app->getMemory<float>(car + kCarVelocity + 4) = 0.0f;
            app->getMemory<float>(car + kCarVelocity + 8) = d[2] * 30.0f;
            SDL_Log("[KNOCK] test: car put before ref %u at %.1f %.1f %.1f", unsigned(n), m.at[0], m.at[1], m.at[2]);
        }
    }
    for (std::size_t i = 0; i < s_fallen.size();)
    {
        Fallen& f = s_fallen[i];
        if (clock < f.since)
        {
            s_fallen.erase(s_fallen.begin() + std::ptrdiff_t(i));   // a new race
            continue;
        }
        const x86::reg32 state = kObjectStates + f.global * 3;
        const x86::reg8 now = app->getMemory<x86::reg8>(state);
        const x86::sreg32 ticks = SDL_max(0, clock - f.seen);
        f.seen = clock;
        if (now < 0x80)
        {
            // Held back to its share of the clock.
            f.owed += float(ticks) * (1.0f - heavyOf(f.kind).spin);
            const x86::sreg32 whole = x86::sreg32(f.owed);
            if (whole > 0)
            {
                app->getMemory<x86::reg32>(kAnimSlots + x86::reg32(now) * kAnimSlotSize + kAnimSlotStart) += x86::reg32(whole);
                f.owed -= float(whole);
            }
        }
        static const x86::sreg32 respawn = x86::sreg32(settingOf("NFS_KNOCK_RESPAWN", kRespawnSeconds) * 64.0f);
        if (clock - f.since < respawn)
        {
            ++i;
            continue;
        }
        // Stood up again, once no car is near.
        bool clear = true;
        const x86::reg32 count = app->getMemory<x86::reg32>(kCarCount);
        for (x86::reg32 c = 0; c < count && c < kMaxCars && clear; ++c)
        {
            const x86::reg32 car = app->getMemory<x86::reg32>(kCarList + 4 * c);
            if (!car)
                continue;
            float d2 = 0.0f;
            for (int k = 0; k < 3; k += 2)
            {
                const float d = app->getMemory<float>(car + kCarPosition + 4 * x86::reg32(k)) - s_kinds[f.ref].at[k];
                d2 += d * d;
            }
            clear = d2 > kClearance * kClearance;
            if (!clear && tracing() && ticks > 0)
                SDL_Log("[KNOCK] t %d %u waits: car %x %.1f m away", int(clock), unsigned(f.global), unsigned(car), std::sqrt(d2));
        }
        if (!clear)
        {
            ++i;
            continue;
        }
        if (now < 0x80)
        {
            const x86::reg32 slot = kAnimSlots + x86::reg32(now) * kAnimSlotSize;
            app->getMemory<x86::reg32>(slot) = 0;
            app->getMemory<x86::reg32>(slot + kAnimSlotStart) = 0xffffffffu;
            app->getMemory<x86::reg32>(slot + 8) = 0;
        }
        app->getMemory<x86::reg8>(state) = kStanding;
        app->getMemory<x86::reg8>(state + 1) = 0;
        app->getMemory<x86::reg8>(state + 2) = 0;
        if (tracing())
            SDL_Log("[KNOCK] t %d %u stands again", int(clock), unsigned(f.global));
        s_fallen.erase(s_fallen.begin() + std::ptrdiff_t(i));
    }
}

}  // namespace nfs3hp
