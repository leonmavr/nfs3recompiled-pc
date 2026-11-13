#ifndef LIB_THRASHRENDERER_H_
#define LIB_THRASHRENDERER_H_

#include <lib/gliderenderer.h>
#include <lib/effectparticle.h>
#include <memory>
#include <array>
#include <unordered_map>
#include <vector>

namespace win32
{

/* 32 bytes: position and Glide's depth, the colour, s/w, t/w and 1/w, and the
 * colour and alpha combine factors (0 or 1) and whether it is fogged. */
struct ThrashVertex
{
    float           x, y, z;
    x86::reg32      color;
    float           u, v, oow;
    x86::reg8       combine[4];
};
static_assert(sizeof(ThrashVertex) == 32, "the attribute offsets in thrashrenderer.cpp assume this layout");

struct ThrashDraw;

/* The native THRASH driver's renderer: what the game draws through voodoo2a,
 * straight to OpenGL ES, with nothing of the Voodoo2 emulated under it.
 *
 * GlideRenderer keeps every texture in one atlas, as the Voodoo2 kept them in
 * its texture memory, and its shader filters, wraps and clamps each texel by
 * hand, four fetches a pixel, so neighbouring tiles never bleed into each
 * other.  Here each of the game's textures is a texture of OpenGL's, with the
 * mipmap chain the game supplies, and the GPU does all of that in one fetch.
 * Only a texture drawn with a chroma key is still filtered by hand: the
 * Voodoo2 keyed texels out before filtering, which no GPU filter does.
 *
 * Everything else -- the batching, the GL thread, the fog table, the 16-bit
 * depth steps, the clip window, the far texture coordinates brought back -- is
 * GlideRenderer's, so the picture is the same up to how a GPU rounds its
 * bilinear weights.  NFS_THRASH_GL=0 draws through GlideRenderer instead. */
class ThrashRenderer: public GlideBackend
{
public:
    explicit ThrashRenderer(Renderer* renderer);
    ~ThrashRenderer();

    void clear(x86::reg32 color) override;
    void swap() override;
    void render(x86::reg32 buffer) override;
    void atlasFreeSpace(x86::reg32& texels, x86::reg32& wholeTiles, x86::reg32& total) const override;
    x86::reg32 getTextureMemSize(x86::reg32 tmu, x86::reg32 largeMipmapSize, TextureFormat format) override;
    void setZWrite(bool enable) override;
    void setZTest(bool enable) override;
    void setDepthAlways(bool always) override;
    void setAlphaBlendDst(AlphaBlend method) override;
    void setDither(bool enable) override;
    void setCullMode(x86::reg32 mode) override;
    void setChromakeyMode(bool enable) override;
    /* Port: what is drawn is a car's body, which the ground's grain leaves
     * alone (glide2x::carSurface, from the car's mesh in native_vertices.cpp). */
    void setCarSurface(bool car);
    /* Port: whether the texture selected is a car's shadow, which
     * native_thrash.cpp stretches away from the sun. */
    bool drawingShadow() const { return m_textureShadow; }
    /* Port: how wet the road is (0 dry to 1), from the frame on; the GL
     * thread's reflection of the last frame is taken while it is above 0. */
    void setWetRoad(float wetness) { m_wetness = wetness; }
    /* The race's colour grade (glide2x::colourGrade): a tint, a saturation and
     * a contrast, each 1 for none; the picture before its HUD is graded. */
    /* The player's headlights in the view, and the projection (glide2x::headlights). */
    void setHeadlights(bool on, const float origin[3], const float direction[3], const float up[3], float scaleX,
                       float scaleY, float centreX, float centreY)
    {
        for (int k = 0; k < 3; ++k)
            m_head[11 + k] = up[k];
        m_head[0] = on ? 1.0f : 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            m_head[1 + k] = origin[k];
            m_head[4 + k] = direction[k];
        }
        m_head[7] = scaleX; m_head[8] = scaleY; m_head[9] = centreX; m_head[10] = centreY;
    }
    /* The sun in the view for the shadows of what stands by the road (glide2x::sunShadows). */
    void setSunShadows(float strength, const float sun[3], const float up[3], float scaleX, float scaleY,
                       float centreX, float centreY, float glare)
    {
        m_sun[0] = strength;
        m_sun[11] = glare;
        for (int k = 0; k < 3; ++k)
        {
            m_sun[1 + k] = sun[k];
            m_sun[8 + k] = up[k];
        }
        m_sun[4] = scaleX; m_sun[5] = scaleY; m_sun[6] = centreX; m_sun[7] = centreY;
    }
    /* Whether a race is being drawn (glide2x::raceView): the haze and the
     * whole-picture pass are for it alone, never the menus. */
    void setRaceView(bool race)
    {
        m_raceView = race;
        if (!race)
            m_particles.reset();   // none left over for the next race's first frame
    }
    /* How fast the first player goes, m/s (glide2x::motion): the motion blur's. */
    void setMotion(float speed) { m_speed = speed; }
    /* The port's own particles for this frame (glide2x::effectParticles). */
    void setParticles(std::vector<EffectParticle>&& particles)
    {
        m_particles = std::make_shared<const std::vector<EffectParticle>>(std::move(particles));
    }
    // Whether the whole-picture pass is on to draw them (initPost).
    bool particlesDrawn() const { return m_particlesOn; }
    void setColourGrade(float red, float green, float blue, float saturation, float contrast)
    {
        m_grade[0] = red; m_grade[1] = green; m_grade[2] = blue; m_grade[3] = saturation; m_grade[4] = contrast;
    }
    void setTexClampMode(bool wrapS, bool wrapT) override;
    void setChromakeyValue(x86::reg32 color) override;
    void setClipWindow(x86::reg32 minX, x86::reg32 minY, x86::reg32 maxX, x86::reg32 maxY) override;
    void setGamma(float correction) override;
    void setColorFactor(float colorFactor) override;
    void setAlphaFactor(float alphaFactor) override;
    void setFogMode(x86::reg32 mode) override;
    void setFogColor(x86::reg32 color) override;
    void setFogTable(const x86::reg8* table) override;
    void setAlphaTestRef(x86::reg32 value) override;
    void setTexture(x86::reg32 tmu, x86::reg32 address) override;
    void setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data,
                        x86::reg32 largeMipmap, x86::reg32 smallMimmap, TextureFormat format) override;
    void drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c) override;
    void resetTextures() override;

    /* A triangle's corner as the native THRASH driver hands it over, worked
     * out straight from the game's vertex (nfs3hp::thrashCorner): what
     * drawTriangle keeps of a GrVertex, the colour already as bytes,
     * r | g << 8 | b << 16 | a << 24. */
    struct Corner
    {
        float x, y, ooz, oow, sow, tow;
        x86::reg32 color;
    };
    void drawCorners(const Corner& a, const Corner& b, const Corner& c);

    /* The native driver's way in, without a Corner between: room for one
     * triangle in the batch, which the caller fills -- x, y, z (Glide's ooz),
     * color, u and v (s/w, t/w) and oow -- and then hands to finishTriangle,
     * which brings far texture coordinates back and marks the three as
     * drawCorners does.  Nothing else may be drawn in between. */
    ThrashVertex* reserveTriangle()
    {
        if (m_vertexCount + 3 > s_maxVertices)
            renderPending();
        return m_vertices + m_vertexCount;
    }
    void finishTriangle(ThrashVertex* corners);

    static const x86::reg32 s_maxVertices = 128000;

private:
    /* One batch: the state it is drawn with, and the triangles up to `end`. */
    struct Batch
    {
        x86::reg32 end;
        x86::reg32 texture;     // index into m_glTextures on the GL thread
        x86::reg32 levels;      // its mipmap levels, for the hand-filtered chroma key
        int format;             // 0 RGBA8, 1 565, 2 1555, 3 4444: how the chroma key compares
        bool wrapS, wrapT;
        bool zTest, zTestAlways, zBuffer, alphaBlend, cull, chromaKey;
        x86::reg32 cullMode;
        x86::reg32 clipMinX, clipMinY, clipMaxX, clipMaxY;
        float gamma;
        int fogTable;
        bool detail;            // the ground's grain over it (initGl)
        bool surface;           // the rest of the world: its own, fainter grain (initGl)
        bool sharpen;           // drawn in the world: magnified bicubic (initGl)
        bool carShine;          // a car's reflection pass, shaped by its facing (initGl)
        bool headlit;           // lit by the player's headlights in the shader (initGl)
        bool car;               // a car's or an effect's: 0 into the picture's alpha (postProcess's shadows)
        bool decal;             // a car's shadow on the road: no depth, the road's alpha kept
        bool light;             // a lamp's glow: drawn into the light layer too (initPost)
    };
    friend struct ThrashDraw;

    /* A texture of the game's, as the game thread knows it: the index of its
     * GL texture, its size and levels, and the render pass it was last
     * selected in. */
    struct Texture
    {
        x86::reg32 index;
        x86::reg32 size;
        x86::reg32 levels;
        x86::reg32 useStamp;
        int format;
        bool shadow;            // a car's shadow (the port's softening, thrashrenderer.cpp)
        bool light;             // a lamp's glow, glw0 or glw3 (the light layer, thrashrenderer.cpp)
    };

    void initGl();
    void compileShaders(int depthBits, const char* renderer);
    void clearFrame(x86::reg32 color, x86::reg32 clipMinX, x86::reg32 clipMinY,
                    x86::reg32 clipMaxX, x86::reg32 clipMaxY);
    void flush();
    void renderPending();
    void drawPending(const ThrashDraw& draw);
    void upload(x86::reg32 index, x86::reg32 size, x86::reg32 levels, TextureFormat format,
                std::vector<x86::reg8>& texels);

private:
    Renderer*               m_renderer;
    // The GL thread's.
    unsigned int            m_depthBuffer;
    unsigned int            m_framebuffer;
    unsigned int            m_vertexBuffer;
    unsigned int            m_vertexArray;
    unsigned int            m_program;
    unsigned int            m_samplers[4];  // [wrapS + 2 * wrapT]
    std::vector<unsigned int> m_glTextures;
    struct
    {
        int position, color, texCoord, combine;
        int transform, fogW, fogTable, fogColor, alphaRef, depthCut, chromaKey, chromaKeyColor, gamma, wrap, levels, texture,
            format, detail, detailTexture, detailScale, wet, reflection, screen, surface, sharpen, carShine, haze, sky, headlight, headOrigin, headDir, headUp, projection, projCentre;
    } m_locations;
    unsigned int            m_detailTexture = 0;
    bool                    m_carSurface = false;   // the game thread's
    bool                    m_textureShadow = false;
    bool                    m_textureLight = false;
    float                   m_wetness = 0.0f;
    // The GL thread's: the last frame, for the wet road's reflection.
    unsigned int            m_reflection = 0;
    int                     m_reflectionW = 0, m_reflectionH = 0;
    bool                    m_reflectionValid = false;
    bool                    m_reflectionTaken = false;  // this frame's, as its HUD began
    bool                    m_sceneDrawn = false;       // this frame has drawn some of the world
    void takeReflection(int width, int height);
    // The haze's sky and the whole-picture effects (thrashrenderer.cpp, initPost).
    void initPost();
    void takeSky(int width, int height);
    void allocateLightLayer(int width, int height);
    void drawParticles(const ThrashDraw& draw, int width, int height);
    void postProcess(const ThrashDraw& draw);
    float                   m_grade[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };  // the game thread's
    float                   m_head[14] = {};        // the game thread's: on, origin, direction, projection, up
    float                   m_headStrength = 0.0f;  // NFS_HEADLIGHT_OBJECTS
    float                   m_sun[12] = {};         // the game thread's: strength, sun, projection, up, glare
    unsigned int            m_postDepth = 0, m_depthFramebuffer = 0;  // the GL thread's: the depth, copied
    float                   m_haze = 0.0f;          // NFS_HAZE
    float                   m_bloom = 0.0f;         // NFS_BLOOM
    bool                    m_gradeOn = false;      // NFS_GRADE
    bool                    m_antialias = false;    // NFS_AA
    unsigned int            m_sky = 0;              // the GL thread's, from here on
    int                     m_skyW = 0, m_skyH = 0;
    bool                    m_skyTaken = false;     // this frame's
    bool                    m_skyValid = false;
    unsigned int            m_postCopy = 0, m_bright = 0, m_postFramebuffer = 0, m_postArray = 0;
    unsigned int            m_brightProgram = 0, m_compositeProgram = 0;
    int                     m_postW = 0, m_postH = 0;
    bool                    m_postDone = false;     // this frame's
    bool                    m_hudBegun = false;     // this frame's
    bool                    m_raceView = false;     // the game thread's
    float                   m_speed = 0.0f;         // the game thread's
    float                   m_motionBlur = 0.0f;    // NFS_MOTION_BLUR
    float                   m_lightGlow = 0.0f;     // NFS_LIGHT_GLOW
    float                   m_lightTrails = 0.0f;   // NFS_LIGHT_TRAILS
    // The GL thread's: the frame's lamps alone (the halo's and the trails').
    unsigned int            m_lightLayer = 0, m_lightFramebuffer = 0;
    // The port's particles: the game thread's for the next frame, and how they are drawn.
    std::shared_ptr<const std::vector<EffectParticle>> m_particles;
    bool                    m_particlesOn = true;
    unsigned int            m_particleProgram = 0, m_particleArray = 0, m_particleBuffer = 0;
    int                     m_brightSource = -1, m_compositeSource = -1, m_compositeBright = -1,
                            m_compositePixel = -1, m_compositeBloom = -1, m_compositeGrade = -1,
                            m_compositeTint = -1, m_compositeAa = -1, m_compositeDepth = -1, m_compositeShadow = -1,
                            m_compositeSun = -1, m_compositeScale = -1, m_compositeCentre = -1, m_compositeUp = -1,
                            m_compositeSize = -1, m_compositeBlur = -1, m_compositeLight = -1, m_compositeHalo = -1,
                            m_compositeTrailOn = -1, m_compositeGlare = -1, m_compositeGlareAt = -1;
    float                   m_detailStrength = 0.0f;  // NFS_ROAD_DETAIL; 0 for none
    float                   m_surfaceStrength = 0.0f; // NFS_OBJECT_DETAIL; 0 for none
    bool                    m_sharpen = false;        // NFS_TEXTURE_SHARP
    float                   m_carShine = 0.0f;        // NFS_CAR_SHINE
    bool                    m_depth16;
    bool                    m_makeMipmaps = false;  // levels made for the textures (initGl)

    // The game thread's.
    ThrashVertex*           m_vertices;
    x86::reg32              m_vertexCount;
    std::vector<Batch>      m_batches;
    std::unordered_map<x86::reg32, Texture> m_textures;  // by the address the game downloaded to
    x86::reg32              m_nextTexture;
    x86::reg32              m_texture;
    x86::reg32              m_textureLevels;
    int                     m_textureFormat;
    x86::reg32              m_renderStamp;
    std::vector<std::array<float, 64>> m_fogTables;
    int                     m_fogTableIndex;
    x86::reg8               m_fogTableRaw[64];
    bool                    m_fogTableValid;
    x86::reg32              m_fogMode;
    float                   m_fogColor[3];
    float                   m_alphaTestRef;
    float                   m_colorFactor;
    float                   m_alphaFactor;
    bool                    m_zBuffer;
    bool                    m_zTest;
    bool                    m_depthAlways;
    bool                    m_alphaBlend;
    bool                    m_cull;
    bool                    m_chromaKey;
    bool                    m_wrapS;
    bool                    m_wrapT;
    x86::reg32              m_cullMode;
    x86::reg32              m_chromaKeyColor;
    x86::reg32              m_clipMinX;
    x86::reg32              m_clipMinY;
    x86::reg32              m_clipMaxX;
    x86::reg32              m_clipMaxY;
    float                   m_gamma;
};

}

#endif
