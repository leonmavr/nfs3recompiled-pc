#include <nfs3hp.h>
#include "native_guest.h"
#include <SDL3/SDL.h>
#include <cmath>

/* Port: the cars' bodies move a little on their springs, as they are drawn
 * (NFS_BODY_MOTION).
 *
 * Discreet, and indicative only: a car leans a little out of a corner, tilts a
 * little back going uphill and forward going downhill, and settles a few
 * centimetres onto its springs when it lands.  Nothing the game works out --
 * its physics, its collisions, its camera, how a car drives -- sees any of it:
 * the car's record is never written.
 *
 * How.  A car's model is drawn part by part by sub_49de20 (from sub_49d5c0's
 * loop over the model's parts, and from the lights' stand-ins in
 * native_render.cpp), handed the car's matrix ([esp+4], a copy of its axes
 * +0xc0 right, +0xcc up, +0xd8 forward) and its position ([esp+8]).
 * bodyMotionPart, at that function's top (tools/apply_body_motion.py), finds
 * the race car whose axes the matrix is, and draws the part through the
 * original with a leaned matrix and a moved position put in for the call
 * alone, then puts back what was there.  (Writing the lean into the cars'
 * records for the frame let the game's physics, which runs between, read it:
 * the cars ended up on two wheels.)
 *
 * The lean, worked out once a frame as its drawing begins (bodyMotionFrame,
 * the main view's sky), from springs with dampers in short steps:
 *   - roll, about the car's length: kRollPerG radians a g of sideways
 *     acceleration (the velocity's change along the car's right, eased over
 *     kAccelSeconds), the body to the outside of the corner;
 *   - pitch, about its width: kPitchPerSlope of the road's slope (the car's
 *     forward axis against level), the body back going up and forward going
 *     down -- not the throttle or the brakes;
 *   - sink, down its up axis: on landing ([car+0x164] going to 0) kLandPerSpeed
 *     metres for each m/s it came down at, and back on the spring.
 * Each held to its most (kMostRoll, kMostPitch, kMostSink), all of it times
 * NFS_BODY_MOTION (1; up to 3; 0 or off for none); in the air the lean
 * settles.  The body turns about the ground under its centre (the lowest of
 * its wheels' contacts), so it leans over its wheels. */
namespace nfs3hp
{

void wheelContact(win32::WinApplication* app, x86::reg32 car, int wheel, float at[3]);   // native_particles.cpp

namespace
{

namespace body
{
// The game's.
constexpr x86::reg32 kCars = 0x5e1120, kCarSize = 0x9ac, kSlots = 16;
constexpr x86::reg32 kRecords = 0x6fd52c, kRecordSize = 0x6c, kRecord = 0x220;
constexpr x86::reg32 kPosition = 0x98, kVelocity = 0xa4, kAxes = 0xc0, kAir = 0x164;
constexpr x86::reg32 kClock = 0x7d3684;   // 64 a second
constexpr x86::reg32 kDrawPart = 0x49de20;  // sub_49de20, ret 0x14
constexpr x86::reg32 kDrawPartArguments = 0x14;
constexpr float kG = 9.81f;

// The port's: discreet.
constexpr float kRollPerG = 0.018f;       // rad of roll a g sideways (1 degree)
constexpr float kMostRoll = 0.026f;       // rad (1.5 degrees)
constexpr float kPitchPerSlope = 0.06f;   // rad of pitch for each rad of the road's slope
constexpr float kMostPitch = 0.017f;      // rad (1 degree)
constexpr float kMostAccel = 12.0f;       // m/s^2 taken at most: past it, a hit, not a corner
constexpr float kAccelSeconds = 0.25f;    // the sideways acceleration eased over
constexpr float kAngleHz = 1.4f, kAngleDamping = 0.7f;   // the body on its springs, turning: barely overshooting
constexpr float kSinkHz = 2.4f, kSinkDamping = 0.5f;     // and going up and down
constexpr float kLandPerSpeed = 0.006f;   // m sunk for each m/s of the fall, on landing
constexpr float kMostSink = 0.04f;        // m
constexpr float kStep = 1.0f / 120.0f;    // s: the springs worked out in steps no longer
constexpr float kLongestFrame = 0.1f;     // s: a longer gap is a pause, not motion
}  // namespace body

struct Spring
{
    float at = 0.0f, rate = 0.0f;

    void toward(float target, float hz, float damping, float dt)
    {
        const float w = 6.2832f * hz;
        rate += (w * w * (target - at) - 2.0f * damping * w * rate) * dt;
        at += rate * dt;
    }
};

struct Body
{
    bool known = false;
    bool air = false;
    float velocity[3] = {};
    float fall = 0.0f;           // m/s down, the last frame in the air
    float sideways = 0.0f;       // its sideways acceleration, eased: m/s^2
    Spring roll, pitch, sink;
    float height = 0.0f;         // its centre over the ground under it, this frame
};
Body s_bodies[body::kSlots];
x86::sreg32 s_clock = -1;

float setting()
{
    static const float value = [] {
        const char* text = SDL_getenv("NFS_BODY_MOTION");
        if (!text || !*text || SDL_strcasecmp(text, "on") == 0)
            return 1.0f;
        if (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(text));
        return parsed > 0.0f ? SDL_min(parsed, 3.0f) : 1.0f;
    }();
    return value;
}

bool inRace(win32::WinApplication* app, x86::reg32 car)
{
    const x86::reg32 record = app->getMemory<x86::reg32>(car + body::kRecord);
    return record >= body::kRecords && record < body::kRecords + body::kSlots * body::kRecordSize
           && (record - body::kRecords) % body::kRecordSize == 0;
}

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

/* As a frame's drawing begins: each race car's lean brought on by its springs. */
void bodyMotionFrame(win32::WinApplication* app)
{
    const float amount = setting();
    if (amount <= 0.0f)
        return;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(body::kClock));
    if (s_clock < 0 || clock < s_clock)
        for (Body& b : s_bodies)
            b = Body{};   // a new race
    float dt = s_clock < 0 ? 0.0f : float(clock - s_clock) / 64.0f;
    s_clock = clock;
    if (dt > body::kLongestFrame)
    {
        dt = 0.0f;   // a pause or a jump: nothing to make of the change
        for (Body& b : s_bodies)
            b.known = false;
    }
    for (x86::reg32 k = 0; k < body::kSlots; ++k)
    {
        const x86::reg32 car = body::kCars + k * body::kCarSize;
        Body& b = s_bodies[k];
        if (!inRace(app, car))
        {
            b = Body{};
            continue;
        }
        const float* at = &app->getMemory<float>(car + body::kPosition);
        const float* axes = &app->getMemory<float>(car + body::kAxes);   // right, up, forward
        const float* v = &app->getMemory<float>(car + body::kVelocity);
        const bool air = app->getMemory<uint16_t>(car + body::kAir) != 0;
        float ground = 1e9f;
        for (int w = 0; w < 4; ++w)
        {
            float contact[3];
            wheelContact(app, car, w, contact);
            ground = SDL_min(ground, contact[1]);
        }
        b.height = SDL_clamp(at[1] - ground, 0.0f, 1.5f);
        if (!(dt > 0.0f))
            continue;
        if (b.known && !air)
        {
            float a[3];
            for (int i = 0; i < 3; ++i)
                a[i] = (v[i] - b.velocity[i]) / dt;
            const float sideways = SDL_clamp(dot(a, axes), -body::kMostAccel, body::kMostAccel);
            b.sideways += (sideways - b.sideways) * (1.0f - std::exp(-dt / body::kAccelSeconds));
        }
        else if (air)
            b.sideways *= std::exp(-dt / body::kAccelSeconds);
        if (b.known && b.air && !air)   // landing: onto its springs by how fast it came down
            b.sink.rate -= b.fall * body::kLandPerSpeed * 6.2832f * body::kSinkHz;
        if (air)
            b.fall = SDL_max(0.0f, -v[1]);
        b.air = air;
        for (int i = 0; i < 3; ++i)
            b.velocity[i] = v[i];
        b.known = true;
        // The road's slope: the forward axis against level (+ uphill).
        const float slope = std::asin(SDL_clamp(axes[7], -1.0f, 1.0f));
        const float rollTo = air ? 0.0f
                                 : SDL_clamp(b.sideways / body::kG * body::kRollPerG, -body::kMostRoll, body::kMostRoll)
                                       * amount;
        const float pitchTo = air ? 0.0f
                                  : SDL_clamp(slope * body::kPitchPerSlope, -body::kMostPitch, body::kMostPitch) * amount;
        for (float left = dt; left > 0.0f; left -= body::kStep)
        {
            const float step = SDL_min(left, body::kStep);
            b.roll.toward(rollTo, body::kAngleHz, body::kAngleDamping, step);
            b.pitch.toward(pitchTo, body::kAngleHz, body::kAngleDamping, step);
            b.sink.toward(0.0f, body::kSinkHz, body::kSinkDamping, step);
        }
        b.sink.at = SDL_clamp(b.sink.at, -body::kMostSink * amount, body::kMostSink * amount);
    }
}

/* At sub_49de20's top (tools/apply_body_motion.py): a part of a race car's
 * model drawn leaned.  True when it was drawn here (the original called for
 * it, its ret 0x14 done), false to let the original draw it as it is. */
bool bodyMotionPart(win32::WinApplication* app, x86::CPU& cpu)
{
    static bool s_inner = false;   // the original, called from here
    if (s_inner || setting() <= 0.0f)
        return false;
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 matrix = app->getMemory<x86::reg32>(entry + 4);
    const x86::reg32 position = app->getMemory<x86::reg32>(entry + 8);
    if (!matrix || !position)
        return false;
    const float* given = &app->getMemory<float>(matrix);
    // Whose axes these are: a race car's, as they are now.
    int which = -1;
    for (x86::reg32 k = 0; k < body::kSlots && which < 0; ++k)
    {
        const x86::reg32 car = body::kCars + k * body::kCarSize;
        if (!s_bodies[k].known)
            continue;
        const float* axes = &app->getMemory<float>(car + body::kAxes);
        bool same = true;
        for (int i = 0; i < 9 && same; ++i)
            same = given[i] == axes[i];
        if (same)
            which = int(k);
    }
    if (which < 0)
        return false;
    const Body& b = s_bodies[which];
    if (b.roll.at == 0.0f && b.pitch.at == 0.0f && b.sink.at == 0.0f)
        return false;
    const float* right = given;
    const float* up = given + 3;
    const float* forward = given + 6;
    /* Roll about its length: up over to the outside of the corner (a positive
     * sideways acceleration is to its right, the body to its left); then pitch
     * about its width: the nose up as the road climbs. */
    const float cr = std::cos(b.roll.at), sr = std::sin(b.roll.at);
    const float cp = std::cos(b.pitch.at), sp = std::sin(b.pitch.at);
    float leaned[9], moved[3];
    for (int i = 0; i < 3; ++i)
    {
        const float r1 = right[i] * cr + up[i] * sr;
        const float u1 = up[i] * cr - right[i] * sr;
        const float f2 = forward[i] * cp + u1 * sp;
        const float u2 = u1 * cp - forward[i] * sp;
        leaned[i] = r1;
        leaned[3 + i] = u2;
        leaned[6 + i] = f2;
        // About the ground under its centre, then down by its sink: the centre
        // moves by (h + sink) u' - h u.
        moved[i] = (b.height + b.sink.at) * u2 - b.height * up[i];
    }
    // For this call alone: the leaned matrix and the moved position in place.
    float savedMatrix[9], savedPosition[3];
    float* m = &app->getMemory<float>(matrix);
    float* p = &app->getMemory<float>(position);
    for (int i = 0; i < 9; ++i)
    {
        savedMatrix[i] = m[i];
        m[i] = leaned[i];
    }
    for (int i = 0; i < 3; ++i)
    {
        savedPosition[i] = p[i];
        p[i] += moved[i];
    }
    x86::reg32 arguments[5];
    for (int k = 0; k < 5; ++k)
        arguments[k] = app->getMemory<x86::reg32>(entry + 4 + 4 * x86::reg32(k));
    s_inner = true;
    const bool ran = guest::call(app, cpu, body::kDrawPart,
                                 { arguments[0], arguments[1], arguments[2], arguments[3], arguments[4] });
    s_inner = false;
    for (int i = 0; i < 9; ++i)
        m[i] = savedMatrix[i];
    for (int i = 0; i < 3; ++i)
        p[i] = savedPosition[i];
    if (!ran)
        return true;   // the game stopped
    cpu.esp = entry + 4 + body::kDrawPartArguments;   // its ret 0x14
    return true;
}

}  // namespace nfs3hp
