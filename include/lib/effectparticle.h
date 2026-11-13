#ifndef EFFECTPARTICLE_H
#define EFFECTPARTICLE_H

/* Port: one of the port's own particles (src/nfs3hp/native_particles.cpp) as
 * the frame shows it, for the native THRASH renderer to draw over the world
 * before its whole-picture pass (ThrashRenderer::drawParticles). */
struct EffectParticle
{
    enum Type { Grain = 0, Puff = 1, Spark = 2 };
    float x, y;         // the centre, in the picture's pixels
    float tx, ty;       // a spark: where it was a moment ago; else where it was thrown from
    float depth;        // its distance in the view, as the depth buffer has it (z)
    float size;         // its half-width, in pixels
    float r, g, b, a;   // its colour where the ground's cannot be had; a is how much of it shows
    float seed;         // 0 to 1, its own shape
    float type;         // Type
};

#endif
