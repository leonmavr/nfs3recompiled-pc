#ifndef EFFECTPARTICLE_H
#define EFFECTPARTICLE_H

/* Port: one of the port's own particles (src/nfs3hp/native_particles.cpp) as
 * the frame shows it, for the native THRASH renderer to draw over the world
 * before its whole-picture pass (ThrashRenderer::drawParticles). */
struct EffectParticle
{
    // Blade: off grass; Spike: a strip's; Streak: a piece of the air trail beside a slipstreaming car;
    // Flame: out of an exhaust on a gear change; Glow: the light a flame throws on the road;
    // Smoke: off a wheel spinning or locked; Droplet: a raindrop; Flake: a snowflake;
    // Frost: a sharp chip of ice off a tyre on snow
    enum Type
    {
        Grain = 0, Puff = 1, Spark = 2, Blade = 3, Spike = 4, Streak = 5, Flame = 6, Glow = 7, Smoke = 8,
        Droplet = 9, Flake = 10, Frost = 12   // 11 was BrakeGlow, removed
    };
    float x, y;         // the centre, in the picture's pixels
    float tx, ty;       // a spark's, a drop's, a flake's: where it was a moment ago; else where it was thrown from
    float depth;        // its distance in the view, as the depth buffer has it (z)
    float size;         // its half-width, in pixels
    float r, g, b, a;   // its colour where the ground's cannot be had; a is how much of it shows
    float seed;         // 0 to 1, its own shape
    float type;         // Type
    float turn;         // radians: how a grass blade lies, turning as it tumbles
};

#endif
