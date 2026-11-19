#include <lib/thrashrenderer.h>
#include <lib/renderer.h>
#include <SDL3/SDL.h>
#include <lib/glcompat.h>
#include <lib/glfuncs.h>
#include <lib/glthread.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace win32
{

static const char g_preamble[] = NFS_GLSL_PREAMBLE;

// EXT_texture_filter_anisotropic's, core in OpenGL 4.6.
static const GLenum kTextureMaxAnisotropy = 0x84FE;
static const GLenum kMaxTextureMaxAnisotropy = 0x84FF;

/* GlideRenderer's vertex shader without the atlas: the fog factor from the
 * draw's table at the corner's w, worked out here (see gliderenderer.cpp). */
static const char g_vertexShader[] = ""
"in vec3 g_position;"
"in vec4 g_color;"
"in vec3 g_texCoord;"
"in vec4 g_combine;"
"out vec3 v_texCoord;"
"out vec4 v_color;"
"out float v_fog;"
"flat out vec4 v_combine;"
"out float v_facing;"
"uniform mat4 u_transform;"
"uniform float u_fogW[64];"
"uniform float u_fogTable[64];"
"float fogFactor(float oow)"
"{"
"    if (oow <= 0.0) return 0.0;"
"    float w = 1.0 / oow;"
"    if (w <= u_fogW[0]) return u_fogTable[0];"
"    if (w >= u_fogW[63]) return u_fogTable[63];"
"    int octave = 4 * (int(floatBitsToUint(w) >> 23u) - 127);"
"    int lo = octave + int(w >= u_fogW[octave + 1]) + int(w >= u_fogW[octave + 2])"
"           + int(w >= u_fogW[octave + 3]);"
"    float span = u_fogW[lo + 1] - u_fogW[lo];"
"    float t = span > 0.0 ? (w - u_fogW[lo]) / span : 0.0;"
"    return u_fogTable[lo] + (u_fogTable[lo + 1] - u_fogTable[lo]) * t;"
"}"
"void main()"
"{"
"    v_texCoord = g_texCoord;"
"    v_combine = g_combine;"
"    v_facing = g_combine.p;"
"    v_color = g_color;"
"    v_fog = g_combine.q > 0.5 ? fogFactor(g_texCoord.p) : 0.0;"
"    gl_Position = u_transform * vec4(g_position, 1.0);"
"}";

/* The texture through the GPU's own sampler: bilinear, the nearest mipmap
 * level, wrapped or clamped -- what the game asks the Voodoo2 for, in one
 * fetch.  A Glide coordinate spans 256 over the whole texture, whatever its
 * size.
 *
 * With a chroma key the texels of the key colour have to be dropped before
 * filtering, as the Voodoo2 did, or a halo of half-keyed colour rings every
 * cut-out edge; for those draws the four texels are fetched and weighed here,
 * as GlideRenderer does for everything. */
static const char g_fragmentShader[] =
"in vec3 v_texCoord;"
"in vec4 v_color;"
"flat in vec4 v_combine;"
"in float v_facing;"
"in float v_fog;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_texture;"
"uniform vec3 u_fogColor;"
"uniform float u_alphaRef;"
"uniform float u_depthCut;"
"uniform bool u_chromaKey;"
"uniform ivec3 u_chromaKeyBytes;"
"uniform int u_format;"
"uniform float u_gamma;"
"uniform vec2 u_wrap;"
"uniform float u_levels;"
"uniform float u_detail;"
"uniform float u_detailScale;"
"uniform sampler2D u_detailTexture;"
"uniform float u_wet;"
"uniform sampler2D u_reflection;"
"uniform vec2 u_screen;"
"uniform float u_surface;"
"uniform bool u_sharpen;"
"uniform float u_carShine;"
"uniform float u_haze;"
"uniform float u_headlight;"
"uniform vec3 u_headOrigin;"
"uniform vec3 u_headDir;"
"uniform vec3 u_headUp;"
"uniform vec2 u_projScale;"
"uniform vec2 u_projCentre;"
"uniform sampler2D u_sky;"
"uniform float u_fogW[64];"
"uniform float u_fogTable[64];"
/* The fog factor at a pixel's own w, as the vertex shader works it out at the
 * corners (haze, below): 1/w is linear across the screen, so it is exact. */
"float pixelFog(float oow)"
"{"
"    if (oow <= 0.0) return 0.0;"
"    float w = 1.0 / oow;"
"    if (w <= u_fogW[0]) return u_fogTable[0];"
"    if (w >= u_fogW[63]) return u_fogTable[63];"
"    int octave = 4 * (int(floatBitsToUint(w) >> 23u) - 127);"
"    int lo = octave + int(w >= u_fogW[octave + 1]) + int(w >= u_fogW[octave + 2])"
"           + int(w >= u_fogW[octave + 3]);"
"    float span = u_fogW[lo + 1] - u_fogW[lo];"
"    float t = span > 0.0 ? (w - u_fogW[lo]) / span : 0.0;"
"    return u_fogTable[lo] + (u_fogTable[lo + 1] - u_fogTable[lo]) * t;"
"}"
/* Whether a texel is the key colour, compared as GlideRenderer compares it:
 * against the bytes its own conversion to RGBA8 gave the texel -- 565 widened
 * by a shift, 1555 scaled, 4444 and 8888 exact -- so a key matches the texels
 * it matched before the textures stayed in their own formats. */
"bool keyed(vec4 t)"
"{"
"    ivec3 bytes;"
"    if (u_format == 1)"
"        bytes = ivec3(int(round(t.r * 31.0)) << 3, int(round(t.g * 63.0)) << 2, int(round(t.b * 31.0)) << 3);"
"    else if (u_format == 2)"
"        bytes = ivec3(round(t.rgb * 31.0)) * 255 / 31;"
"    else"
"        bytes = ivec3(round(t.rgb * 255.0));"
"    if (all(equal(bytes, u_chromaKeyBytes)))"
"        return true;"
/* A 565 texel's byte is either the 5- or 6-bit value shifted up (0 to 248) or
 * that value scaled to 0 to 255: the game's own keys are given in one of the
 * two, its pictures being 565 as often as not (the weather's drops are, their
 * blank green 0x07e0 around them), so both are compared. */
"    return u_format == 1 && all(equal(ivec3(round(t.rgb * 255.0)), u_chromaKeyBytes));"
"}"
/* How much of a quad is inside its circle: 1 well inside, 0 at its rim and
 * beyond.  A cloud's or a flake's ragged edge is scaled by its noise, which
 * leaves it out of reach of the edge itself -- and the straight side of the quad
 * showed, a turned square of dimmed picture over the view. */
"float inside(vec2 uv)"
"{"
"    return 1.0 - smoothstep(0.75, 1.0, length(uv));"
"}"
"vec4 texel(vec2 at, float size, int level)"
"{"
"    vec2 t = mix(clamp(at, 0.0, size - 1.0), mod(at, size), u_wrap);"
"    return texelFetch(u_texture, ivec2(t), level);"
"}"
/* A texture seen larger than it is (initGl, NFS_TEXTURE_SHARP): Catmull-Rom
 * over the sixteen texels around the point, in five bilinear reads (below), instead
 * of the GPU's bilinear over four -- the track's 64 x 64 pictures spread over
 * a wall or a hillside keep their edges instead of melting into a cross-hatched
 * blur -- in the world only (what is fogged): the menus' lettering, cut out
 * of its own small pictures, came out ringed and smeared.  Where a texel is a
 * pixel or smaller the GPU's own filtering (the
 * levels, trilinear, anisotropic) is the better one and is what is used; the
 * two are blended across the step, so no line shows where one gives way. */
/* Five of the nine (2026-11-18): the four corner reads carry the products of
 * the curve's two small end weights, a few hundredths, and are left out, the
 * rest renormalised -- no difference to be seen -- and the GPU's own filtered
 * read is made only where it is blended in, not under every magnified pixel
 * (with 16x anisotropic filtering, the dearest read of them all). */
"vec4 sharpTexture(vec2 uv, vec2 gx, vec2 gy)"
"{"
"    vec2 size = vec2(textureSize(u_texture, 0));"
"    float rho = max(length(gx * size), length(gy * size));"
"    if (rho >= 1.0) return textureGrad(u_texture, uv, gx, gy);"
"    vec2 p = uv * size;"
"    vec2 t1 = floor(p - 0.5) + 0.5;"
"    vec2 f = p - t1;"
"    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));"
"    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);"
"    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));"
"    vec2 w3 = f * f * (-0.5 + 0.5 * f);"
"    vec2 w12 = w1 + w2;"
"    vec2 a = (t1 - 1.0) / size, b = (t1 + w2 / w12) / size, c = (t1 + 2.0) / size;"
"    float wn = w12.x * w0.y, ww = w0.x * w12.y, wm = w12.x * w12.y, we = w3.x * w12.y, ws = w12.x * w3.y;"
"    vec4 sum = textureLod(u_texture, vec2(b.x, a.y), 0.0) * wn"
"             + textureLod(u_texture, vec2(a.x, b.y), 0.0) * ww"
"             + textureLod(u_texture, vec2(b.x, b.y), 0.0) * wm"
"             + textureLod(u_texture, vec2(c.x, b.y), 0.0) * we"
"             + textureLod(u_texture, vec2(b.x, c.y), 0.0) * ws;"
"    vec4 sharp = clamp(sum / (wn + ww + wm + we + ws), 0.0, 1.0);"
"    float blend = smoothstep(1.0, 0.6, rho);"
"    if (blend >= 1.0) return sharp;"
"    return mix(textureGrad(u_texture, uv, gx, gy), sharp, blend);"
"}"
"void main()"
"{"
"    vec2 uv = (v_texCoord.st / v_texCoord.p) / 256.0;"
"    vec2 gx = dFdx(uv), gy = dFdy(uv);"
"    vec4 textureColor;"
"    if (u_chromaKey) {"
"        float size = float(textureSize(u_texture, 0).x);"
"        vec2 texelPos = uv * size;"
"        float rho = max(length(dFdx(texelPos)), length(dFdy(texelPos)));"
"        float lod = clamp(floor(log2(max(rho, 1e-6)) + 0.5), 0.0, max(u_levels - 1.0, 0.0));"
"        float scale = exp2(lod);"
"        float tile = max(size / scale, 1.0);"
"        vec2 pos = texelPos / scale;"
"        pos = mix(pos, mod(pos, tile), u_wrap);"
"        pos = mix(clamp(pos, 0.5, max(tile - 0.5, 0.5)), pos, u_wrap);"
"        vec2 base = floor(pos - 0.5);"
"        vec2 w = pos - 0.5 - base;"
"        int level = int(lod);"
"        vec4 t00 = texel(base, tile, level);"
"        vec4 t10 = texel(base + vec2(1.0, 0.0), tile, level);"
"        vec4 t01 = texel(base + vec2(0.0, 1.0), tile, level);"
"        vec4 t11 = texel(base + vec2(1.0, 1.0), tile, level);"
"        vec4 keep = vec4(keyed(t00) ? 0.0 : 1.0, keyed(t10) ? 0.0 : 1.0,"
"                         keyed(t01) ? 0.0 : 1.0, keyed(t11) ? 0.0 : 1.0);"
"        vec4 wgt = vec4((1.0 - w.x) * (1.0 - w.y), w.x * (1.0 - w.y),"
"                        (1.0 - w.x) * w.y,         w.x * w.y) * keep;"
"        float wsum = wgt.x + wgt.y + wgt.z + wgt.w;"
"        if (wsum <= 0.0) discard;"
"        textureColor = (t00 * wgt.x + t10 * wgt.y + t01 * wgt.z + t11 * wgt.w) / wsum;"
"    } else if (u_sharpen && v_combine.q > 0.5) {"
"        textureColor = sharpTexture(uv, gx, gy);"
"    } else {"
"        textureColor = texture(u_texture, uv);"
"    }"
"    vec4 color = vec4(mix(v_color.rgb, v_color.rgb * textureColor.rgb, v_combine.s),"
"                      mix(v_color.a, v_color.a * textureColor.a, v_combine.t));"
"    if (color.a <= u_alphaRef) discard;"
/* What is blended in and writes its depth too -- the game's flakes and spray
 * sprites -- writes it only where it shows: the all but clear rest of its
 * square left the square's depth over what lies behind, and the port's
 * particles (drawParticles), hidden behind it, left a hole of its shape. */
"    if (color.a < u_depthCut) discard;"
/* The player's headlights on what stands beside and across the road (initGl,
 * NFS_HEADLIGHT_OBJECTS): the walls, fences, trees, signs and buildings, which
 * the game's own patch of light on the road never reached.  The pixel's
 * place in the view comes back from the screen and its own 1/w through the
 * projection (u_projScale, u_projCentre: what sub_4bf4c0 uses), its facing
 * from how that place changes across the screen; the lamps' place and aim are
 * the game's (native_render.cpp, noteHeadlight).  A cone, bright on its axis
 * and soft to its edge, falling off with distance, brighter on what faces the
 * lamps -- and the surface's own texture colour lit by it, added. */
"    if (u_headlight > 0.0 && v_combine.s > 0.5 && v_combine.q > 0.5) {"
"        float z = 1.0 / max(v_texCoord.p, 1e-6);"
"        vec3 P = vec3((gl_FragCoord.xy - u_projCentre) * z / u_projScale, z);"
"        vec3 L = u_headOrigin - P;"
"        float dist = length(L);"
"        vec3 toLight = L / max(dist, 1e-3);"
/* Wide and soft (2026-11-17): a narrow cone bright on its axis showed as a
 * brighter beam of its own inside the game's patch on the road. */
"        const float kConeOuter = 0.45, kConeInner = 0.95, kLevelShare = 0.08;"
"        float cone = smoothstep(kConeOuter, kConeInner, dot(-toLight, u_headDir));"
"        cone *= cone;"
"        float reach = 1.0 / (1.0 + dist * dist / 400.0) * (1.0 - smoothstep(45.0, 70.0, dist));"
"        vec3 n = cross(dFdx(P), dFdy(P));"
"        vec3 nn = dot(n, n) > 0.0 ? normalize(n) : -toLight;"
"        float facing = abs(dot(nn, toLight));"
/* What lies level -- the road, which has the game's own patch of light, and
 * the ground -- gets a quarter of it; what stands up gets all of it. */
"        float level = smoothstep(0.6, 0.9, abs(dot(nn, u_headUp)));"
"        float lit = u_headlight * cone * reach * (0.3 + 0.7 * facing) * mix(1.0, kLevelShare, level);"
"        color.rgb += textureColor.rgb * vec3(1.0, 0.96, 0.86) * lit;"
"    }"
/* The rest of the world's grain (initGl, NFS_OBJECT_DETAIL): the same noise,
 * fainter and twice to a texture, on the walls, the buildings, the grass and
 * the rocks -- the opaque texels only, so smoke and glass stay as they are. */
"    if (u_surface > 0.0 && v_combine.s > 0.5 && v_combine.q > 0.5 && textureColor.a > 0.99) {"
"        float fine = textureGrad(u_detailTexture, uv * 2.0 + vec2(0.21, 0.53), gx * 2.0, gy * 2.0).r;"
"        color.rgb *= 1.0 + u_surface * (fine - 0.5) * 2.0;"
"    }"
/* The ground's grain (initGl): a fine noise over the texture, in its own
 * coordinates so it lies on the surface, lighter and darker by u_detail.  Far
 * off its mipmaps average to grey and it is gone of itself. */
"    if (u_detail > 0.0 && v_combine.s > 0.5 && v_combine.q > 0.5) {"
"        float grain = texture(u_detailTexture, uv * u_detailScale).r;"
"        color.rgb *= 1.0 + u_detail * (grain - 0.5) * 2.0;"
/* A wet road (setWetRoad): darker, puddled, and reflecting.  1/w is linear
 * down the screen over a level surface and reaches 0 at the horizon, so the
 * horizon's y is here's less 1/w over its rate down the screen.  Seen in the
 * road, what stands far beyond appears mirrored about the horizon, and what
 * stands close by just above the point; so the last frame (u_reflection) is
 * read at five points from here up to the mirror image and blended -- the
 * streaks a wet road draws under lights.  Strongest at a grazing angle (near
 * the horizon), faint looking down; only on surfaces that lie level (1/w
 * changing down the screen, not across), and not from the HUD's top band. */
"        if (u_wet > 0.0) {"
"            float puddle = smoothstep(0.42, 0.62, texture(u_detailTexture, uv * 1.3 + vec2(0.37, 0.71)).r);"
"            float wet = u_wet * (0.55 + 0.45 * puddle);"
"            color.rgb *= 1.0 - 0.32 * wet;"
"            float rate = dFdy(v_texCoord.p);"
"            float across = abs(dFdx(v_texCoord.p));"
"            if (rate > 0.0 && across < 0.6 * rate) {"
"                float y = gl_FragCoord.y;"
"                float horizon = y - v_texCoord.p / rate;"
"                float below = (y - horizon) / u_screen.y;"
"                if (horizon > 0.0 && below > 0.0) {"
"                    float mirror = 2.0 * horizon - y;"
"                    float ripple = (grain - 0.5) * 0.012 * u_screen.x;"
"                    vec3 sum = vec3(0.0);"
"                    vec3 brightest = vec3(0.0);"
"                    for (int i = 1; i <= 5; ++i) {"
"                        float ry = mix(y, mirror, float(i) / 5.0);"
"                        float fade = smoothstep(0.10 * u_screen.y, 0.17 * u_screen.y, ry);"
"                        vec3 c = texture(u_reflection, vec2((gl_FragCoord.x + ripple * float(i) * 0.4) / u_screen.x, ry / u_screen.y)).rgb * fade;"
"                        sum += c;"
"                        brightest = max(brightest, c);"
"                    }"
"                    vec3 seen = sum / 5.0 + max(brightest - 0.6, 0.0) * 1.5;"
"                    float fresnel = 0.08 + 0.55 * (1.0 - smoothstep(0.0, 0.3, below));"
/* Not over what is laid on the road half seen -- the tyre marks: lit by the
 * reflection as brightly as the road around them, they were gone in the rain
 * (2026-11-17).  The road under them reflects, through them. */
"                    color.rgb += seen * fresnel * wet * (0.35 + 0.65 * puddle) * step(0.98, color.a);"
"                }"
"            }"
"        }"
"    }"
/* A car's paint (initGl, NFS_CAR_SHINE): its reflection, the pass the game
 * adds over the body from a sphere map, shaped by how squarely the body faces
 * the view there (v_facing, per vertex from the reflection's own normals,
 * 1 edge-on to 255 face-on): faint where the paint faces the eye, strong
 * towards the edges as a lacquer reflects (Schlick's curve), and its bright
 * parts brought up and its dim ones down, so the highlights are sharp spots
 * and streaks instead of a wash over the whole car.  Facing interpolates
 * smoothly across each triangle, so the shine follows the body's curves
 * rather than its facets. */
"    if (u_carShine > 0.0 && v_facing > 0.5) {"
"        float facing = clamp((v_facing - 1.0) / 254.0, 0.0, 1.0);"
"        float edge = 1.0 - facing;"
"        float fresnel = 0.3 + 2.2 * edge * edge * edge;"
"        vec3 r = color.rgb;"
"        vec3 shaped = (r * 0.45 + r * r * 1.9) * fresnel;"
"        color.rgb = mix(r, shaped, u_carShine);"
"    }"
/* Haze (initGl, NFS_HAZE): the fog worked out at each pixel instead of at
 * the corners -- a long polygon no longer fogs in a straight ramp across it --
 * and fading into the sky behind it rather than into one flat colour: the sky
 * as the frame drew it before the world (u_sky, taken by takeSky), blurred
 * down its levels, read at this pixel and brought back from the gamma the
 * picture went out at.  Far hills turn the sky's own colour where they meet
 * it, blue above and pale at the horizon, and the seam between the two goes. */
"    float fog = v_fog;"
"    vec3 fogColor = u_fogColor;"
"    if (u_haze > 0.0 && v_combine.q > 0.5) {"
"        fog = pixelFog(v_texCoord.p);"
"        vec3 sky = textureLod(u_sky, gl_FragCoord.xy / u_screen, 4.5).rgb;"
"        sky = pow(max(sky, vec3(0.0)), vec3(max(u_gamma, 0.001)));"
"        fogColor = mix(u_fogColor, sky, u_haze);"
"    }"
"    color.rgb = mix(color.rgb, fogColor, clamp(fog, 0.0, 1.0));"
"    if (u_gamma != 1.0)"
"        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(1.0 / max(u_gamma, 0.001)));"
"    o_color = color;"
/* The Voodoo's 16-bit depth steps where the buffer has finer ones
 * (needsDepthSteps, gliderenderer.cpp). */
"\n#ifdef NFS_DEPTH16\n"
"    gl_FragDepth = (floor(gl_FragCoord.z * 65536.0) + 0.5) / 65536.0;"
"\n#endif\n"
"}";

static const x86::reg32 s_maxVertices = ThrashRenderer::s_maxVertices;

static std::mutex s_poolMutex;
static std::vector<ThrashVertex*> s_pool;

static ThrashVertex* takeVertices()
{
    {
        std::lock_guard<std::mutex> lock(s_poolMutex);
        if (!s_pool.empty())
        {
            ThrashVertex* vertices = s_pool.back();
            s_pool.pop_back();
            return vertices;
        }
    }
    return new ThrashVertex[s_maxVertices];
}

static void giveVertices(ThrashVertex* vertices)
{
    std::lock_guard<std::mutex> lock(s_poolMutex);
    s_pool.push_back(vertices);
}

/* What renderPending() hands the GL thread. */
struct ThrashDraw
{
    ThrashVertex*                       vertices;
    x86::reg32                          vertexCount;
    std::vector<ThrashRenderer::Batch>  batches;
    std::vector<std::array<float, 64>>  fogTables;
    float                               fogColor[3];
    float                               wetness;
    float                               grade[5];
    float                               head[14];
    float                               sun[12];
    bool                                race;
    float                               speed;
    std::shared_ptr<const std::vector<EffectParticle>> particles;  // the port's own (drawParticles)
    float                               alphaTestRef;
    x86::reg32                          chromaKeyColor;
    x86::reg32                          width;
    x86::reg32                          height;
};

/* Glide's fog table w(i), as gliderenderer.cpp has it. */
static void fogW(float (&w)[64])
{
    for (int i = 0; i < 64; ++i)
        w[i] = float(std::pow(2.0, 3.0 + double(i >> 2)) / double(8 - (i & 3)));
}

ThrashRenderer::ThrashRenderer(Renderer* renderer)
    :   m_renderer(renderer)
    ,   m_depthBuffer(0)
    ,   m_framebuffer(0)
    ,   m_vertexBuffer(0)
    ,   m_vertexArray(0)
    ,   m_program(0)
    ,   m_samplers{0, 0, 0, 0}
    ,   m_locations{}
    ,   m_depth16(false)
    ,   m_vertices(takeVertices())
    ,   m_vertexCount(0)
    ,   m_nextTexture(0)
    ,   m_texture(0)
    ,   m_textureLevels(1)
    ,   m_textureFormat(0)
    ,   m_renderStamp(1)
    ,   m_fogTableIndex(0)
    ,   m_fogTableValid(false)
    ,   m_fogMode(0)
    ,   m_alphaTestRef(0.f)
    ,   m_colorFactor(1.f)
    ,   m_alphaFactor(1.f)
    ,   m_zBuffer(true)
    ,   m_zTest(true)
    ,   m_depthAlways(false)
    ,   m_alphaBlend(true)
    ,   m_cull(false)
    ,   m_chromaKey(false)
    ,   m_wrapS(true)
    ,   m_wrapT(true)
    ,   m_cullMode(0)
    ,   m_chromaKeyColor(0)
    ,   m_clipMinX(0)
    ,   m_clipMinY(0)
    ,   m_clipMaxX(0)
    ,   m_clipMaxY(0)
    ,   m_gamma(1.f)
{
    m_fogColor[0] = m_fogColor[1] = m_fogColor[2] = 0.f;
    m_fogTables.assign(1, std::array<float, 64>());
    glthread::post(m_renderer, [this]() { initGl(); });
    glthread::finish();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] native THRASH renderer: a texture of OpenGL's for each of the game's");
}

ThrashRenderer::~ThrashRenderer()
{
    const unsigned int framebuffer = m_framebuffer, depthBuffer = m_depthBuffer, program = m_program;
    const unsigned int vertexBuffer = m_vertexBuffer, vertexArray = m_vertexArray;
    glthread::post(m_renderer, [this, framebuffer, depthBuffer, program, vertexBuffer, vertexArray]() {
        for (unsigned int& texture : m_glTextures)
            if (texture)
                glDeleteTextures(1, &texture);
        m_glTextures.clear();
        glDeleteSamplers(4, m_samplers);
        if (m_reflection)
            glDeleteTextures(1, &m_reflection);
        if (m_detailTexture)
            glDeleteTextures(1, &m_detailTexture);
        for (unsigned int texture : { m_sky, m_postCopy, m_bright, m_postDepth, m_snowAtlas })
            if (texture)
                glDeleteTextures(1, &texture);
        if (m_postFramebuffer)
            glDeleteFramebuffers(1, &m_postFramebuffer);
        if (m_depthFramebuffer)
            glDeleteFramebuffers(1, &m_depthFramebuffer);
        if (m_postArray)
            glDeleteVertexArrays(1, &m_postArray);
        if (m_brightProgram)
            glDeleteProgram(m_brightProgram);
        if (m_compositeProgram)
            glDeleteProgram(m_compositeProgram);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteRenderbuffers(1, &depthBuffer);
        glDeleteProgram(program);
        glDeleteBuffers(1, &vertexBuffer);
        glDeleteVertexArrays(1, &vertexArray);
    });
    glthread::finish();
    giveVertices(m_vertices);
}

/* The ground's grain: 256 x 256, tiling, around a mean of a half -- asphalt's
 * own speckle, a texel or two across, over a softer mottle of a few dozen.
 * Value noise on a wrapped lattice, so the tile has no seam; a fixed seed, so
 * every run looks the same. */
static std::vector<x86::reg8> grainTexels()
{
    const int size = 256;
    std::vector<float> sum(size * size, 0.0f);
    std::uint32_t seed = 0x9e3779b9u;
    auto next = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return float(seed & 0xffffff) / float(0xffffff);
    };
    struct Octave { int cell; float weight; };
    const Octave octaves[] = { { 1, 0.55f }, { 2, 0.25f }, { 8, 0.12f }, { 32, 0.08f } };
    for (const Octave& octave : octaves)
    {
        const int cells = size / octave.cell;
        std::vector<float> lattice(cells * cells);
        for (float& v : lattice)
            v = next();
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float fx = float(x) / octave.cell, fy = float(y) / octave.cell;
                const int x0 = int(fx) % cells, y0 = int(fy) % cells;
                const int x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
                float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
                tx = tx * tx * (3 - 2 * tx);
                ty = ty * ty * (3 - 2 * ty);
                const float a = lattice[y0 * cells + x0], b = lattice[y0 * cells + x1];
                const float c = lattice[y1 * cells + x0], d = lattice[y1 * cells + x1];
                sum[y * size + x] += octave.weight * ((a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty);
            }
    }
    double mean = 0, square = 0;
    for (float v : sum) { mean += v; square += double(v) * v; }
    mean /= sum.size();
    const double deviation = std::sqrt(square / sum.size() - mean * mean);
    std::vector<x86::reg8> texels(sum.size());
    for (std::size_t i = 0; i < sum.size(); ++i)
    {
        const double v = 0.5 + (sum[i] - mean) / deviation * 0.17;  // about +-0.5 at three deviations
        texels[i] = x86::reg8(std::lround(std::min(1.0, std::max(0.0, v)) * 255.0));
    }
    return texels;
}

void ThrashRenderer::initGl()
{
    loadGlFunctions();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glGenVertexArrays(1, &m_vertexArray);
    glBindVertexArray(m_vertexArray);
    glGenBuffers(1, &m_vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    /* Port: the textures filtered as a GPU of now would.  The game hands every
     * texture over as one level -- the track, the cars, the sky, all of them --
     * so the Voodoo's own "nearest mipmap" had nothing to choose from and
     * everything far away was sampled from the full-size picture: the road's
     * markings broke into blocks towards the horizon and shimmered as the car
     * moved.  So the levels below are made here (upload, glGenerateMipmap), the
     * sampler blends between two of them (trilinear), and anisotropic
     * filtering keeps the road sharp where it is seen at a grazing angle --
     * NFS_ANISOTROPY, 8 by default, at most what the GPU offers; 1 for none.
     * NFS_TEXTURE_FILTER=0 is the Voodoo's way again.  Draws with a chroma key
     * filter by hand from the game's own levels only (the shader's u_levels),
     * so a cut-out edge never picks up a level made here. */
    const char* filter = SDL_getenv("NFS_TEXTURE_FILTER");
    m_makeMipmaps = !(filter && SDL_strcmp(filter, "0") == 0);
    float anisotropy = 1.0f;
    if (m_makeMipmaps)
    {
        const char* wanted = SDL_getenv("NFS_ANISOTROPY");
        anisotropy = wanted && *wanted ? float(SDL_atof(wanted)) : 8.0f;
        GLfloat most = 0.0f;
        while (glGetError() != GL_NO_ERROR) {}
        glGetFloatv(kMaxTextureMaxAnisotropy, &most);
        if (glGetError() != GL_NO_ERROR || most < 1.0f)
            most = 1.0f;
        anisotropy = SDL_max(1.0f, SDL_min(anisotropy, most));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] textures: levels made for them, trilinear, %gx anisotropic (the GPU's most %g)",
                    double(anisotropy), double(most));
    }
    glGenSamplers(4, m_samplers);
    for (int i = 0; i < 4; ++i)
    {
        glSamplerParameteri(m_samplers[i], GL_TEXTURE_MIN_FILTER,
                            m_makeMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST);
        glSamplerParameteri(m_samplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        if (anisotropy > 1.0f)
            glSamplerParameterf(m_samplers[i], kTextureMaxAnisotropy, anisotropy);
    }

    glGenRenderbuffers(1, &m_depthBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, m_renderer->m_width, m_renderer->m_height);
    glGenFramebuffers(1, &m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthBuffer);
    GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffers);
    GLint depthBits = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    compileShaders(depthBits, (const char*)glGetString(GL_RENDERER));

    /* Port: grain on the road and the ground (NFS_ROAD_DETAIL, how much
     * lighter and darker it makes them: 0.35 by default, 0 for none).  The
     * track's textures are 1998's, a few texels to a stretch of road, and up
     * close the road was a smooth blur; a fine noise over them in their own
     * coordinates, eight to a texture, gives it a surface again.  Its mipmaps
     * average it to grey, so it fades by itself with distance and costs a
     * texture read where it is drawn. */
    const char* detail = SDL_getenv("NFS_ROAD_DETAIL");
    m_detailStrength = detail && *detail ? float(SDL_atof(detail)) : 0.35f;
    m_detailStrength = SDL_max(0.0f, SDL_min(m_detailStrength, 1.0f));
    if (m_detailStrength > 0.0f)
    {
        const std::vector<x86::reg8> grain = grainTexels();
        glGenTextures(1, &m_detailTexture);
        glBindTexture(GL_TEXTURE_2D, m_detailTexture);
        glTexStorage2D(GL_TEXTURE_2D, 9, GL_R8, 256, 256);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED, GL_UNSIGNED_BYTE, grain.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] grain on the road and the ground: %.2f", double(m_detailStrength));
    }
    /* Port: the rest of the world's grain (NFS_OBJECT_DETAIL, 0.3 by default,
     * 0 for none) -- the same noise as the road's, finer and fainter, on the
     * walls, buildings, grass and rocks, whose 64 x 64 pictures are stretched
     * as thin as the road's -- and the textures seen larger than they are
     * drawn bicubic (NFS_TEXTURE_SHARP, on; 0 for the GPU's bilinear). */
    const char* objects = SDL_getenv("NFS_OBJECT_DETAIL");
    m_surfaceStrength = objects && *objects ? float(SDL_atof(objects)) : 0.3f;
    m_surfaceStrength = m_detailTexture ? SDL_max(0.0f, SDL_min(m_surfaceStrength, 1.0f)) : 0.0f;
    /* And the cars' paint (NFS_CAR_SHINE, 1 by default, 0 for the game's own
     * reflection): see u_carShine in the shader. */
    const char* shine = SDL_getenv("NFS_CAR_SHINE");
    m_carShine = shine && *shine ? float(SDL_atof(shine)) : 1.0f;
    m_carShine = SDL_max(0.0f, SDL_min(m_carShine, 1.0f));
    const char* sharp = SDL_getenv("NFS_TEXTURE_SHARP");
    m_sharpen = !(sharp && SDL_strcmp(sharp, "0") == 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] the world's grain %.2f; magnified textures %s; the cars' shine %.2f",
                double(m_surfaceStrength), m_sharpen ? "bicubic" : "bilinear", double(m_carShine));
    glUseProgram(m_program);
    glUniform1i(m_locations.detailTexture, 1);
    glUniform1f(m_locations.surface, 0.0f);
    glUniform1i(m_locations.sharpen, 0);
    glUniform1f(m_locations.carShine, 0.0f);
    glUniform1f(m_locations.haze, 0.0f);
    glUniform1i(m_locations.sky, 3);
    glUniform1f(m_locations.headlight, 0.0f);
    glUniform1f(m_locations.detailScale, 8.0f);
    glUniform1f(m_locations.detail, 0.0f);
    glUniform1i(m_locations.reflection, 2);
    glUniform1f(m_locations.wet, 0.0f);
    glUseProgram(0);
    initPost();
}

static GLuint compileShader(GLenum type, const std::string& defines, const char* body)
{
    const GLchar* sources[3] = { g_preamble, defines.c_str(), body };
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 3, sources, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status)
    {
        GLchar log[2048];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof log, &length, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] native THRASH shader: %s", log);
    }
    return shader;
}

void ThrashRenderer::compileShaders(int depthBits, const char* renderer)
{
    m_depth16 = needsDepthSteps(depthBits, renderer);
    const char* depth16 = SDL_getenv("NFS_DEPTH16");
    if (depth16 && SDL_strcmp(depth16, "0") == 0)
        m_depth16 = false;
    else if (depth16 && SDL_strcmp(depth16, "1") == 0)
        m_depth16 = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] depth in %s (%d bits, %s)",
                m_depth16 ? "the Voodoo's 16-bit steps" : "the GPU's own precision", depthBits,
                renderer ? renderer : "?");

    const GLuint vertex = compileShader(GL_VERTEX_SHADER, "", g_vertexShader);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, m_depth16 ? "#define NFS_DEPTH16 1\n" : "",
                                          g_fragmentShader);
    m_program = glCreateProgram();
    glAttachShader(m_program, vertex);
    glAttachShader(m_program, fragment);
    glLinkProgram(m_program);
    GLint status = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &status);
    if (!status)
    {
        GLchar log[2048];
        GLsizei length = 0;
        glGetProgramInfoLog(m_program, sizeof log, &length, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] native THRASH program: %s", log);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    m_locations.position = glGetAttribLocation(m_program, "g_position");
    m_locations.color = glGetAttribLocation(m_program, "g_color");
    m_locations.texCoord = glGetAttribLocation(m_program, "g_texCoord");
    m_locations.combine = glGetAttribLocation(m_program, "g_combine");
    m_locations.transform = glGetUniformLocation(m_program, "u_transform");
    m_locations.fogW = glGetUniformLocation(m_program, "u_fogW");
    m_locations.fogTable = glGetUniformLocation(m_program, "u_fogTable");
    m_locations.fogColor = glGetUniformLocation(m_program, "u_fogColor");
    m_locations.alphaRef = glGetUniformLocation(m_program, "u_alphaRef");
    m_locations.depthCut = glGetUniformLocation(m_program, "u_depthCut");
    m_locations.chromaKey = glGetUniformLocation(m_program, "u_chromaKey");
    m_locations.chromaKeyColor = glGetUniformLocation(m_program, "u_chromaKeyBytes");
    m_locations.format = glGetUniformLocation(m_program, "u_format");
    m_locations.gamma = glGetUniformLocation(m_program, "u_gamma");
    m_locations.wrap = glGetUniformLocation(m_program, "u_wrap");
    m_locations.levels = glGetUniformLocation(m_program, "u_levels");
    m_locations.texture = glGetUniformLocation(m_program, "u_texture");
    m_locations.detail = glGetUniformLocation(m_program, "u_detail");
    m_locations.detailTexture = glGetUniformLocation(m_program, "u_detailTexture");
    m_locations.detailScale = glGetUniformLocation(m_program, "u_detailScale");
    m_locations.wet = glGetUniformLocation(m_program, "u_wet");
    m_locations.reflection = glGetUniformLocation(m_program, "u_reflection");
    m_locations.screen = glGetUniformLocation(m_program, "u_screen");
    m_locations.surface = glGetUniformLocation(m_program, "u_surface");
    m_locations.sharpen = glGetUniformLocation(m_program, "u_sharpen");
    m_locations.carShine = glGetUniformLocation(m_program, "u_carShine");
    m_locations.haze = glGetUniformLocation(m_program, "u_haze");
    m_locations.headlight = glGetUniformLocation(m_program, "u_headlight");
    m_locations.headOrigin = glGetUniformLocation(m_program, "u_headOrigin");
    m_locations.headDir = glGetUniformLocation(m_program, "u_headDir");
    m_locations.headUp = glGetUniformLocation(m_program, "u_headUp");
    m_locations.projection = glGetUniformLocation(m_program, "u_projScale");
    m_locations.projCentre = glGetUniformLocation(m_program, "u_projCentre");
    m_locations.sky = glGetUniformLocation(m_program, "u_sky");
    glUseProgram(m_program);
    glUniform1i(m_locations.texture, 0);
    float w[64];
    fogW(w);
    glUniform1fv(m_locations.fogW, 64, w);
    glUseProgram(0);
}

void ThrashRenderer::clear(x86::reg32 color)
{
    const x86::reg32 clipMinX = m_clipMinX, clipMinY = m_clipMinY, clipMaxX = m_clipMaxX, clipMaxY = m_clipMaxY;
    glthread::post(m_renderer, [this, color, clipMinX, clipMinY, clipMaxX, clipMaxY]() {
        clearFrame(color, clipMinX, clipMinY, clipMaxX, clipMaxY);
    });
}

/* Only the clip window is cleared, as on the Voodoo: the rear-view mirror is
 * drawn by clearing its rectangle and drawing the view behind into it. */
void ThrashRenderer::clearFrame(x86::reg32 color, x86::reg32 clipMinX, x86::reg32 clipMinY,
                                x86::reg32 clipMaxX, x86::reg32 clipMaxY)
{
    glDepthMask(true);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    if (clipMaxX > clipMinX && clipMaxY > clipMinY)
    {
        glEnable(GL_SCISSOR_TEST);
        glScissor(clipMinX, clipMinY, clipMaxX - clipMinX, clipMaxY - clipMinY);
    }
    else
    {
        glDisable(GL_SCISSOR_TEST);
    }
    glClearColor(float(color >> 16 & 0xff) / 255.0f, float(color >> 8 & 0xff) / 255.0f,
                 float(color & 0xff) / 255.0f, 0.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool traceFrameInteresting = false;
x86::reg32 traceFrameNumber = 0;

/* NFS_CAR_SHADOW, beside the other switches (below). */
static bool carShadowSetting();

/* NFS_TEXTURE_DUMP: the pictures of the sprites drawn over the view, saved
 * where the trace is -- <dir>/tex-<index>.bmp, and each line appended to
 * <dir>/trace.txt, so that a run says which picture is the one covering the
 * view: the alpha laid over a mid grey, so a black picture with a shape in its
 * alpha shows the shape.  Nothing without the switch. */
static const char* dumpDirectory()
{
    static const char* directory = SDL_getenv("NFS_TEXTURE_DUMP");
    return directory && *directory ? directory : nullptr;
}

void appendTraceLine(const char* tag, const char* text)
{
    const char* directory = dumpDirectory();
    if (!directory)
        return;
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/trace.txt", directory);
    if (SDL_IOStream* io = SDL_IOFromFile(path, "ab"))
    {
        char line[400];
        const int length = SDL_snprintf(line, sizeof line, "[%s] %s\n", tag, text);
        SDL_WriteIO(io, line, size_t(SDL_max(length, 0)));
        SDL_CloseIO(io);
    }
}

/* One texture, as a BMP: its RGB over mid grey by its alpha, upside down and
 * bottom up as a BMP has it. */
void dumpTexture(x86::reg32 index, GLuint name)
{
    const char* directory = dumpDirectory();
    if (!directory || !name)
        return;
    static std::vector<x86::reg32> done;
    if (std::find(done.begin(), done.end(), index) != done.end())
        return;
    GLint wide = 0, high = 0;
    glBindTexture(GL_TEXTURE_2D, name);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &wide);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &high);
    if (wide <= 0 || high <= 0 || wide > 1024 || high > 1024)
        return;
    done.push_back(index);
    SDL_Log("[SPRITE-QUAD] tex %08x is %dx%d: dumped to %s", unsigned(index), int(wide), int(high), directory);
    std::vector<unsigned char> texels(size_t(wide) * size_t(high) * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    const size_t rowLen = size_t(wide) * 3, padded = (rowLen + 3) & ~size_t(3);
    std::vector<unsigned char> rows(padded * size_t(high));
    for (int y = 0; y < high; ++y)
    {
        const unsigned char* from = &texels[size_t(y) * size_t(wide) * 4];
        unsigned char* to = &rows[size_t(high - 1 - y) * padded];
        for (int x = 0; x < wide; ++x)
        {
            const float a = from[x * 4 + 3] / 255.0f;
            for (int c = 0; c < 3; ++c)
                to[x * 3 + c] = static_cast<unsigned char>(SDL_min(255.0f, from[x * 4 + c] * a + 128.0f * (1.0f - a)));
        }
    }
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/tex-%08x.bmp", directory, unsigned(index));
    if (SDL_IOStream* io = SDL_IOFromFile(path, "wb"))
    {
        Uint8 header[54] = {0};
        header[0] = 'B'; header[1] = 'M';
        const Uint32 dataSize = Uint32(padded * size_t(high)), fileSize = 54 + dataSize, offset = 54, dibSize = 40;
        const Sint32 w32 = wide, h32 = high;
        const Uint16 planes = 1, bpp = 24;
        SDL_memcpy(header + 2, &fileSize, 4);
        SDL_memcpy(header + 10, &offset, 4);
        SDL_memcpy(header + 14, &dibSize, 4);
        SDL_memcpy(header + 18, &w32, 4);
        SDL_memcpy(header + 22, &h32, 4);
        SDL_memcpy(header + 26, &planes, 2);
        SDL_memcpy(header + 28, &bpp, 2);
        SDL_memcpy(header + 34, &dataSize, 4);
        SDL_WriteIO(io, header, sizeof header);
        SDL_WriteIO(io, rows.data(), rows.size());
        SDL_CloseIO(io);
    }
}

/* NFS_FRAME_DUMP=<folder>: a run of race frames as they go out, each at half
 * size as frame-NNNNN.bmp, for finding a frame that differs from the ones
 * round it.  From the bound framebuffer. */
static void dumpFrame(const char* directory, int number, int width, int height)
{
    const int wide = width / 2, high = height / 2;
    std::vector<unsigned char> full(size_t(width) * size_t(height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, full.data());
    const size_t rowLen = size_t(wide) * 3, padded = (rowLen + 3) & ~size_t(3);
    std::vector<unsigned char> rows(padded * size_t(high));
    for (int y = 0; y < high; ++y)
        for (int x = 0; x < wide; ++x)
            for (int c = 0; c < 3; ++c)
            {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        sum += full[(size_t(y * 2 + dy) * size_t(width) + size_t(x * 2 + dx)) * 4 + size_t(c)];
                // The picture is upside down in its texture (the blit turns it over),
                // and a BMP's rows go up from the bottom: row y is written from the
                // top.  Its texels are BGR.
                rows[size_t(high - 1 - y) * padded + size_t(x) * 3 + size_t(2 - c)] = (unsigned char)(sum / 4);
            }
    char path[512];
    SDL_snprintf(path, sizeof path, "%s/frame-%05d.bmp", directory, number);
    if (SDL_IOStream* io = SDL_IOFromFile(path, "wb"))
    {
        Uint8 header[54] = {0};
        header[0] = 'B'; header[1] = 'M';
        const Uint32 dataSize = Uint32(padded * size_t(high)), fileSize = 54 + dataSize, offset = 54, dibSize = 40;
        const Sint32 w32 = wide, h32 = high;
        const Uint16 planes = 1, bpp = 24;
        SDL_memcpy(header + 2, &fileSize, 4);
        SDL_memcpy(header + 10, &offset, 4);
        SDL_memcpy(header + 14, &dibSize, 4);
        SDL_memcpy(header + 18, &w32, 4);
        SDL_memcpy(header + 22, &h32, 4);
        SDL_memcpy(header + 26, &planes, 2);
        SDL_memcpy(header + 28, &bpp, 2);
        SDL_memcpy(header + 34, &dataSize, 4);
        SDL_WriteIO(io, header, sizeof header);
        SDL_WriteIO(io, rows.data(), rows.size());
        SDL_CloseIO(io);
    }
}

void ThrashRenderer::render(x86::reg32 buffer)
{
    m_renderer->m_currentBuffer = buffer;
}

void ThrashRenderer::flush()
{
    GlideCounters& counters = GlideRenderer::counters();
    if (m_vertexCount == (m_batches.empty() ? 0 : m_batches.back().end))
    {
        ++counters.emptyCalls;
        return;
    }
    ++counters.drawCalls;
    Batch batch;
    batch.end = m_vertexCount;
    batch.texture = m_texture;
    batch.levels = m_textureLevels;
    batch.format = m_textureFormat;
    batch.wrapS = m_wrapS;
    batch.wrapT = m_wrapT;
    batch.zTest = m_zTest;
    batch.zTestAlways = m_depthAlways;
    batch.zBuffer = m_zBuffer;
    batch.alphaBlend = m_alphaBlend;
    batch.cull = m_cull;
    batch.chromaKey = m_chromaKey;
    batch.chromaKeyColor = m_chromaKeyColor;
    batch.alphaRef = m_alphaTestRef;
    batch.cullMode = m_cullMode;
    batch.clipMinX = m_clipMinX;
    batch.clipMinY = m_clipMinY;
    batch.clipMaxX = m_clipMaxX;
    batch.clipMaxY = m_clipMaxY;
    batch.gamma = m_gamma;
    batch.fogTable = m_fogTableIndex;
    /* The grain goes on the solid, tiled surfaces of the world -- the road, the
     * ground, the walls: written to depth, not cut out, and wrapped, as the
     * track's textures are and the sky's and the HUD's are not -- and not a
     * car's body, whose textures wrap as well (setCarSurface).
     * (Blending says nothing: the game leaves it on for nearly everything.)  The shader adds that the draw is fogged, which the 2D
     * screens never are. */
    batch.detail = !m_carSurface && m_zTest && m_zBuffer && !m_chromaKey && (m_wrapS || m_wrapT);
    /* The rest of the world -- what is depth tested, the car bodies and the
     * ground's own grain apart: the track's walls, buildings, trees, fences,
     * the grass and the rocks.  The HUD and the menus are never depth tested.
     * Not what is added onto the picture (alphaBlend off is GL_ONE, additive
     * here): the headlights' pool, the glows -- light, not a surface, and a
     * grain over it came out as a white noise lying on the road at night. */
    batch.surface = !batch.detail && !m_carSurface && m_zTest && !m_chromaKey && m_alphaBlend;
    batch.sharpen = m_zTest && !m_chromaKey;
    // The car's reflection: the pass added (alphaBlend off) over its body.
    batch.carShine = m_carSurface && !m_alphaBlend;
    // The player's headlights on the world: the road and its walls (most of
    // the track, `detail`) and everything else in it; not the cars, not light.
    batch.headlit = !m_carSurface && m_zTest && !m_chromaKey && m_alphaBlend;
    /* The cars, and what the race adds over the world from gamedata/render --
     * smoke, dust, spray, sparks: 8888 and 4444 pictures where the track's are
     * 1555 and 565 -- are not things that stand in the sun's way. */
    batch.car = m_carSurface || (!m_textureShadow && (m_textureFormat == TF_ARGB_8888 || m_textureFormat == TF_ARGB_4444));
    batch.decal = m_textureShadow;
    batch.light = m_textureLight;
    batch.wetLight = m_textureWetStreak;
    batch.skipDraw = (batch.decal && !carShadowSetting()) || (m_textureWetStreak && !m_wetLightsLit);
    m_batches.push_back(batch);
    /* The batch's own screen box, for the trace: which of a frame's draws a
     * thing on the screen is. */
    const x86::reg32 from = m_batches.size() >= 2 ? m_batches[m_batches.size() - 2].end : 0;
    float boxMinX = 0.0f, boxMinY = 0.0f, boxMaxX = 0.0f, boxMaxY = 0.0f;
    if (batch.end > from)
    {
        boxMinX = m_vertices[from].x;
        boxMaxX = boxMinX;
        boxMinY = m_vertices[from].y;
        boxMaxY = boxMinY;
        for (x86::reg32 i = from + 1; i < batch.end; ++i)
        {
            boxMinX = std::min(boxMinX, m_vertices[i].x);
            boxMaxX = std::max(boxMaxX, m_vertices[i].x);
            boxMinY = std::min(boxMinY, m_vertices[i].y);
            boxMaxY = std::max(boxMaxY, m_vertices[i].y);
        }
    }
    /* NFS_SPRITE_TRACE=1: each kind of batch once -- the texture the game gave
     * it, its format and levels, whether it is cut out and by what, what it is
     * blended by, and whether it writes depth: for telling a sprite that is cut
     * out from one that is not. */
    if (static const bool traced = SDL_getenv("NFS_SPRITE_TRACE") != nullptr; traced)
    {
        static std::vector<std::string> seen;
        char line[320];
        SDL_snprintf(line, sizeof line,
                     "frame %u tex %08x levels %u fmt %d keyed %d key %08x ref %.2f blend %d depth %d/%d cull %d "
                     "clip %d,%d-%d,%d verts %d box %.0f,%.0f-%.0f,%.0f car %d decal %d light %d detail %d",
                     unsigned(traceFrameNumber), unsigned(m_texture), unsigned(m_textureLevels), batch.format, int(batch.chromaKey),
                     unsigned(batch.chromaKeyColor), batch.alphaRef, int(batch.alphaBlend), int(batch.zTest),
                     int(batch.zBuffer), int(batch.cull), int(batch.clipMinX), int(batch.clipMinY),
                     int(batch.clipMaxX), int(batch.clipMaxY), int(batch.end - from), double(boxMinX),
                     double(boxMinY), double(boxMaxX), double(boxMaxY), int(batch.car), int(batch.decal),
                     int(batch.light), int(batch.detail));
        bool known = false;
        for (const std::string& was : seen)
            known = known || was == line;
        /* Every kind of batch, however many there are: a race has hundreds,
         * and the one that is wrong is not among the first forty. */
        if (!known && seen.size() < 4000)
        {
            seen.push_back(line);
            SDL_Log("[SPRITE] %s", line);
            appendTraceLine("SPRITE", line);
        }
        /* A quad or two that covers a quarter of the view and more: what a
         * sprite drawn over the view is -- a drop on the lens, a cloud, a
         * shadow gone wrong -- and never a piece of the track.  Logged apart
         * and with its first corner's z and 1/w, which tell it apart: a
         * screen-space sprite hung on the overlay list (the drops on the view,
         * sub_4cb750) has z 0 and 1/w 0x3f7fff00. */
        const x86::reg32 start = from;
        const int corners = int(batch.end - from);
        if (corners > 0 && corners <= 12)
        {
            const float minX = boxMinX, maxX = boxMaxX, minY = boxMinY, maxY = boxMaxY;
            const float wide = float(batch.clipMaxX - batch.clipMinX), high = float(batch.clipMaxY - batch.clipMinY);
            /* Big, or a 565/1555 picture cut out by nothing, blended: a sprite
             * with no alpha of its own to hide its square with. */
            const bool noAlphaSprite = batch.format != 0 && batch.format != 3 && !batch.chromaKey
                                       && batch.alphaBlend && corners <= 24;
            /* A quad that is turned, inside a batch as well as a batch of its
             * own: the game draws a sprite with the same texture and state as
             * the surface round it, so a turned square hides in a batch of the
             * road's and the batch's own box says nothing of it.  Every six
             * vertices are a quad; one wide enough to see and neither of whose
             * edges runs level is logged with its own box, and its picture
             * dumped. */
            bool turnedQuad = false;
            for (x86::reg32 i = from; i + 2 < batch.end; i += 3)
            {
                float qMinX = m_vertices[i].x, qMaxX = qMinX, qMinY = m_vertices[i].y, qMaxY = qMinY;
                for (int c = 1; c < 3; ++c)
                {
                    qMinX = std::min(qMinX, m_vertices[i + x86::reg32(c)].x);
                    qMaxX = std::max(qMaxX, m_vertices[i + x86::reg32(c)].x);
                    qMinY = std::min(qMinY, m_vertices[i + x86::reg32(c)].y);
                    qMaxY = std::max(qMaxY, m_vertices[i + x86::reg32(c)].y);
                }
                const float qw = qMaxX - qMinX, qh = qMaxY - qMinY;
                if (qw < 0.12f * wide || qh < 0.12f * high)
                    continue;
                /* Squarish -- a sprite, not a strip of the road -- and with an
                 * edge that runs at an angle: a turned square. */
                const float aspect = qw > qh ? qw / SDL_max(qh, 1.0f) : qh / SDL_max(qw, 1.0f);
                if (aspect > 1.8f)
                    continue;
                bool slanted = false;
                for (int c = 0; c < 3; ++c)
                {
                    const x86::reg32 a = i + x86::reg32(c), b = i + x86::reg32((c + 1) % 3);
                    const float dx = m_vertices[b].x - m_vertices[a].x, dy = m_vertices[b].y - m_vertices[a].y;
                    const float level = std::min(std::fabs(dx), std::fabs(dy));
                    const float slope = std::max(std::fabs(dx), std::fabs(dy));
                    slanted = slanted || (slope > 0.0f && level > 0.25f * slope);
                }
                if (!slanted)
                    continue;
                turnedQuad = true;
                static std::vector<std::string> seenTurns;
                char turn[300];
                SDL_snprintf(turn, sizeof turn,
                             "tex %08x fmt %d keyed %d blend %d depth %d/%d quad %.0f,%.0f-%.0f,%.0f "
                             "(%.0fx%.0f) z %.4g car %d decal %d light %d detail %d",
                             unsigned(batch.texture), batch.format, int(batch.chromaKey), int(batch.alphaBlend),
                             int(batch.zTest), int(batch.zBuffer), double(qMinX), double(qMinY), double(qMaxX),
                             double(qMaxY), double(qw), double(qh), double(m_vertices[i].z), int(batch.car),
                             int(batch.decal), int(batch.light), int(batch.detail));
                if (seenTurns.size() < 400 && std::find(seenTurns.begin(), seenTurns.end(), std::string(turn)) == seenTurns.end())
                {
                    seenTurns.push_back(turn);
                    SDL_Log("[SPRITE-TURN] %s", turn);
                    appendTraceLine("TURN", turn);
                    if (batch.textureMean <= 0.35f && qw > 0.18f * wide && qh > 0.18f * high)
                        traceFrameInteresting = true;
                }
            }
            if (turnedQuad)
                m_batches.back().suspicious = true;
            if ((maxX - minX > 0.15f * wide && maxY - minY > 0.15f * high) || noAlphaSprite)
            {
                m_batches.back().suspicious = true;
                static std::vector<x86::reg32> big;
                if (big.size() < 4000 && std::find(big.begin(), big.end(), batch.texture) == big.end())
                {
                    big.push_back(batch.texture);
                    char quad[300];
                    SDL_snprintf(quad, sizeof quad,
                                 "tex %08x fmt %d keyed %d key %08x ref %.2f blend %d depth %d/%d verts %d "
                                 "box %.0f,%.0f-%.0f,%.0f z %.4g oow %08x car %d decal %d light %d detail %d",
                                 unsigned(batch.texture), batch.format, int(batch.chromaKey), unsigned(batch.chromaKeyColor),
                                 double(batch.alphaRef), int(batch.alphaBlend), int(batch.zTest), int(batch.zBuffer), corners,
                                 double(minX), double(minY), double(maxX), double(maxY),
                                 double(m_vertices[start].z), unsigned(m_vertices[start].oow), int(batch.car),
                                 int(batch.decal), int(batch.light), int(batch.detail));
                    SDL_Log("[SPRITE-QUAD] %s", quad);
                    appendTraceLine("QUAD", quad);
                    /* Only a *dark* picture is worth a frame of its own: the
                     * sky is big and turned too. */
                    if (batch.textureMean <= 0.35f)
                        traceFrameInteresting = true;
                }
            }
        }
    }
}

void ThrashRenderer::renderPending()
{
    if (!m_vertexCount)
        return;
    flush();
    std::shared_ptr<ThrashDraw> draw = std::make_shared<ThrashDraw>();
    draw->vertices = m_vertices;
    draw->vertexCount = m_vertexCount;
    draw->batches.swap(m_batches);
    draw->fogTables = m_fogTables;
    std::copy(m_fogColor, m_fogColor + 3, draw->fogColor);
    draw->alphaTestRef = m_alphaTestRef;
    draw->wetness = m_detailTexture ? m_wetness : 0.0f;
    std::copy(m_grade, m_grade + 5, draw->grade);
    std::copy(m_head, m_head + 14, draw->head);
    std::copy(m_sun, m_sun + 12, draw->sun);
    draw->race = m_raceView;
    draw->speed = m_speed;
    draw->particles = m_particles;
    draw->chromaKeyColor = m_chromaKeyColor;
    draw->width = m_renderer->m_width;
    draw->height = m_renderer->m_height;
    m_vertices = takeVertices();
    glthread::post(m_renderer, [this, draw]() {
        drawPending(*draw);
        giveVertices(draw->vertices);
    });
    m_vertexCount = 0;
    m_fogTables.front() = m_fogTables[m_fogTableIndex];
    m_fogTables.resize(1);
    m_fogTableIndex = 0;
    ++m_renderStamp;
}

/* The picture so far into m_reflection: one copy on the GPU, from the
 * framebuffer drawPending has bound. */
void ThrashRenderer::takeReflection(int width, int height)
{
    if (!m_reflection || m_reflectionW != width || m_reflectionH != height)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_reflection);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    m_reflectionValid = true;
    m_reflectionTaken = true;
}

/* Port: the whole picture, before its HUD (initGl's NFS_BLOOM, NFS_GRADE and
 * NFS_AA; each on by default, =0 for off).  As the world is drawn and the HUD
 * begins -- the moment the wet road's reflection is taken -- the picture is
 * copied (m_postCopy) and drawn back over itself through one pass:
 *
 * - anti-aliased: FXAA as Timothy Lottes first published it, five reads a
 *   pixel and a blur along the edge it finds, only where the edge is;
 * - with a glow: what is brighter than most of the picture -- headlights,
 *   lamps, the sun -- near white, not the sky's blue -- is kept at half size (m_bright, a bright
 *   pass), spread down that texture's levels and added back from five of
 *   them, wide and faint over narrow and strong;
 * - graded: a contrast about the middle grey, a saturation and a tint, the
 *   track's own (glide2x::colourGrade, from nfs3hp_main.cpp);
 * - shadowed: what stands by the road casts on what lies behind it, away from
 *   the sun, worked out from the depth buffer (sunShadow, below).
 *
 * The HUD then draws over it as it always did, and the menus are never
 * touched: they have no world to come before a HUD. */
static const char g_postVertexShader[] =
"out vec2 v_uv;"
"void main()"
"{"
"    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
"    v_uv = p;"
"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);"
"}";

static const char g_brightShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_source;"
"uniform sampler2D u_sky;"
"uniform bool u_hasSky;"
/* What glows is what stands brighter than the sky behind it (the haze's
 * u_sky, blurred): a lamp, a headlight, a tail light at night, the sun's
 * disc -- not a pale sky at the horizon by day, which is bright only because
 * it is the sky.  With no sky taken, near white alone. */
"void main()"
"{"
"    vec3 c = texture(u_source, v_uv).rgb;"
"    float peak = max(c.r, max(c.g, c.b));"
"    float floorLevel = 0.9;"
"    if (u_hasSky) {"
"        vec3 sky = textureLod(u_sky, v_uv, 4.5).rgb;"
"        floorLevel = max(max(sky.r, max(sky.g, sky.b)) + 0.08, 0.55);"
"    }"
"    o_color = vec4(c * smoothstep(floorLevel, floorLevel + 0.2, peak), 1.0);"
"}";

/* Auto-exposure (NFS_EXPOSURE): the picture brightened as it goes dark and
 * dimmed as it goes bright, as an eye or a camera does, so a tunnel's mouth
 * or a dark stretch is not a black hole for as long as the car is in it.
 *
 * The meter (g_meterShader): the picture, before its glow and grade, into a
 * 64x64 texture (m_meter), each texel the log brightness of sixteen of its
 * pixels -- in exposure::kStops stops below white, 0 to 1 -- weighted towards
 * the middle of the view (centre-weighted, as a camera meters), the weight
 * beside it so that the texture's levels down to one texel are the weighted
 * mean: the scene's brightness, as a geometric mean, so one bright lamp or a
 * patch of sky does not outweigh the rest.
 *
 * The adaptation (g_adaptShader): one texel (m_adapt, two of them in turn, the
 * last frame's read as this one's is written) keeps two brightnesses, each in
 * two bytes of 8 bits: the eye's -- following the scene within a second, faster
 * to the light than to the dark -- and the track's usual one, following it over
 * many seconds, slowly into the dark and faster out of it, so that a long
 * tunnel does not become the usual.  No reading back to the CPU: the composite
 * reads the texel itself.
 *
 * The composite then brightens or dims the picture by how far the eye's is
 * from the usual, in stops, less a dead zone (so a track's own light and shade
 * are left as they are), times NFS_EXPOSURE, within kMostUp and kMostDown. */
static const char g_meterShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_source;"
"uniform float u_stops;"
"uniform vec2 u_texel;"   // a quarter of a meter texel, in the picture's uv
"float stopsOf(vec3 c) { return clamp((log2(max(dot(c, vec3(0.299, 0.587, 0.114)), 1e-4)) + u_stops) / u_stops, 0.0, 1.0); }"
"void main()"
"{"
"    float t = 0.25 * (stopsOf(texture(u_source, v_uv + vec2(-u_texel.x, -u_texel.y)).rgb)"
"                    + stopsOf(texture(u_source, v_uv + vec2( u_texel.x, -u_texel.y)).rgb)"
"                    + stopsOf(texture(u_source, v_uv + vec2(-u_texel.x,  u_texel.y)).rgb)"
"                    + stopsOf(texture(u_source, v_uv + vec2( u_texel.x,  u_texel.y)).rgb));"
"    vec2 d = (v_uv - 0.5) * vec2(1.6, 2.0);"
"    float w = 0.25 + 0.75 * exp(-dot(d, d) * 2.0);"
"    o_color = vec4(t * w, w, 0.0, 1.0);"
"}";

static const char g_adaptShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_meter;"
"uniform sampler2D u_last;"
"uniform float u_meterLevel;"
"uniform vec4 u_rate;"   // the eye to the dark, to the light; the usual to the dark, to the light (1: at once)
"float unpack(vec2 c) { return c.x + c.y / 255.0; }"
"vec2 pack(float t) { float s = clamp(t, 0.0, 1.0) * 255.0; return vec2(floor(s) / 255.0, fract(s)); }"
"void main()"
"{"
"    vec4 m = textureLod(u_meter, vec2(0.5), u_meterLevel);"
"    float now = m.r / max(m.g, 1e-4);"
"    vec4 last = texture(u_last, vec2(0.5));"
"    float eye = unpack(last.rg), usual = unpack(last.ba);"
"    eye += (now - eye) * (now < eye ? u_rate.x : u_rate.y);"
"    usual += (now - usual) * (now < usual ? u_rate.z : u_rate.w);"
"    o_color = vec4(pack(eye), pack(usual));"
"}";

/* Whether the sky is overhead (skyOpen): of a grid over the top of the picture
 * (the picture's own rows from the top, before the HUD), the share where the
 * depth buffer is the sky's or as far (over 250 m, as the glare has it).  In a
 * tunnel, or under a bridge, that is all roof.  One texel kept from frame to
 * frame (two in turn), eased towards it; the particles read it to leave the
 * rain and snow out where nothing falls (drawParticles).  The composited snow
 * on the glass, though, is let go of slowly: its share is eased towards the
 * sky's here too, but over seconds, so a tunnel takes the flakes off the
 * windscreen as it would, not the instant the roof begins. */
static const char g_skyOpenShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_depth;"
"uniform sampler2D u_last;"
"uniform float u_rate;"
"uniform vec2 u_band;"   // from, to: the top of the picture, in its rows (0 the top)
"uniform float u_raining;"   // 1 while it rains (the road wet), else 0
"uniform vec3 u_lensRate;"   // the lens wetting, drying (a frame's share); the sky share it takes to wet
"uniform vec2 u_openFade;"   // from, to: no weather below the first share of sky, all of it from the second
"uniform vec2 u_snowRate;"   // the snow on the glass coming, going (a frame's share)
"float unpack(vec2 c) { return c.x + c.y / 255.0; }"
"vec2 pack(float t) { float s = clamp(t, 0.0, 1.0) * 255.0; return vec2(floor(s) / 255.0, fract(s)); }"
"void main()"
"{"
"    float open = 0.0;"
"    for (int j = 0; j < 6; ++j)"
"        for (int i = 0; i < 12; ++i) {"
"            vec2 at = vec2((float(i) + 0.5) / 12.0, mix(u_band.x, u_band.y, (float(j) + 0.5) / 6.0));"
"            open += step(0.996, texture(u_depth, at).r);"
"        }"
"    open /= 72.0;"
"    vec4 last = texture(u_last, vec2(0.5));"
/* And how wet the lens is (the windscreen's drops, the composite): wetting in
 * the rain under an open sky, drying under a roof or out of the rain, in two
 * bytes so a slow drying does not stall. */
"    float lens = unpack(last.ba);"
"    float wetting = u_raining * step(u_lensRate.z, open);"
"    lens += (wetting - lens) * (wetting > lens ? u_lensRate.x : u_lensRate.y);"
/* And how much of the weather lies on the glass (the composite's snow): all of
 * it under an open sky, easing away where the sky is shut out. */
"    float snowy = smoothstep(u_openFade.x, u_openFade.y, open);"
"    float snow = last.g;"
"    snow += (snowy - snow) * (snowy > snow ? u_snowRate.x : u_snowRate.y);"
"    o_color = vec4(mix(last.r, open, u_rate), snow, pack(lens));"
"}";

/* The soft particles' half-size layer (drawParticles), premultiplied, laid
 * over the picture. */
static const char g_softLayerShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_layer;"
"void main()"
"{"
"    o_color = texture(u_layer, v_uv);"
"}";

static const char g_compositeShader[] =
"in vec2 v_uv;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_source;"
"uniform sampler2D u_bright;"
"uniform vec2 u_pixel;"
"uniform float u_bloom;"
"uniform vec3 u_grade;"      // saturation, contrast, on
"uniform vec3 u_tint;"
"uniform bool u_aa;"
"uniform sampler2D u_depth;"
"uniform float u_shadow;"
"uniform vec3 u_sun;"
"uniform vec2 u_scale;"
"uniform vec2 u_centre;"
"uniform vec3 u_up;"
"uniform vec2 u_size;"
"uniform float u_blur;"
"uniform sampler2D u_light;"
"uniform float u_halo;"
"uniform float u_trailOn;"
"uniform float u_glare;"
"uniform vec2 u_glareAt;"
"uniform sampler2D u_adapt;"
"uniform vec4 u_exposure;"   // strength, dead zone, most up, most down (stops)
"uniform sampler2D u_skyOpen;"   // r: the sky overhead, g: the snow still on the glass, b, a: how wet the lens is
"uniform vec4 u_lens;"           // amount (0 for none), seconds, speed share (0 to 1), unused
"uniform sampler2D u_snowSprites;"
"uniform vec4 u_snow;"           // amount (0 to 1, eased), seconds, the speed's (1 to 2.5 times, eased), unused
"uniform float u_stops;"
"float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }"
/* Rain on the lens (NFS_LENS_RAIN): drops on the view, in cells of two grids
 * -- clusters (a main drop and two to five smaller ones broken off round it)
 * and a fine mist of single droplets.  A cell has its cluster or not by its
 * hash; it lives three to six seconds and comes again elsewhere in its cell.
 * Each drop a little narrower than it is tall, 0.8 to 1 of it by its hash.
 * A drop is a little lens: it shows the picture round it, upside down and
 * gathered from 2.6 to 3.8 times its size (the more the bigger the drop), as a
 * drop on a lens does, blended into the picture over its outer part so that
 * nothing outlines it; it catches a little light.  Still, a drop creeps down;
 * at speed it slides out towards the edge of the view and is gone sooner, the
 * small ones lagging behind it, a trail of what broke off.  A cluster and its
 * travel keep within half a cell of its middle, so a pixel looks at the four
 * cells nearest it in each grid and finds every drop that can reach it; a
 * cell whose cluster is out of reach is passed over at once.  How many there
 * are is how wet the lens is (g_skyOpenShader), times u_lens.x, each fading in
 * or out by its own hash. */
"float unpack2(vec2 c) { return c.x + c.y / 255.0; }"
"float lensHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }"
"void lensDrop(vec2 px, vec2 c, float r, float aspect, float big, float fade, inout vec2 pull, inout float best)"
"{"
"    vec2 d = px - c;"
"    vec2 e = vec2(d.x / aspect, d.y);"   /* its width against its height, 0.8 to 1 */
"    float body = 1.0 - dot(e, e) / (r * r);"
"    if (body <= 0.0) return;"
"    float show = smoothstep(0.0, 0.25, body) * fade;"
"    if (show <= best) return;"
"    best = show;"
"    float gather = mix(1.6, 2.8, clamp(r / big, 0.0, 1.0));"
"    pull = -d * (1.0 + gather);"
"}"
"void lensCell(vec2 px, vec2 id, float cell, float seed, float wet, float cluster, inout vec2 pull, inout float best)"
"{"
"    float h = lensHash(id + seed);"
"    float here = clamp((wet * u_lens.x * 0.6 - h) * 6.0, 0.0, 1.0);"
"    if (here <= 0.0) return;"
"    float period = mix(3.0, 6.0, lensHash(id + seed + 1.3));"
"    float tt = (u_lens.y + h * 37.0) / period;"
"    float cycle = floor(tt), t = fract(tt);"
"    float life = mix(0.9, 0.45, u_lens.z);"
"    if (t > life) return;"
"    float age = t / life;"
"    float big = cell * (cluster > 0.5 ? 0.08 : 0.24);"
"    float r = big * mix(0.55, 1.0, lensHash(id + cycle * 1.7 + seed));"
"    vec2 c = (id + vec2(lensHash(id + cycle * 0.37 + seed), lensHash(id + cycle * 0.71 + seed + 2.1))) * cell;"
"    float side = c.x < u_size.x * 0.5 ? -1.0 : 1.0;"
"    vec2 travel = vec2(side * u_lens.z, (1.0 - u_lens.z) * 0.4) * cell * 0.22 * age * age;"
"    float reach = (cluster > 0.5 ? 3.1 * r : r) + length(travel) + 1.0;"
"    if (distance(px, c) > reach) return;"
"    float fade = here * smoothstep(0.0, 0.04, t) * (1.0 - smoothstep(0.7, 1.0, age));"
"    lensDrop(px, c + travel, r, mix(0.8, 1.0, lensHash(id + cycle * 3.7 + seed)), big, fade, pull, best);"
"    if (cluster < 0.5) return;"
"    for (int k = 0; k < 5; ++k) {"
"        float n = float(k) + cycle * 7.0;"
"        if (lensHash(id + seed + n * 0.53) > 0.75) continue;"
"        float a = lensHash(id + seed + n * 3.1) * 6.2832;"
"        float apart = mix(1.35, 2.5, lensHash(id + seed + n * 1.7));"
"        float rs = r * mix(0.18, 0.42, lensHash(id + seed + n * 5.3));"
"        float lag = mix(0.25, 0.65, lensHash(id + seed + n * 2.9));"
"        lensDrop(px, c + vec2(cos(a), sin(a)) * apart * r + travel * lag, rs,"
"                 mix(0.8, 1.0, lensHash(id + seed + n * 4.1)), big, fade, pull, best);"
"    }"
"}"
"vec2 lensDrops(vec2 uv, out float best)"
"{"
"    best = 0.0;"
"    float wet = unpack2(texture(u_skyOpen, vec2(0.5)).ba);"
"    if (wet <= 0.01) return uv;"
"    vec2 px = uv * u_size;"
"    vec2 pull = vec2(0.0);"
"    float coarse = u_size.y / 2.7, fine = u_size.y / 10.0;"
"    vec2 a = floor(px / coarse - 0.5), b = floor(px / fine - 0.5);"
"    for (int j = 0; j < 2; ++j)"
"        for (int i = 0; i < 2; ++i) {"
"            lensCell(px, a + vec2(float(i), float(j)), coarse, 0.0, wet, 1.0, pull, best);"
"            lensCell(px, b + vec2(float(i), float(j)), fine, 17.3, wet, 0.0, pull, best);"
"        }"
"    return uv + pull / u_size;"
"}"
/* Snow on the glass: pixel-art clumps of wet snow (assets/snowflakes,
 * u_snowSprites; tools/generate_snowflakes.py) in cells of a field
 * over the view, falling down it -- each column a little faster or slower than
 * the next.  Each cell's flake lives SNOW_LIFE seconds, melting down through
 * the atlas's shrink stages from SNOW_SHRINK_FROM, and the cell then rests a
 * while before a new one -- its own sprite, size, place and mirroring -- lands
 * there.  Big flakes and small ones are different sprites.  Read texel by texel
 * at a whole number of screen pixels each, so they stay pixel art.  How many
 * cells have one is how much snow lies on the glass (the sky texel's g, let go
 * of slowly under a roof) by the weather's share (u_snow.x) and the speed's
 * (u_snow.z), both eased on the CPU; a flake whose cell drops out of that share
 * fades rather than blinking.  None bends the picture as the drops do. */
"vec3 snowOnScreen(vec2 uv, vec3 color)"
"{"
"    if (u_snow.x <= 0.0) return color;"
"    float onGlass = texture(u_skyOpen, vec2(0.5)).g;"   /* how much of it is still on the glass, let go of slowly under a roof */
"    float amount = u_snow.x * onGlass;"
"    if (amount <= 0.002) return color;"
"    vec2 px = vec2(uv.x, 1.0 - uv.y) * u_size;"   /* px.y grows up the view */
"    float cell = clamp(u_size.y / 9.0, 56.0, 120.0);"
"    float unit = u_size.y / SNOW_BASE_HEIGHT;"
"    float column = floor(px.x / cell);"
"    float seen = min(mix(0.07, 0.30, amount) * u_snow.z, 0.95);"
"    float t = u_snow.y;"
"    vec3 result = color;"
"    for (int i = -1; i <= 1; ++i) {"
"        float x = column + float(i);"
"        float fall = -t * cell / 6.0 * mix(0.8, 1.2, lensHash(vec2(x, 87.3)));"
"        float row = floor((px.y - fall) / cell);"
"        for (int j = -1; j <= 1; ++j) {"
"            vec2 home = vec2(x, row + float(j));"
"            float period = SNOW_LIFE + mix(SNOW_REST_LEAST, SNOW_REST_MOST, lensHash(home + 3.1));"
"            float clock = t + lensHash(home + 5.3) * period;"
"            float life = floor(clock / period);"
"            float age = clock - life * period;"
"            if (age >= SNOW_LIFE) continue;"
"            vec2 id = home + vec2(life * 0.137, life * 0.311);"   /* each life its own flake */
"            float seed = lensHash(id + vec2(23.7, 71.9));"
"            if (seed > seen) continue;"
"            float sway = sin(t * 0.7 + seed * 6.2832) * cell * 0.035;"
"            vec2 center = (home + vec2(mix(0.30, 0.70, lensHash(id + 4.7)),"
"                                        mix(0.30, 0.70, lensHash(id + 9.7)))) * cell"
"                          + vec2(sway, fall);"
"            bool big = lensHash(id + 18.1) < SNOW_BIG_SHARE;"
"            float size = lensHash(id + 61.7);"
"            float scale = max(1.0, floor(unit * (big ? mix(1.5, 2.0, size) : mix(1.0, 1.5, size)) + 0.5));"
"            vec2 texel = floor((px - (floor(center) - SNOW_TILE * 0.5 * scale)) / scale);"
"            if (any(lessThan(texel, vec2(0.0))) || any(greaterThanEqual(texel, vec2(SNOW_TILE)))) continue;"
"            if (lensHash(id + 27.3) < 0.5) texel.x = SNOW_TILE - 1.0 - texel.x;"   /* mirrored or not; never turned, which would break the pixels */
"            float sprite = floor(lensHash(id + 41.9) * (big ? SNOW_BIG : SNOW_SMALL)) + (big ? 0.0 : SNOW_BIG);"
"            float stage = min(floor(clamp((age - SNOW_SHRINK_FROM) / (SNOW_LIFE - SNOW_SHRINK_FROM), 0.0, 1.0)"
"                                    * SNOW_STAGES), SNOW_STAGES - 1.0);"
"            vec4 flake = texelFetch(u_snowSprites, ivec2(int(sprite * SNOW_TILE + texel.x),"
"                                    int(stage * SNOW_TILE + SNOW_TILE - 1.0 - texel.y)), 0);"
"            if (flake.a <= 0.0) continue;"
"            float edgeFade = smoothstep(0.0, cell * 0.5, center.y)"
"                           * (1.0 - smoothstep(u_size.y - cell * 0.5, u_size.y, center.y));"
"            float landing = clamp(age / SNOW_LANDING, 0.0, 1.0) * clamp((SNOW_LIFE - age) / SNOW_GOING, 0.0, 1.0);"
"            float share = clamp((seen - seed) / SNOW_SHARE_FADE, 0.0, 1.0);"
"            float weight = edgeFade * landing * share * mix(0.6, 0.9, lensHash(id + 53.1));"
"            result = result * (1.0 - flake.a * weight) + flake.rgb * weight;"
"        }"
"    }"
"    return result;"
"}"
/* Auto-exposure (above): the eye's brightness against the usual, in stops. */
"float exposureGain()"
"{"
"    vec4 a = texture(u_adapt, vec2(0.5));"
"    float stops = ((a.b + a.a / 255.0) - (a.r + a.g / 255.0)) * u_stops;"
"    float d = sign(stops) * max(abs(stops) - u_exposure.y, 0.0) * u_exposure.x;"
"    return exp2(clamp(d, -u_exposure.w, u_exposure.z));"
"}"
/* The shadows of what stands by the road (initPost, NFS_OBJECT_SHADOWS): from
 * each pixel's place in the view -- the depth buffer holds 1 - 1/z (as the
 * game's vertices give THRASH their depth), so z comes back as 1 / (1 - d);
 * the screen through the projection --
 * a ray towards the sun, its steps growing, each brought back to the screen;
 * where the picture there is nearer than the ray by more than a hair and less
 * than a thing's thickness, something stands between this pixel and the sun.
 * Soft: the nearer the blocking step, the darker, and faded towards the edges
 * of the screen, where what casts may lie beyond it. */
"float viewZ(vec2 uv) { return 1.0 / max(1.0 - texture(u_depth, uv).r, 1e-5); }"
"float sunShadow(vec2 uv)"
"{"
"    float d = texture(u_depth, uv).r;"
"    if (d >= 0.9999 || texture(u_source, uv).a < 0.5) return 0.0;"
"    float z = 1.0 / max(1.0 - d, 1e-5);"
"    if (z < 1.0) return 0.0;"
"    vec3 p = vec3((uv * u_size - u_centre) * z / u_scale, z);"
/* Cast onto what lies level -- the road, the verges, the ground -- not onto
 * what stands: a fence's own rails, a wall, shaded by themselves, only went
 * grey.  Level from the depth of the pixels beside it. */
"    vec2 px = 1.0 / u_size;"
"    float zx = viewZ(uv + vec2(px.x, 0.0)), zy = viewZ(uv + vec2(0.0, px.y));"
"    vec3 ax = vec3(((uv + vec2(px.x, 0.0)) * u_size - u_centre) * zx / u_scale, zx) - p;"
"    vec3 ay = vec3(((uv + vec2(0.0, px.y)) * u_size - u_centre) * zy / u_scale, zy) - p;"
"    vec3 n = cross(ax, ay);"
"    if (dot(n, n) <= 0.0) return 0.0;"
"    float level = smoothstep(0.55, 0.8, abs(dot(normalize(n), u_up)));"
"    if (level <= 0.0) return 0.0;"
"    float shade = 0.0;"
/* Each pixel's steps shifted by its own fraction (interleaved gradient
 * noise), so the places a ray happens to sample do not line up across the
 * screen into bands; and a ray that passes from in front of the picture to
 * behind it has gone through what is there, however thick -- a solid wall --
 * where only the thin was caught before, in stripes. */
"    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));"
"    float before = 1.0;"
"    for (int i = 1; i <= 18; ++i) {"
"        float s = float(i) - jitter;"
"        float t = 0.05 * s * s;"
"        vec3 q = p + u_sun * t;"
"        if (q.z < 0.5) break;"
"        vec2 at = (q.xy * u_scale / q.z + u_centre) / u_size;"
"        if (at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0) break;"
"        if (texture(u_source, at).a < 0.5) { before = 1.0; continue; }"   // a car: casts its own
"        float there = viewZ(at);"
"        float gap = q.z - there;"
"        bool through = before < -0.02 && gap > 0.0 && gap < 4.0 + 0.5 * t;"
"        if ((gap > 0.05 + 0.02 * t && gap < 0.6 + 0.1 * t) || through) {"
"            shade = 1.0 - s / 22.0;"
"            break;"
"        }"
"        before = gap;"
"    }"
"    vec2 edge = min(uv, 1.0 - uv);"
"    return shade * level * smoothstep(0.0, 0.06, min(edge.x, edge.y));"
"}"
/* Speed you can feel, 2 (initPost, NFS_MOTION_BLUR): the world smeared along
 * the way it streams past.  Going forward at v, a point still in the world at
 * z in the view moves on the screen away from the screen's centre by its
 * distance from it times v dt / z -- the near verges and walls streak, the
 * horizon stands -- so each pixel is averaged with eight more along that line
 * behind it, u_blur being v times the shutter.  The cars are left sharp: their
 * own pixels (0 in the picture's alpha) are not blurred, and are not taken
 * into the blur of what lies around them.  NFS_MOTION_BLUR 2 by default. */
"vec3 motionBlur(vec2 uv, vec3 c)"
"{"
"    if (texture(u_source, uv).a < 0.5) return c;"
"    float d = texture(u_depth, uv).r;"
"    if (d >= 0.9999) return c;"
"    float z = 1.0 / max(1.0 - d, 1e-5);"
"    vec2 away = uv * u_size - u_centre;"
"    vec2 trail = away * min(u_blur / max(z, 1.0), 0.25);"
"    float len = length(trail);"
"    if (len < 1.0) return c;"
"    trail *= min(len, 48.0) / len;"
"    vec3 sum = c;"
"    float n = 1.0;"
"    for (int i = 1; i <= 8; ++i) {"
"        vec2 at = uv - trail * (float(i) / 8.0) / u_size;"
"        vec4 s = texture(u_source, at);"
"        if (s.a < 0.5) continue;"
"        float w = 1.0 - float(i) / 10.0;"
"        sum += s.rgb * w;"
"        n += w;"
"    }"
"    return sum / n;"
"}"
"vec3 fxaa(vec2 uv)"
"{"
"    vec3 nw = texture(u_source, uv + vec2(-1.0, -1.0) * u_pixel).rgb;"
"    vec3 ne = texture(u_source, uv + vec2( 1.0, -1.0) * u_pixel).rgb;"
"    vec3 sw = texture(u_source, uv + vec2(-1.0,  1.0) * u_pixel).rgb;"
"    vec3 se = texture(u_source, uv + vec2( 1.0,  1.0) * u_pixel).rgb;"
"    vec3 m  = texture(u_source, uv).rgb;"
"    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);"
"    float lo = min(lm, min(min(lnw, lne), min(lsw, lse)));"
"    float hi = max(lm, max(max(lnw, lne), max(lsw, lse)));"
"    if (hi - lo < max(0.0312, hi * 0.125)) return m;"
"    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));"
"    float reduce = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);"
"    float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);"
"    dir = clamp(dir * scale, vec2(-8.0), vec2(8.0)) * u_pixel;"
"    vec3 a = 0.5 * (texture(u_source, uv + dir * (1.0 / 3.0 - 0.5)).rgb"
"                  + texture(u_source, uv + dir * (2.0 / 3.0 - 0.5)).rgb);"
"    vec3 b = a * 0.5 + 0.25 * (texture(u_source, uv - dir * 0.5).rgb + texture(u_source, uv + dir * 0.5).rgb);"
"    float lb = luma(b);"
"    return (lb < lo || lb > hi) ? a : b;"
"}"
/* The sun's glare (initPost, NFS_SUN_GLARE): by day and with no weather, the
 * sun where the light the cars are lit by comes from (u_glareAt, in the
 * picture's pixels; u_glare already less the further it is off the screen).
 * How much of it is seen: of sixteen points round it, those where the depth
 * buffer is the sky's or as far (over 250 m: the sky is drawn with a depth of
 * its own) -- behind trees, hills and buildings it dims, and
 * flickers through gaps.  Then a small hot core, a warm haze round it, a few
 * faint ghosts of the lens along the line from it through the middle of the
 * picture, and the blacks raised a little. */
"float glareSeen()"
"{"
"    float seen = 0.0, n = 0.0;"
"    for (int i = 0; i < 16; ++i) {"
"        float a = float(i) * 2.39996;"
"        float r = (0.2 + 0.8 * fract(float(i) * 0.618)) * 0.025 * u_size.y;"
"        vec2 p = clamp(u_glareAt + vec2(cos(a), sin(a)) * r, vec2(1.0), u_size - 1.0);"
"        seen += step(0.996, texture(u_depth, p / u_size).r);"
"        n += 1.0;"
"    }"
"    return seen / n;"
"}"
"vec3 glare(vec3 c)"
"{"
"    float g = u_glare * glareSeen();"
"    if (g <= 0.0) return c;"
"    vec2 px = v_uv * u_size;"
"    vec2 d = (px - u_glareAt) / u_size.y;"
"    float r = length(d);"
"    vec3 warm = vec3(1.0, 0.86, 0.64);"
"    float haze = 1.0 * exp(-r * r / 0.0012) + 0.35 * exp(-r / 0.09) + 0.12 * exp(-r / 0.4);"
"    vec3 add = warm * haze;"
"    vec2 mid = u_size * 0.5;"
"    const float along[4] = float[4](-0.35, -0.7, -1.1, 0.5);"
"    const float size[4] = float[4](0.022, 0.06, 0.035, 0.014);"
"    const vec3 hue[4] = vec3[4](vec3(0.55, 0.75, 1.0), vec3(0.6, 1.0, 0.75), vec3(1.0, 0.7, 0.45), vec3(0.85, 0.8, 1.0));"
"    for (int k = 0; k < 4; ++k) {"
"        vec2 at = mid + (u_glareAt - mid) * along[k];"
"        float rad = size[k] * u_size.y;"
"        float e = length(px - at) / rad;"
"        float disc = (1.0 - smoothstep(0.75, 1.0, e)) * (0.6 + 0.4 * smoothstep(0.3, 0.95, e));"
"        add += hue[k] * disc * 0.08;"
"    }"
"    c += add * g;"
"    return c + (1.0 - c) * 0.05 * g;"
"}"
"void main()"
"{"
"    float drop = 0.0;"
"    vec2 seen = u_lens.x > 0.0 ? lensDrops(v_uv, drop) : v_uv;"
"    vec3 c = u_aa ? fxaa(v_uv) : texture(u_source, v_uv).rgb;"
"    if (drop > 0.0)"
"        c = mix(c, texture(u_source, seen).rgb * 1.06, drop);"
"    if (u_blur > 0.0)"
"        c = motionBlur(v_uv, c);"
"    if (u_shadow > 0.0)"
"        c *= 1.0 - u_shadow * sunShadow(v_uv);"
"    if (u_glare > 0.0)"
"        c = glare(c);"
"    if (u_exposure.x > 0.0)"
"        c *= exposureGain();"
/* The lamps (initPost): a soft halo round each, from the light layer's
 * smaller levels -- wide and faint over narrow -- and, at speed, their
 * trails: what the lamps nearer the middle of the view than this pixel, on the
 * same line from it, lay down behind them -- the world's light left on the
 * road behind a car going as fast as the camera, fading with the distance --
 * of it what is not a lamp itself. */
"    if (u_halo > 0.0) {"
"        vec3 halo = textureLod(u_light, v_uv, 2.0).rgb * 0.45 + textureLod(u_light, v_uv, 3.5).rgb * 0.35"
"                  + textureLod(u_light, v_uv, 5.0).rgb * 0.30;"
"        c += halo * u_halo;"
"    }"
"    if (u_trailOn > 0.0) {"
"        vec2 away = v_uv * u_size - u_centre;"
"        vec3 trail = vec3(0.0);"
"        for (int i = 1; i <= 16; ++i) {"
"            float t = float(i) / 16.0;"
"            vec2 at = (u_centre + away / (1.0 + u_trailOn * t)) / u_size;"
"            trail = max(trail, textureLod(u_light, at, 1.5).rgb * (1.0 - t));"
"        }"
"        c += max(trail * 1.6 - textureLod(u_light, v_uv, 0.0).rgb, 0.0);"
"    }"
"    if (u_bloom > 0.0) {"
"        vec3 glow = textureLod(u_bright, v_uv, 1.0).rgb * 0.30 + textureLod(u_bright, v_uv, 2.0).rgb * 0.25"
"                  + textureLod(u_bright, v_uv, 3.0).rgb * 0.20 + textureLod(u_bright, v_uv, 4.0).rgb * 0.15"
"                  + textureLod(u_bright, v_uv, 5.0).rgb * 0.10;"
"        c += glow * u_bloom;"
"    }"
"    if (u_grade.z > 0.0) {"
"        c = (c - 0.5) * u_grade.y + 0.5;"
"        c = mix(vec3(luma(c)), c, u_grade.x) * u_tint;"
"    }"
"    c = snowOnScreen(v_uv, c);"
"    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);"
"}";

/* The port's own particles (drawParticles; native_particles.cpp makes them):
 * dust and grit thrown up from the wheels and sparks, each a quad, its shape
 * worked out here from its seed rather than a picture.  The quad's corners come
 * from the game thread in the picture's pixels; v_uv is -1 to 1 across it. */
static const char g_particleVertexShader[] =
"layout (location=0) in vec2 a_position;"
"layout (location=1) in vec2 a_uv;"
"layout (location=2) in vec4 a_color;"
"layout (location=3) in vec2 a_from;"
"layout (location=4) in vec4 a_params;"   /* depth, seed, type, turn */
"out vec2 v_uv;"
"out vec4 v_color;"
"out vec2 v_from;"
"flat out vec4 v_params;"
"uniform vec2 u_size;"
"void main()"
"{"
"    v_uv = a_uv;"
"    v_color = a_color;"
"    v_from = a_from;"
"    v_params = a_params;"
"    gl_Position = vec4(a_position / u_size * 2.0 - 1.0, 0.0, 1.0);"
"}";

/* The shapes a particle's quad can be (EffectParticle::Type), for the shader
 * below to tell apart by name: its numbers are the enum's, read here rather
 * than written out again. */
static std::string particleTypeNames()
{
    const std::pair<const char*, EffectParticle::Type> types[] = {
        { "kGrain", EffectParticle::Grain },     { "kPuff", EffectParticle::Puff },
        { "kSpark", EffectParticle::Spark },     { "kBlade", EffectParticle::Blade },
        { "kSpike", EffectParticle::Spike },     { "kStreak", EffectParticle::Streak },
        { "kFlame", EffectParticle::Flame },     { "kGlow", EffectParticle::Glow },
        { "kSmoke", EffectParticle::Smoke },     { "kDroplet", EffectParticle::Droplet },
        { "kFlake", EffectParticle::Flake },     { "kBrakeGlow", EffectParticle::BrakeGlow },
        { "kFrost", EffectParticle::Frost },
    };
    std::string names;
    for (const auto& type : types)
        names += std::string("const int ") + type.first + " = " + std::to_string(int(type.second)) + ";\n";
    return names;
}

/* A grain: a clump with a ragged edge -- its outline a few waves round it,
 * turned by its seed -- lit a little from above.  A puff: a cloud of fine
 * dust, soft and uneven, faded where it meets what is behind it so it never
 * shows a hard line against the ground.  Both are the colour of the ground they
 * were thrown from (round v_from, the picture there -- the world's pixels
 * only, a car's being 0 in the alpha, and the car that threw them standing
 * over the very spot; or every pixel there, where too few are the world's),
 * a grain darker and a puff paler.  A spark: a streak from where it was to
 * where it is, white hot at its head.  Each is hidden behind what stands in
 * front of it, from the depth buffer (1 - 1/z). */
static const char g_particleShader[] =
"in vec2 v_uv;"
"in vec4 v_color;"
"in vec2 v_from;"
"flat in vec4 v_params;"
"layout (location=0) out vec4 o_color;"
"uniform sampler2D u_source;"
"uniform sampler2D u_depth;"
"uniform vec2 u_size;"
"uniform sampler2D u_open;"   /* how much sky is overhead (g_skyOpenShader), 0 to 1 */
"uniform float u_fragScale;"  /* the picture's pixels to one of the target's: 2 in the half-size layer */
"uniform vec2 u_openFade;"    /* from, to: none of the weather below the first, all of it from the second */
"float overhead() { return smoothstep(u_openFade.x, u_openFade.y, texture(u_open, vec2(0.5)).r); }"
/* Value noise, its lattice's values a table (u_noise, 256 by 256, made once:
 * makeNoise) rather than worked out with sin() at each of four corners: the
 * smoothstep put in the coordinates, so the texture's own bilinear filter
 * gives the same smooth noise with one read (2026-11-18: three of these a
 * pixel of every puff, and the rain's spray piles puffs up). */
"uniform sampler2D u_noise;"
"float noise(vec2 p)"
"{"
"    vec2 i = floor(p), f = fract(p);"
"    vec2 u = f * f * (3.0 - 2.0 * f);"
"    return texture(u_noise, (i + u + 0.5) / 256.0).r;"
"}"
/* How much of a quad is inside its circle: 1 well inside, 0 at its rim and
 * beyond (as the world shader's). */
"float inside(vec2 uv)"
"{"
"    return 1.0 - smoothstep(0.75, 1.0, length(uv));"
"}"
"vec3 ground(vec3 otherwise)"
"{"
"    if (v_from.x < 0.0) return otherwise;"
"    vec3 sum = vec3(0.0), all = vec3(0.0);"
"    float n = 0.0;"
"    for (int i = 0; i < 12; ++i) {"
"        float a = float(i) * 2.39996;"
"        float d = (i < 6 ? 0.05 : 0.1) * u_size.y;"
"        vec4 s = texture(u_source, (v_from + vec2(cos(a), sin(a)) * d) / u_size);"
"        float w = step(0.5, s.a);"
"        sum += s.rgb * w;"
"        all += s.rgb;"
"        n += w;"
"    }"
/* The world's pixels where there are enough of them; else every one (a
 * ground of 32-bit textures is 0 in the alpha too, like a car). */
"    return n >= 3.0 ? sum / n : all / 12.0;"
"}"
"void main()"
"{"
"    float z = v_params.x, seed = v_params.y;"
"    int type = int(v_params.z + 0.5);"
/* The round ones -- grains, puffs, flames, glows, smoke, flakes, brake glows --
 * have nothing in their quad's corners: gone before anything is read. */
"    float r = length(v_uv);"
"    if (r >= 1.0 && (type <= kPuff || (type >= kFlame && type != kDroplet))) discard;"
"    float behind = texture(u_depth, gl_FragCoord.xy * u_fragScale / u_size).r;"
"    float zBehind = 1.0 / max(1.0 - behind, 1e-5);"
"    float room = zBehind - z;"
"    if (room < (type == kSpike ? -0.4 : type == kBrakeGlow ? -0.45 : -0.05)) discard;"
"    if (type == kStreak) {"   /* a piece of an air trail: thin, even, soft at its sides */
"        o_color = vec4(v_color.rgb, exp(-v_uv.y * v_uv.y * 4.0) * v_color.a);"
"        return;"
"    }"
/* A raindrop: a short dash, soft at its sides and at both ends, so it has
 * no corner and no square to it however large it comes to be. */
"    if (type == kDroplet) {"
"        float a = exp(-v_uv.y * v_uv.y * 4.0) * (1.0 - smoothstep(0.45, 1.0, abs(v_uv.x)));"
"        o_color = vec4(v_color.rgb, a * v_color.a * overhead());"
"        return;"
"    }"
/* An exhaust flame: a ball of burning gas, ragged and flickering by its
 * seed, the car's flame colour at its edge and much lighter -- near white -- in
 * its core (v_color.rgb is the car's colour, v_params.y its own flicker).  It is
 * added to what is behind it, so the core reads as light rather than paint. */
"    if (type == kFlame) {"
"        vec2 q = v_uv * 1.8 + vec2(seed * 13.7, seed * 27.1);"
"        float n = noise(q) * 0.55 + noise(q * 2.3) * 0.3 + noise(q * 4.7) * 0.15;"
"        float rr = length(v_uv) * (0.72 + 0.5 * n);"
"        float a = (1.0 - smoothstep(0.35, 1.0, rr)) * inside(v_uv);"
"        if (a <= 0.002) discard;"
"        a *= v_color.a;"
"        float core = 1.0 - smoothstep(0.0, 0.85, rr);"
"        vec3 c = mix(v_color.rgb, vec3(1.0, 0.96, 0.86), core * core * 0.85 + 0.1);"
"        o_color = vec4(c * (0.9 + 0.6 * core), a * (0.8 + 0.2 * core));"
"        return;"
"    }"
/* The light a flame throws: a soft pool of it on the road under the pipe,
 * its colour the flame's, squared away to nothing at its edge, squashed
 * lengthwise -- a disc on the ground seen from behind.  Additive, like the
 * flame it comes from. */
"    if (type == kGlow) {"
"        float rr = length(vec2(v_uv.x, v_uv.y * 2.6));"
"        float a = max(1.0 - rr, 0.0);"
"        a *= a;"
"        a *= v_color.a;"
"        if (a <= 0.002) discard;"
"        o_color = vec4(v_color.rgb * (0.75 + 0.5 * a), a);"
"        return;"
"    }"
/* A hot brake disc: a soft glow round the hub, its ring a little hotter
 * than its middle, as a disc glows through the spokes; narrowed sideways as the
 * disc turns edge on (v_from.x, 0 to 1, how round it is seen).  Added, like a
 * flame's light.  It may lie up to 45 cm behind what is drawn in front of it --
 * the tyre's tread, from behind the car -- and fades the deeper it lies, so a
 * glow behind the tyre is a faint warmth round the wheel, not cut off. */
"    if (type == kBrakeGlow) {"
"        vec2 q = vec2(v_uv.x / max(v_from.x, 0.2), v_uv.y);"
"        float rr = length(q);"
"        if (rr >= 1.0) discard;"
"        float soft = (1.0 - rr) * (1.0 - rr);"
"        float ring = exp(-(rr - 0.55) * (rr - 0.55) / 0.04);"
"        float a = (soft * 0.7 + ring * 0.3) * v_color.a * clamp(room / 0.45 + 1.0, 0.0, 1.0);"
"        if (a <= 0.002) discard;"
"        o_color = vec4(v_color.rgb * (0.85 + 0.4 * soft), a);"
"        return;"
"    }"
/* A snowflake: a small soft speck, ragged by its seed -- soft at the ends
 * of its quad along x as well as at its sides, so that the streak its quad is
 * stretched into at speed (drawParticles) is a flake's dash and not a turned
 * box; standing still the quad is square and the flake the speck it was.
 * Blended over the picture, not added to it. */
"    if (type == kFlake) {"
/* Never under 1, so both axes are at their rim where the quad is: nothing of
 * the flake reaches its edge, however its noise came out. */
"        float ragged = 1.0 + 0.4 * noise(v_uv * 3.0 + vec2(seed * 19.0, seed * 7.0));"
"        float along = 1.0 - smoothstep(0.3, 1.0, abs(v_uv.x) * ragged);"
"        float across = 1.0 - smoothstep(0.3, 1.0, abs(v_uv.y) * ragged);"
"        float a = along * across;"
"        if (a <= 0.002) discard;"
"        o_color = vec4(v_color.rgb, a * v_color.a * clamp(room / 0.1 + 0.5, 0.0, 1.0) * overhead());"
"        return;"
"    }"
/* Tyre smoke: a dark cloud off a wheel spinning or locked, ragged by its
 * own noise, its top catching the sky and its heart almost black, neither of
 * them the ground's colour.  Blended over the picture, not added to it. */
"    if (type == kSmoke) {"
"        vec2 q = v_uv * 1.7 + vec2(seed * 21.3, seed * 37.7);"
"        float n = noise(q) * 0.55 + noise(q * 2.2) * 0.3 + noise(q * 4.6) * 0.15;"
"        float rr = length(v_uv) * (0.72 + 0.45 * n);"
"        float a = (1.0 - smoothstep(0.25, 1.0, rr)) * inside(v_uv);"
"        if (a <= 0.002) discard;"
"        a *= a * v_color.a;"
"        float up = clamp(v_uv.y * 0.6 + 0.5, 0.0, 1.0);"
"        vec3 c = v_color.rgb * (0.7 + 0.7 * up) + vec3(0.045) * up;"
"        o_color = vec4(c, a * clamp(room / 0.3 + 0.25, 0.0, 1.0));"
"        return;"
"    }"
/* A chip of ice off a tyre on snow: a little faceted solid, three to six flat
 * sides, each its own shade and its own reach, brightest along the edges where
 * it catches the light, tumbling by its seed (v_params.w is its turn).  White,
 * cyan or light grey from the particle's colour; blended over the picture. */
"    if (type == kFrost) {"
"        float ang = atan(v_uv.y, v_uv.x) + v_params.w;"
"        int facets = 3 + int(seed * 3.99);"
"        float sector = 6.283185 / float(facets);"
"        float side = mod(ang, sector) - sector * 0.5;"
"        float facet = floor(ang / sector);"
"        float circum = 0.55 + 0.30 * fract(sin(seed * 97.0 + facet * 13.7) * 43758.55);"
"        float radius = circum * cos(sector * 0.5) / max(cos(side), 1e-3);"
"        float r = length(v_uv);"
"        if (r > radius) discard;"
"        float rim = smoothstep(0.72, 1.0, r / radius);"
"        float shade = 0.72 + 0.28 * fract(sin(seed * 41.0 + facet * 7.3) * 24634.6);"
"        vec3 c = mix(v_color.rgb, vec3(1.0), 0.2 + 0.5 * rim) * (0.85 + 0.3 * shade);"
"        o_color = vec4(c, v_color.a * (0.55 + 0.45 * rim));"
"        return;"
"    }"
"    if (type == kSpark) {"
"        float across = exp(-v_uv.y * v_uv.y * 5.0);"
"        float along = clamp(v_uv.x * 0.5 + 0.5, 0.0, 1.0);"
"        float a = across * mix(0.15, 1.0, along * along) * v_color.a;"
"        vec3 hot = mix(v_color.rgb, vec3(1.0, 0.97, 0.85), across * along * along * 0.6);"
"        o_color = vec4(hot, a);"
"        return;"
"    }"
"    if (type == kSpike) {"
/* A spike of a spike strip: a steel point (up the screen is -y) on a dark
 * rubber belt, lit from above. */
"        float up = -v_uv.y;"
"        float belt = 1.0 - smoothstep(0.0, 0.1, abs(up + 0.8) - 0.12);"
"        float halfWidth = (0.7 - up) * 0.32;"
"        float spike = step(up, 0.7) * step(-0.75, up) * (1.0 - smoothstep(halfWidth * 0.75, halfWidth, abs(v_uv.x)));"
"        float a = max(belt, spike);"
"        if (a <= 0.0) discard;"
"        vec3 steel = vec3(0.62, 0.64, 0.68) * (0.75 + 0.45 * (1.0 - abs(v_uv.x) / max(halfWidth, 1e-3)) * (0.6 + 0.4 * up));"
"        vec3 c = spike > belt ? steel : vec3(0.06, 0.06, 0.07);"
"        o_color = vec4(c * v_color.rgb, a * v_color.a);"
"        return;"
"    }"
"    float angle = atan(v_uv.y, v_uv.x) + seed * 6.2832;"
"    if (type == kGrain || type == kBlade) {"
"        vec3 g = ground(v_color.rgb);"
/* What comes off grass is blades: long and thin, tapering, a little bent,
 * lying as they tumble (v_params.w), a greener and brighter shade of the
 * ground -- where the ground is green; off snow or earth, grains. */
"        if (type == kBlade && g.g > g.r * 1.02 && g.g > g.b * 0.9) {"
"            vec2 d = vec2(cos(v_params.w), sin(v_params.w));"
"            vec2 q = vec2(dot(v_uv, d), dot(v_uv, vec2(-d.y, d.x)));"
"            q.y -= (seed - 0.5) * 0.5 * q.x * q.x;"
"            float w = 0.27 * (1.0 - 0.7 * q.x * q.x);"
"            float a = (1.0 - smoothstep(0.8, 1.0, abs(q.x))) * (1.0 - smoothstep(w * 0.5, w, abs(q.y)));"
"            if (a <= 0.0) discard;"
"            vec3 hue = g / max(max(g.r, g.g), max(g.b, 1e-3));"
"            vec3 c = mix(hue, hue * vec3(0.8, 1.0, 0.55), 0.5) * max(g.g, 0.3) * 1.35"
"                   * (0.8 + 0.4 * fract(seed * 5.71)) * (0.9 + 0.2 * q.y / max(w, 1e-3));"
"            o_color = vec4(c, a * v_color.a * clamp(room / 0.1 + 0.5, 0.0, 1.0));"
"            return;"
"        }"
"        vec2 h = v_uv * 2.0;"
"        r = length(h);"
"        angle = atan(h.y, h.x) + seed * 6.2832;"
"        float edge = 0.72 + 0.1 * sin(angle * 2.0 + seed * 11.0) + 0.06 * sin(angle * 3.0 + seed * 23.0)"
"                   + 0.04 * noise(vec2(angle * 1.5, seed * 40.0));"
"        float a = 1.0 - smoothstep(edge - 0.3, edge, r);"
"        if (a <= 0.0) discard;"
"        float lit = 0.92 + 0.16 * clamp(h.y * 0.7 - r * 0.2, -1.0, 1.0);"
"        vec3 c = g * (0.8 + 0.22 * fract(seed * 7.13)) * lit;"
"        o_color = vec4(c, a * v_color.a * clamp(room / 0.1 + 0.5, 0.0, 1.0));"
"        return;"
"    }"
"    vec2 q = v_uv * 1.6 + vec2(seed * 17.0, seed * 31.0);"
"    float n = noise(q) * 0.55 + noise(q * 2.1) * 0.3 + noise(q * 4.3) * 0.15;"
/* Ragged by its noise inside the circle, nothing at its edge: the noise alone
 * left some of the quad's edge showing, a square round a near puff. */
"    float a = (1.0 - smoothstep(0.15, 1.0, r + (n - 0.5) * 0.7)) * (0.55 + 0.45 * n) * (1.0 - smoothstep(0.6, 1.0, r));"
"    if (a <= 0.0) discard;"
"    vec3 g = ground(v_color.rgb);"
"    float l = dot(g, vec3(0.299, 0.587, 0.114));"
"    vec3 c = mix(g, vec3(l), 0.35) * 1.12 + 0.04;"
"    o_color = vec4(c, a * v_color.a * clamp(room / 0.3 + 0.2, 0.0, 1.0));"
"}";

static GLuint postProgram(GLuint (*compile)(GLenum, const std::string&, const char*), const char* fragmentBody,
                          const std::string& defines = std::string())
{
    const GLuint vertex = compile(GL_VERTEX_SHADER, "", g_postVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, defines, fragmentBody);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status)
    {
        GLchar log[2048];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof log, &length, log);
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] whole-picture pass: %s", log);
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

/* The alpha under which a blended batch's fragment writes no depth (u_depthCut). */
constexpr float kDepthCutCar = 0.08f;
constexpr float kDepthCutWorld = 0.5f;

/* A car's shadow (soften, SoftenedPicture::Shadow): a superellipse of this
 * power -- the cube root below matches it -- dark to this share of the way out,
 * this dark at its heart. */
constexpr float kShadowRound = 3.0f;
constexpr float kShadowSolid = 0.6f;
constexpr float kShadowDarkness = 0.9f;

/* NFS_CAR_SHADOW: 0 or off leaves the cars' shadows off the road (a car's
 * shadow is a decal the port has recognised, batch.decal). */
static bool carShadowSetting()
{
    static const bool on = [] {
        const char* value = SDL_getenv("NFS_CAR_SHADOW");
        return !(value && (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0));
    }();
    return on;
}

static float lightGlowSetting();
static float lightTrailSetting();
static float smoothStep(float edge0, float edge1, float x);

/* NFS_PERF_TRACE: where the GL thread's time goes, a race frame at a time --
 * the world (to the whole-picture pass), the particles, the rest of the pass --
 * each ended by a glFinish, so it is the GPU's time too.  Only when asked: the
 * glFinishes stall the pipeline. */
static bool perfTraced()
{
    static const bool on = SDL_getenv("NFS_PERF_TRACE") != nullptr;
    return on;
}
/* NFS_PERF_TRACE=2: each of the costly effects left out in turn, two seconds
 * each, and the frames timed without it -- what each one costs on this GPU,
 * from one race.  The picture changes as it goes; for finding costs only. */
enum PerfLeftOut
{
    kPerfNone, kPerfHaze, kPerfSharpen, kPerfGrain, kPerfWet, kPerfHeadlights, kPerfShadows, kPerfBlur,
    kPerfAa, kPerfBloom, kPerfHalo, kPerfTrails, kPerfLens, kPerfExposure, kPerfCount
};
static const char* const kPerfNames[kPerfCount] = { "nothing", "haze", "bicubic textures", "grain", "wet road",
                                                     "headlights on objects", "object shadows", "motion blur", "FXAA",
                                                     "glow", "lamps' halo", "light trails", "lens rain",
                                                     "auto-exposure" };
static int s_perfLeftOut = kPerfNone;   // this frame's
static bool perfCycling()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_PERF_TRACE");
        return text && SDL_strcmp(text, "2") == 0;
    }();
    return on;
}
static bool without(int effect) { return s_perfLeftOut == effect; }

struct GpuCost
{
    Uint64 frameStart = 0, mark = 0;
    double world = 0.0, particles = 0.0, post = 0.0;
    unsigned frames = 0;
    Uint64 said = 0;
};
static GpuCost s_gpu;

/* NFS_PERF_TRACE: the textures the game uploaded this frame, how big, and the
 * game thread's time in them (setTextureData), for a slow frame's line. */
static unsigned s_uploads = 0;
static double s_uploadKB = 0.0, s_uploadMs = 0.0;

static double gpuLap()
{
    glFinish();
    const Uint64 now = SDL_GetTicksNS();
    const double ms = double(now - s_gpu.mark) * 1e-6;
    s_gpu.mark = now;
    return ms;
}

/* NFS_WET_TRACE: what the frames of a wet race were drawn as.  Asked when
 * first needed, after nfs3hp.ini is in the environment. */
static bool wetTraced()
{
    static const bool on = SDL_getenv("NFS_WET_TRACE") != nullptr || SDL_getenv("NFS_FRAME_DUMP") != nullptr;
    return on;
}

/* The weather under a roof (g_skyOpenShader): the band of the picture looked
 * at for sky, from the top; under what share of sky none falls, and from what
 * share all of it; how fast it follows. */
namespace overhead
{
constexpr float kBandFrom = 0.03f, kBandTo = 0.43f;   // of the picture's height, from the top
constexpr float kNone = 0.03f, kAll = 0.12f;          // share of the band that is sky
constexpr float kSeconds = 0.25f;                     // its ease
constexpr float kRestartAfter = 0.5f;                 // s without a race picture: from what is seen
}  // namespace overhead

/* Rain on the lens (the composite's lensDrops): how fast the lens wets in the
 * rain under an open sky and dries out of it, and how much sky it takes. */
namespace lens
{
constexpr float kAmount = 1.0f;         // NFS_LENS_RAIN: how many drops, against the shader's own
constexpr float kWetSeconds = 1.5f;     // to wet, under an open sky in the rain
constexpr float kDrySeconds = 5.0f;     // to dry, under a roof or out of the rain
constexpr float kSkyToWet = 0.06f;      // the share of sky overhead it takes
}  // namespace lens

/* Snow on the windscreen (the composite's snowOnScreen): how much more of it
 * there is as the view comes on, and how long it takes to go. */
namespace snow
{
constexpr float kMostSpeed = 300.0f / 3.6f;   // m/s: as fast as it comes down hardest, and no more
constexpr float kMost = 2.5f;                 // times the weather's own snow, at that speed
constexpr float kComeSeconds = 0.2f;          // to cover the glass, out in the open in the snow
constexpr float kGoSeconds = 2.5f;            // to leave it, under a roof (a tunnel, a bridge)
constexpr float kEaseSeconds = 1.0f;          // the weather's and the speed's share, eased: no flake blinks on a frame's change
constexpr double kClockWrap = 3600.0;         // seconds: the field's clock starts again after an hour
// Each flake (the composite's snowOnScreen; the atlas is tools/generate_snowflakes.py's):
constexpr float kLife = 6.0f;                 // seconds on the glass
constexpr float kShrinkFrom = 4.0f;           // seconds: melting down through the stages from here
constexpr float kRestLeast = 0.5f;            // seconds a cell rests before the next lands, at least
constexpr float kRestMost = 4.0f;             // and at most
constexpr float kLanding = 0.12f;             // seconds to come on as it lands
constexpr float kGoing = 0.15f;               // and to go at the end
constexpr float kShareFade = 0.03f;           // of the share: a cell dropping out of it fades over this much
constexpr float kBigShare = 0.4f;             // of the flakes, the big ones
constexpr float kBaseHeight = 720.0f;         // a picture this high draws a sprite's pixel at 1 or 2 of its own
// The atlas's layout, as the generator writes it.
constexpr int kTile = 32;                     // pixels a sprite
constexpr int kStages = 6;                    // shrink stages, a row each
constexpr int kBig = 9, kSmall = 9;           // sprites, a column each: the big first
}  // namespace snow

/* The snow's tunables as the composite shader's constants. */
static std::string snowShaderConstants()
{
    const std::pair<const char*, float> values[] = {
        { "SNOW_LIFE", snow::kLife },           { "SNOW_SHRINK_FROM", snow::kShrinkFrom },
        { "SNOW_REST_LEAST", snow::kRestLeast }, { "SNOW_REST_MOST", snow::kRestMost },
        { "SNOW_LANDING", snow::kLanding },     { "SNOW_GOING", snow::kGoing },
        { "SNOW_SHARE_FADE", snow::kShareFade }, { "SNOW_BIG_SHARE", snow::kBigShare },
        { "SNOW_BASE_HEIGHT", snow::kBaseHeight }, { "SNOW_TILE", float(snow::kTile) },
        { "SNOW_STAGES", float(snow::kStages) }, { "SNOW_BIG", float(snow::kBig) },
        { "SNOW_SMALL", float(snow::kSmall) },
    };
    std::string text;
    char line[96];
    for (const auto& value : values)
    {
        SDL_snprintf(line, sizeof line, "const float %s = %.6f;\n", value.first, double(value.second));
        text += line;
    }
    return text;
}

/* Auto-exposure's tunables (g_meterShader, g_adaptShader, the composite). */
namespace exposure
{
constexpr float kStrength = 0.7f;        // NFS_EXPOSURE: of each stop past the dead zone, how much is made up
constexpr float kStops = 10.0f;          // the meter's range: 2^-10 to white
constexpr int kMeterSize = 64;           // texels a side; its levels down to one
constexpr int kMeterLevels = 7;
constexpr float kDeadZone = 0.35f;       // stops either side of the usual left alone
constexpr float kMostUp = 1.0f;          // stops brighter at most
constexpr float kMostDown = 0.5f;        // stops darker at most
constexpr float kEyeToDark = 0.8f;       // seconds: the eye opening in the dark
constexpr float kEyeToLight = 0.3f;      // and closing in the light, faster
constexpr float kUsualToDark = 60.0f;    // seconds: the track's usual brightness, slowly into the dark
constexpr float kUsualToLight = 8.0f;    // and back out of it
constexpr float kRestartAfter = 0.5f;    // seconds without a race's picture: start again from the scene
constexpr float kLongestStep = 0.1f;     // seconds: a frame's step, at most
}  // namespace exposure

static float onOff(const char* name, float otherwise)
{
    const char* value = SDL_getenv(name);
    if (!value || !*value)
        return otherwise;
    if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
        return 0.0f;
    if (SDL_strcasecmp(value, "on") == 0)
        return otherwise;
    const float parsed = float(SDL_atof(value));
    return parsed > 0.0f ? SDL_min(parsed, 2.0f) : otherwise;
}

#if !defined(__ANDROID__)
static unsigned int loadSnowflakeAtlas()
{
    const char* basePath = SDL_GetBasePath();
    if (!basePath)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] snow sprites: cannot find the executable directory: %s",
                     SDL_GetError());
        return 0;
    }
    const std::string path = std::string(basePath) + "assets/snowflakes/snowflake-atlas.png";
    SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
    if (!loaded)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] snow sprites: cannot load %s: %s", path.c_str(), SDL_GetError());
        return 0;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] snow sprites: cannot convert %s: %s", path.c_str(), SDL_GetError());
        return 0;
    }
    constexpr int side = 768;
    if (rgba->w != side || rgba->h != side)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] snow sprites: %s is %dx%d, expected %dx%d", path.c_str(),
                     rgba->w, rgba->h, side, side);
        SDL_DestroySurface(rgba);
        return 0;
    }
    std::vector<x86::reg8> pixels(std::size_t(side) * side * 4);
    for (int y = 0; y < side; ++y)
        std::memcpy(pixels.data() + std::size_t(y) * side * 4,
                    static_cast<const x86::reg8*>(rgba->pixels) + std::size_t(y) * rgba->pitch, side * 4);
    SDL_DestroySurface(rgba);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        for (std::size_t channel = 0; channel < 3; ++channel)
            pixels[i + channel] = x86::reg8((unsigned(pixels[i + channel]) * pixels[i + 3] + 127) / 255);

    unsigned int texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // Pixel art: read texel by texel (snowOnScreen's texelFetch), never filtered.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] snow sprites: loaded %s", path.c_str());
    return texture;
}
#endif

void ThrashRenderer::initPost()
{
    // Each on unless set to 0 (or "off"); a number for how much, where it has one.
    m_haze = SDL_min(onOff("NFS_HAZE", 0.8f), 1.0f);
    m_headStrength = onOff("NFS_HEADLIGHT_OBJECTS", 1.6f);
    m_motionBlur = onOff("NFS_MOTION_BLUR", 2.0f);
    m_lightGlow = lightGlowSetting();
    m_lightTrails = lightTrailSetting();
    m_bloom = onOff("NFS_BLOOM", 0.6f);
    m_gradeOn = onOff("NFS_GRADE", 1.0f) > 0.0f;
    m_antialias = onOff("NFS_AA", 1.0f) > 0.0f;
    m_exposure = SDL_min(onOff("NFS_EXPOSURE", exposure::kStrength), 1.0f);
    m_lensRain = onOff("NFS_LENS_RAIN", lens::kAmount);
#if !defined(__ANDROID__)
    m_snowAtlas = loadSnowflakeAtlas();
#endif
    m_exposureTrace = SDL_getenv("NFS_EXPOSURE_TRACE") != nullptr;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] auto-exposure %.2f", double(m_exposure));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[GFX] haze %.2f; glow %.2f; colour grade %s; anti-aliasing %s; headlights on objects %.2f; motion blur %.2f",
                double(m_haze), double(m_bloom), m_gradeOn ? "on" : "off", m_antialias ? "FXAA" : "off", double(m_headStrength),
                double(m_motionBlur));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] the lamps' glow %.2f, their trails %.2f", double(m_lightGlow),
                double(m_lightTrails));
    {
        m_brightProgram = postProgram(compileShader, g_brightShader);
        {
            // The port's particles (drawParticles): NFS_PARTICLES=0 leaves the game's.
            const char* particles = SDL_getenv("NFS_PARTICLES");
            m_particlesOn = !(particles && (SDL_strcmp(particles, "0") == 0 || SDL_strcasecmp(particles, "off") == 0));
            const GLuint vertex = compileShader(GL_VERTEX_SHADER, "", g_particleVertexShader);
            const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, particleTypeNames(), g_particleShader);
            m_particleProgram = glCreateProgram();
            glAttachShader(m_particleProgram, vertex);
            glAttachShader(m_particleProgram, fragment);
            glLinkProgram(m_particleProgram);
            GLint linked = 0;
            glGetProgramiv(m_particleProgram, GL_LINK_STATUS, &linked);
            if (!linked)
            {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] particles: their shader did not link");
                m_particlesOn = false;
            }
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            glGenVertexArrays(1, &m_particleArray);
            glGenBuffers(1, &m_particleBuffer);
        }
        m_compositeProgram = postProgram(compileShader, g_compositeShader, snowShaderConstants());
        m_brightSource = glGetUniformLocation(m_brightProgram, "u_source");
        m_compositeSource = glGetUniformLocation(m_compositeProgram, "u_source");
        m_compositeBright = glGetUniformLocation(m_compositeProgram, "u_bright");
        m_compositePixel = glGetUniformLocation(m_compositeProgram, "u_pixel");
        m_compositeBloom = glGetUniformLocation(m_compositeProgram, "u_bloom");
        m_compositeGrade = glGetUniformLocation(m_compositeProgram, "u_grade");
        m_compositeTint = glGetUniformLocation(m_compositeProgram, "u_tint");
        m_compositeAa = glGetUniformLocation(m_compositeProgram, "u_aa");
        m_compositeDepth = glGetUniformLocation(m_compositeProgram, "u_depth");
        m_compositeShadow = glGetUniformLocation(m_compositeProgram, "u_shadow");
        m_compositeSun = glGetUniformLocation(m_compositeProgram, "u_sun");
        m_compositeScale = glGetUniformLocation(m_compositeProgram, "u_scale");
        m_compositeCentre = glGetUniformLocation(m_compositeProgram, "u_centre");
        m_compositeUp = glGetUniformLocation(m_compositeProgram, "u_up");
        m_compositeSize = glGetUniformLocation(m_compositeProgram, "u_size");
        m_compositeBlur = glGetUniformLocation(m_compositeProgram, "u_blur");
        m_compositeLight = glGetUniformLocation(m_compositeProgram, "u_light");
        m_compositeHalo = glGetUniformLocation(m_compositeProgram, "u_halo");
        m_compositeTrailOn = glGetUniformLocation(m_compositeProgram, "u_trailOn");
        m_compositeGlare = glGetUniformLocation(m_compositeProgram, "u_glare");
        m_compositeGlareAt = glGetUniformLocation(m_compositeProgram, "u_glareAt");
        m_compositeAdapt = glGetUniformLocation(m_compositeProgram, "u_adapt");
        m_compositeExposure = glGetUniformLocation(m_compositeProgram, "u_exposure");
        m_compositeSnow = glGetUniformLocation(m_compositeProgram, "u_snow");
        m_compositeSnowSprites = glGetUniformLocation(m_compositeProgram, "u_snowSprites");
        glUseProgram(m_compositeProgram);
        glUniform1f(glGetUniformLocation(m_compositeProgram, "u_stops"), exposure::kStops);
        glUniform4f(m_compositeExposure, 0.0f, 0.0f, 0.0f, 0.0f);
        glUseProgram(0);
        m_skyOpenProgram = postProgram(compileShader, g_skyOpenShader);
        m_softLayerProgram = postProgram(compileShader, g_softLayerShader);
        m_softHalf = onOff("NFS_PARTICLES_HALF", 1.0f) > 0.0f;
        if (m_exposure > 0.0f)
        {
            m_meterProgram = postProgram(compileShader, g_meterShader);
            m_adaptProgram = postProgram(compileShader, g_adaptShader);
        }
        glGenFramebuffers(1, &m_lightFramebuffer);
        glGenFramebuffers(1, &m_depthFramebuffer);
        glGenVertexArrays(1, &m_postArray);
        glGenFramebuffers(1, &m_postFramebuffer);
    }
}

static int levelsFor(int width, int height)
{
    int levels = 1;
    for (int size = SDL_max(width, height); size > 1; size >>= 1)
        ++levels;
    return levels;
}

/* The sky as the frame drew it, before the world: into m_sky with its levels,
 * for the haze.  From the framebuffer drawPending has bound. */
void ThrashRenderer::takeSky(int width, int height)
{
    m_skyTaken = true;
    if (!m_sky || m_skyW != width || m_skyH != height)
    {
        if (m_sky)
            glDeleteTextures(1, &m_sky);
        glGenTextures(1, &m_sky);
        glBindTexture(GL_TEXTURE_2D, m_sky);
        glTexStorage2D(GL_TEXTURE_2D, levelsFor(width, height), GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_skyW = width;
        m_skyH = height;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_sky);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    m_skyValid = true;
}

/* The world's picture through FXAA, the glow and the grade, back into the
 * framebuffer, before the HUD.  drawPending sets its own state again after. */
/* The lamps' layer (full size, with levels for the halo and the trails,
 * initPost), made with the post's textures, empty. */
void ThrashRenderer::allocateLightLayer(int width, int height)
{
    if (!m_lightFramebuffer || (m_lightGlow <= 0.0f && m_lightTrails <= 0.0f))
        return;
    if (m_lightLayer)
        glDeleteTextures(1, &m_lightLayer);
    auto make = [&](unsigned int& texture, int levels) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    make(m_lightLayer, levelsFor(width, height));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glBindFramebuffer(GL_FRAMEBUFFER, m_lightFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_lightLayer, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthBuffer);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
}

/* The port's particles over the world (g_particleShader): a quad each, two
 * triangles, the grains and puffs blended over the picture back to front and
 * the sparks added on last, none writing the picture's alpha (what is a car
 * and what is not, for the passes after). */
/* The particles' noise (g_particleShader's u_noise): 256 by 256 values, the
 * same each run, wrapping, filtered. */
static unsigned int makeNoise()
{
    std::vector<unsigned char> values(256 * 256);
    std::uint32_t state = 0x9e3779b9u;
    for (unsigned char& v : values)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        v = (unsigned char)(state >> 24);
    }
    unsigned int texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, 256, 256);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED, GL_UNSIGNED_BYTE, values.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return texture;
}

void ThrashRenderer::drawParticles(const ThrashDraw& draw, int width, int height)
{
    struct Corner
    {
        float x, y, u, v, r, g, b, a, fx, fy, depth, seed, type, turn;
    };
    const std::vector<EffectParticle>& list = *draw.particles;
    /* Three runs, in this order: the soft ones (spray's mist, smoke) blended in
     * the half-size layer; the rest of the blended ones; the added ones. */
    std::vector<Corner> soft, corners, added;
    soft.reserve(list.size() * 6);
    corners.reserve(list.size() * 6);
    added.reserve(list.size() * 6);
    for (const EffectParticle& p : list)
    {
        /* A spark, a streak, a drop and a flake are quads stretched from where
         * they were to where they are -- white hot at its head, a drop and a
         * flake soft at both ends; a flame is a quad like a puff's, but added
         * to what is behind it rather than blended over it -- light, not paint. */
        const bool stretch = p.type == EffectParticle::Spark || p.type == EffectParticle::Streak
                             || p.type == EffectParticle::Droplet || p.type == EffectParticle::Flake;
        const bool additive = (stretch && p.type != EffectParticle::Flake)
                              || p.type == EffectParticle::Flame || p.type == EffectParticle::Glow
                              || p.type == EffectParticle::BrakeGlow;
        float ax, ay, bx, by;   // the quad's two axes, half its length and width
        if (stretch)
        {
            float dx = p.x - p.tx, dy = p.y - p.ty;
            float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1e-3f)
            {
                dx = 1.0f;
                dy = 0.0f;
                length = 1.0f;
            }
            const float half = 0.5f * length + p.size;
            ax = dx / length * half;
            ay = dy / length * half;
            bx = -dy / length * p.size;
            by = dx / length * p.size;
        }
        else
        {
            ax = p.size;
            ay = 0.0f;
            bx = 0.0f;
            /* A smoke cloud's height is its own, over its width (p.turn): the
             * drifting one is taller than it is wide. */
            by = p.size * (p.type == EffectParticle::Smoke ? p.turn : 1.0f);
        }
        const bool cloud = !additive && m_softHalf
                           && (p.type == EffectParticle::Puff || p.type == EffectParticle::Smoke);
        std::vector<Corner>& into = additive ? added : cloud ? soft : corners;
        const float cx = stretch ? 0.5f * (p.x + p.tx) : p.x;
        const float cy = stretch ? 0.5f * (p.y + p.ty) : p.y;
        const float fromX = stretch ? -1.0f : p.tx;
        auto corner = [&](float s, float t) {
            into.push_back({ cx + ax * s + bx * t, cy + ay * s + by * t, s, t, p.r, p.g, p.b, p.a, fromX, p.ty,
                                p.depth, p.seed, p.type, p.turn });
        };
        corner(-1, -1); corner(1, -1); corner(1, 1);
        corner(-1, -1); corner(1, 1); corner(-1, 1);
    }
    const std::size_t softCount = soft.size(), blendedCount = corners.size(), addedCount = added.size();
    soft.insert(soft.end(), corners.begin(), corners.end());
    soft.insert(soft.end(), added.begin(), added.end());
    const std::vector<Corner>& all = soft;
    glBindVertexArray(m_particleArray);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleBuffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(all.size() * sizeof(Corner)), all.data(), GL_STREAM_DRAW);
    const GLsizei stride = sizeof(Corner);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(Corner, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(Corner, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(Corner, r));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(Corner, fx));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(Corner, depth));
    for (GLuint k = 0; k < 5; ++k)
        glEnableVertexAttribArray(k);
    glUseProgram(m_particleProgram);
    glUniform1i(glGetUniformLocation(m_particleProgram, "u_source"), 0);
    glUniform1i(glGetUniformLocation(m_particleProgram, "u_depth"), 2);
    glUniform2f(glGetUniformLocation(m_particleProgram, "u_size"), float(width), float(height));
    glUniform1i(glGetUniformLocation(m_particleProgram, "u_open"), 7);
    if (!m_particleNoise)
        m_particleNoise = makeNoise();
    glUniform1i(glGetUniformLocation(m_particleProgram, "u_noise"), 8);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, m_particleNoise);
    glActiveTexture(GL_TEXTURE7);
    if (m_skyOpen[0])
    {
        glBindTexture(GL_TEXTURE_2D, m_skyOpen[m_skyIndex]);
        glUniform2f(glGetUniformLocation(m_particleProgram, "u_openFade"), overhead::kNone, overhead::kAll);
    }
    else
    {
        // No sky looked for (its shader failed): the weather as it is, all of it.
        glBindTexture(GL_TEXTURE_2D, m_postCopy);
        glUniform2f(glGetUniformLocation(m_particleProgram, "u_openFade"), -2.0f, -1.0f);
    }
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_postDepth);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_postCopy);
    glEnable(GL_BLEND);
    /* The soft ones at half the size -- a quarter of the pixels, and the
     * rain's spray piles big ones up near the lens -- into a layer cleared to
     * nothing: its colour premultiplied, its alpha what they cover; then the
     * layer laid over the picture, filtered (2026-11-18). */
    if (softCount && m_softLayerProgram)
    {
        const int halfW = SDL_max(width / 2, 1), halfH = SDL_max(height / 2, 1);
        if (!m_softLayer || m_softW != halfW || m_softH != halfH)
        {
            if (m_softLayer)
                glDeleteTextures(1, &m_softLayer);
            glGenTextures(1, &m_softLayer);
            glBindTexture(GL_TEXTURE_2D, m_softLayer);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, halfW, halfH);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, m_postCopy);
            if (!m_softFramebuffer)
                glGenFramebuffers(1, &m_softFramebuffer);
            m_softW = halfW;
            m_softH = halfH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_softFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_softLayer, 0);
        glViewport(0, 0, halfW, halfH);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(glGetUniformLocation(m_particleProgram, "u_fragScale"), float(width) / float(halfW));
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(softCount));
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, width, height);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_softLayerProgram);
        glUniform1i(glGetUniformLocation(m_softLayerProgram, "u_layer"), 9);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_2D, m_softLayer);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_postArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(m_particleArray);
        glUseProgram(m_particleProgram);
    }
    glUniform1f(glGetUniformLocation(m_particleProgram, "u_fragScale"), 1.0f);
    glViewport(0, 0, width, height);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!m_softLayerProgram && softCount)
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(softCount));   // no layer to draw them in: as before
    if (blendedCount)
        glDrawArrays(GL_TRIANGLES, GLsizei(softCount), GLsizei(blendedCount));
    if (addedCount)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDrawArrays(GL_TRIANGLES, GLsizei(softCount + blendedCount), GLsizei(addedCount));
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

/* How much sky is overhead this frame (g_skyOpenShader), from the depth copied
 * into m_postDepth, into m_skyOpen[m_skyIndex] from the last frame's.  The
 * picture's framebuffer is bound again after. */
void ThrashRenderer::meterSkyOpen(bool raining)
{
    if (!m_skyOpenProgram)
        return;
    if (!m_skyOpen[0])
        for (unsigned int& texture : m_skyOpen)
        {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    const Uint64 now = SDL_GetTicksNS();
    const float since = m_skyOpenNs ? float(double(now - m_skyOpenNs) * 1e-9) : 1e9f;
    m_skyOpenNs = now;
    const bool restart = since > overhead::kRestartAfter;
    const float rate = restart ? 1.0f : 1.0f - std::exp(-since / overhead::kSeconds);
    const float wetting = restart ? 1.0f : 1.0f - std::exp(-since / lens::kWetSeconds);
    const float drying = restart ? 1.0f : 1.0f - std::exp(-since / lens::kDrySeconds);
    /* The snow on the glass comes as fast as the sky shows and goes slowly. */
    const float covering = restart ? 1.0f : 1.0f - std::exp(-since / snow::kComeSeconds);
    const float leaving = restart ? 1.0f : 1.0f - std::exp(-since / snow::kGoSeconds);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_skyOpen[m_skyIndex]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_postDepth);
    m_skyIndex ^= 1;
    glBindFramebuffer(GL_FRAMEBUFFER, m_postFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_skyOpen[m_skyIndex], 0);
    glViewport(0, 0, 1, 1);
    glUseProgram(m_skyOpenProgram);
    glUniform1i(glGetUniformLocation(m_skyOpenProgram, "u_depth"), 2);
    glUniform1i(glGetUniformLocation(m_skyOpenProgram, "u_last"), 6);
    glUniform1f(glGetUniformLocation(m_skyOpenProgram, "u_rate"), rate);
    glUniform2f(glGetUniformLocation(m_skyOpenProgram, "u_band"), overhead::kBandFrom, overhead::kBandTo);
    glUniform1f(glGetUniformLocation(m_skyOpenProgram, "u_raining"), raining && m_lensRain > 0.0f ? 1.0f : 0.0f);
    glUniform3f(glGetUniformLocation(m_skyOpenProgram, "u_lensRate"), wetting, drying, lens::kSkyToWet);
    glUniform2f(glGetUniformLocation(m_skyOpenProgram, "u_openFade"), overhead::kNone, overhead::kAll);
    glUniform2f(glGetUniformLocation(m_skyOpenProgram, "u_snowRate"), covering, leaving);
    glBindVertexArray(m_postArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
}

/* Auto-exposure's meter and adaptation (g_meterShader, g_adaptShader): the
 * picture in m_postCopy (on unit 0) metered, and this frame's eye and usual
 * brightness written to m_adapt[m_adaptIndex] from the last frame's.  The
 * picture's framebuffer is bound again after.  False where they cannot be. */
bool ThrashRenderer::meterExposure()
{
    if (!m_meterProgram || !m_adaptProgram)
        return false;
    auto make = [](unsigned int& texture, int size, int levels) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, size, size);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, levels > 1 ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (!m_meter)
    {
        make(m_meter, exposure::kMeterSize, exposure::kMeterLevels);
        make(m_adapt[0], 1, 1);
        make(m_adapt[1], 1, 1);
    }
    /* The step since the last race picture; after a while without one -- the
     * menus, a new race, a pause -- the eye and the usual both start from what
     * is seen now. */
    const Uint64 now = SDL_GetTicksNS();
    const float since = m_adaptNs ? float(double(now - m_adaptNs) * 1e-9) : 1e9f;
    m_adaptNs = now;
    const bool restart = since > exposure::kRestartAfter;
    const float dt = SDL_min(since, exposure::kLongestStep);
    auto rate = [&](float seconds) { return restart ? 1.0f : 1.0f - std::exp(-dt / seconds); };

    glBindFramebuffer(GL_FRAMEBUFFER, m_postFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_meter, 0);
    glViewport(0, 0, exposure::kMeterSize, exposure::kMeterSize);
    glUseProgram(m_meterProgram);
    glUniform1i(glGetUniformLocation(m_meterProgram, "u_source"), 0);
    glUniform1f(glGetUniformLocation(m_meterProgram, "u_stops"), exposure::kStops);
    glUniform2f(glGetUniformLocation(m_meterProgram, "u_texel"), 0.25f / float(exposure::kMeterSize),
                0.25f / float(exposure::kMeterSize));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_meter);
    glGenerateMipmap(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_adapt[m_adaptIndex]);
    m_adaptIndex ^= 1;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_adapt[m_adaptIndex], 0);
    glViewport(0, 0, 1, 1);
    glUseProgram(m_adaptProgram);
    glUniform1i(glGetUniformLocation(m_adaptProgram, "u_meter"), 5);
    glUniform1i(glGetUniformLocation(m_adaptProgram, "u_last"), 6);
    glUniform1f(glGetUniformLocation(m_adaptProgram, "u_meterLevel"), float(exposure::kMeterLevels - 1));
    glUniform4f(glGetUniformLocation(m_adaptProgram, "u_rate"), rate(exposure::kEyeToDark), rate(exposure::kEyeToLight),
                rate(exposure::kUsualToDark), rate(exposure::kUsualToLight));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    /* NFS_EXPOSURE_TRACE: once a second, what the eye and the usual are and
     * what that makes of the picture -- read back, so only when asked. */
    if (m_exposureTrace && now - m_exposureSaidNs > 1000000000ull)
    {
        m_exposureSaidNs = now;
        unsigned char texel[4] = {};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, texel);
        auto stops = [](unsigned char hi, unsigned char lo) {
            return (float(hi) / 255.0f + float(lo) / 255.0f / 255.0f) * exposure::kStops - exposure::kStops;
        };
        const float eye = stops(texel[0], texel[1]), usual = stops(texel[2], texel[3]);
        const float off = usual - eye;
        const float d = (off < 0.0f ? -1.0f : 1.0f) * SDL_max(SDL_fabsf(off) - exposure::kDeadZone, 0.0f) * m_exposure;
        SDL_Log("[EXPOSURE] scene %.2f stops, usual %.2f, picture %+.2f stops", double(eye), double(usual),
                double(SDL_clamp(d, -exposure::kMostDown, exposure::kMostUp)));
    }
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    return true;
}

void ThrashRenderer::postProcess(const ThrashDraw& draw)
{
    m_postDone = true;
    const bool timed = perfTraced() && s_gpu.frameStart != 0;
    if (timed)
        s_gpu.world += gpuLap();
    if (!m_compositeProgram)
        return;
    const int width = int(draw.width), height = int(draw.height);
    const int halfW = SDL_max(width / 2, 1), halfH = SDL_max(height / 2, 1);
    if (!m_postCopy || m_postW != width || m_postH != height)
    {
        if (m_postCopy)
            glDeleteTextures(1, &m_postCopy);
        if (m_bright)
            glDeleteTextures(1, &m_bright);
        glGenTextures(1, &m_postCopy);
        glBindTexture(GL_TEXTURE_2D, m_postCopy);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (m_postDepth)
            glDeleteTextures(1, &m_postDepth);
        glGenTextures(1, &m_postDepth);
        glBindTexture(GL_TEXTURE_2D, m_postDepth);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT16, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_postDepth, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glGenTextures(1, &m_bright);
        glBindTexture(GL_TEXTURE_2D, m_bright);
        glTexStorage2D(GL_TEXTURE_2D, levelsFor(halfW, halfH), GL_RGBA8, halfW, halfH);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_postW = width;
        m_postH = height;
        allocateLightLayer(width, height);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_postCopy);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    const bool shadows = draw.sun[0] > 0.0f;
    /* The motion blur's length: v times a shutter of a ninetieth of a second
     * by NFS_MOTION_BLUR, coming in from 10 m/s and whole from 35. */
    const float ramp = SDL_max(0.0f, SDL_min(1.0f, (draw.speed - 10.0f) / 25.0f));
    const float blur = m_motionBlur * draw.speed * ramp * ramp * (3.0f - 2.0f * ramp) / 90.0f;
    const bool particles = m_particlesOn && draw.particles && !draw.particles->empty();
    float snowAmount = 0.0f;
    if (particles && m_snowAtlas)
    {
        std::size_t snowflakes = 0;
        for (const EffectParticle& particle : *draw.particles)
            snowflakes += particle.type == EffectParticle::Flake;
        snowAmount = SDL_min(1.0f, float(snowflakes) / 24.0f);
    }
    /* Eased: how many flakes are alive changes every frame, and the speed
     * wavers, and a flake whose cell sat at the edge of that share blinked on
     * and off with it. */
    {
        const Uint64 now = SDL_GetTicksNS();
        const float since = m_snowEasedNs ? float(double(now - m_snowEasedNs) * 1e-9) : 1e9f;
        m_snowEasedNs = now;
        const float ease = since > 1.0f ? 1.0f : 1.0f - std::exp(-since / snow::kEaseSeconds);
        const float speedShare = SDL_clamp(draw.speed / snow::kMostSpeed, 0.0f, 1.0f);
        m_snowAmount += (snowAmount - m_snowAmount) * ease;
        m_snowSpeed += (speedShare - m_snowSpeed) * ease;
        snowAmount = m_snowAtlas && m_snowAmount > 0.002f ? m_snowAmount : 0.0f;
    }
    /* The sun's glare: where the sun is on the screen, and how strong it is
     * there -- faded out as it goes off the screen, none behind. */
    float glareAmount = 0.0f, glareX = 0.0f, glareY = 0.0f;
    if (draw.sun[11] > 0.0f && draw.sun[3] > 0.05f && draw.sun[4] != 0.0f && draw.sun[5] != 0.0f)
    {
        glareX = draw.sun[1] * draw.sun[4] / draw.sun[3] + draw.sun[6];
        glareY = draw.sun[2] * draw.sun[5] / draw.sun[3] + draw.sun[7];
        const float out = SDL_max(SDL_max(-glareX, glareX - float(width)), SDL_max(-glareY, glareY - float(height)));
        glareAmount = draw.sun[11] * (1.0f - smoothStep(0.0f, 0.3f, SDL_max(out, 0.0f) / float(height)));
    }
    const bool needDepth = shadows || blur > 0.0f || particles || glareAmount > 0.0f;
    if (needDepth)
    {
        // The world's depth, for its shadows and its blur: a copy, as the buffer stays bound.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depthFramebuffer);
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    }
    if (particles)
    {
        // How much sky is overhead, for the rain and snow; then the particles
        // over the world, then the picture taken again with them in it.
        meterSkyOpen(draw.wetness > 0.0f);
        if (timed)
            s_gpu.post += gpuLap();
        drawParticles(draw, width, height);
        if (timed)
            s_gpu.particles += gpuLap();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_postCopy);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    }
    glBindVertexArray(m_postArray);
    /* The lamps (NFS_LIGHT_GLOW, NFS_LIGHT_TRAILS): the layer they were drawn
     * to again this frame (drawPending) given its smaller levels, for the
     * halo and the trails -- these as long as NFS_LIGHT_TRAILS, coming in from
     * 10 m/s and whole from 35. */
    const bool lights = m_lightLayer && (m_lightGlow > 0.0f || m_lightTrails > 0.0f);
    const float trails = lights ? 0.45f * m_lightTrails * smoothStep(10.0f, 35.0f, draw.speed) : 0.0f;
    if (lights)
    {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_lightLayer);
        glGenerateMipmap(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
    }
    const bool exposed = m_exposure > 0.0f && !without(kPerfExposure) && meterExposure();
    if (m_bloom > 0.0f && !without(kPerfBloom))
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_postFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_bright, 0);
        glViewport(0, 0, halfW, halfH);
        glUseProgram(m_brightProgram);
        glUniform1i(m_brightSource, 0);
        glUniform1i(glGetUniformLocation(m_brightProgram, "u_sky"), 3);
        glUniform1i(glGetUniformLocation(m_brightProgram, "u_hasSky"), m_skyTaken && m_skyW == width && m_skyH == height ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_bright);
        glGenerateMipmap(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    glViewport(0, 0, width, height);
    glUseProgram(m_compositeProgram);
    glUniform1i(m_compositeSource, 0);
    glUniform1i(m_compositeBright, 1);
    glUniform2f(m_compositePixel, 1.0f / float(width), 1.0f / float(height));
    glUniform1f(m_compositeBloom, without(kPerfBloom) ? 0.0f : m_bloom);
    glUniform3f(m_compositeGrade, draw.grade[3], draw.grade[4], m_gradeOn ? 1.0f : 0.0f);
    glUniform3f(m_compositeTint, draw.grade[0], draw.grade[1], draw.grade[2]);
    glUniform1i(m_compositeAa, m_antialias && !without(kPerfAa) ? 1 : 0);
    glUniform1f(m_compositeShadow, shadows && !without(kPerfShadows) ? draw.sun[0] : 0.0f);
    glUniform1f(m_compositeBlur, without(kPerfBlur) ? 0.0f : blur);
    if (needDepth)
    {
        glUniform1i(m_compositeDepth, 2);
        glUniform3f(m_compositeSun, draw.sun[1], draw.sun[2], draw.sun[3]);
        glUniform2f(m_compositeScale, draw.sun[4], draw.sun[5]);
        glUniform2f(m_compositeCentre, draw.sun[6], draw.sun[7]);
        glUniform3f(m_compositeUp, draw.sun[8], draw.sun[9], draw.sun[10]);
        glUniform2f(m_compositeSize, float(width), float(height));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_postDepth);
        glActiveTexture(GL_TEXTURE0);
    }
    if (m_bloom > 0.0f)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_bright);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1f(m_compositeHalo, lights && !without(kPerfHalo) ? m_lightGlow * 0.8f : 0.0f);
    glUniform1f(m_compositeTrailOn, without(kPerfTrails) ? 0.0f : trails);
    glUniform1f(m_compositeGlare, glareAmount);
    glUniform2f(m_compositeGlareAt, glareX, glareY);
    const bool lensRain = m_lensRain > 0.0f && particles && m_skyOpen[0] && !without(kPerfLens);
    const bool snowOnLens = snowAmount > 0.0f && m_skyOpen[0];
    if (lensRain || snowOnLens)
    {
        glUniform1i(glGetUniformLocation(m_compositeProgram, "u_skyOpen"), 6);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, m_skyOpen[m_skyIndex]);
        glActiveTexture(GL_TEXTURE0);
    }
    if (lensRain)
    {
        const double seconds = double(SDL_GetTicksNS() % (1000ull * SDL_NS_PER_SECOND)) * 1e-9;
        glUniform4f(glGetUniformLocation(m_compositeProgram, "u_lens"), m_lensRain, float(seconds),
                    smoothStep(8.0f, 30.0f, draw.speed), 0.0f);
        glUniform2f(m_compositeSize, float(width), float(height));
    }
    else
        glUniform4f(glGetUniformLocation(m_compositeProgram, "u_lens"), 0.0f, 0.0f, 0.0f, 0.0f);
    if (snowOnLens)
    {
        glUniform1i(m_compositeSnowSprites, 7);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, m_snowAtlas);
        glActiveTexture(GL_TEXTURE0);
        const double seconds = std::fmod(double(SDL_GetTicksNS()) * 1e-9, snow::kClockWrap);
        /* As much again as the weather has, up to kMost times it by the speed
         * below kMostSpeed, and no more after. */
        glUniform4f(m_compositeSnow, snowAmount, float(seconds), 1.0f + (snow::kMost - 1.0f) * m_snowSpeed, 0.0f);
        glUniform2f(m_compositeSize, float(width), float(height));
    }
    else
        glUniform4f(m_compositeSnow, 0.0f, 0.0f, 0.0f, 0.0f);
    if (exposed)
    {
        glUniform1i(m_compositeAdapt, 5);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_adapt[m_adaptIndex]);
        glActiveTexture(GL_TEXTURE0);
        glUniform4f(m_compositeExposure, m_exposure, exposure::kDeadZone, exposure::kMostUp, exposure::kMostDown);
    }
    else
        glUniform4f(m_compositeExposure, 0.0f, 0.0f, 0.0f, 0.0f);
    if (glareAmount > 0.0f)
        glUniform2f(m_compositeSize, float(width), float(height));
    if (lights)
    {
        glUniform1i(m_compositeLight, 4);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, m_lightLayer);
        glActiveTexture(GL_TEXTURE0);
        // The middle of the view the trails run from: the projection's, or the picture's.
        const bool centred = draw.sun[6] != 0.0f || draw.sun[7] != 0.0f;
        glUniform2f(m_compositeCentre, centred ? draw.sun[6] : 0.5f * float(width),
                    centred ? draw.sun[7] : 0.5f * float(height));
        glUniform2f(m_compositeSize, float(width), float(height));
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (lights)
    {
        // The layer emptied for the next frame's lamps.
        glBindFramebuffer(GL_FRAMEBUFFER, m_lightFramebuffer);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    }

    if (timed && perfCycling())
    {
        s_gpu.post += gpuLap();
        s_gpu.frameStart = 0;
        /* Per effect left out: a frame's times summed while it is out, the first
         * few after a change let go (their pipeline still the last one's). */
        static int s_current = -1, s_skip = 0;
        static double s_world = 0.0, s_particles = 0.0, s_post = 0.0;
        static unsigned s_frames = 0;
        if (s_perfLeftOut != s_current)
        {
            if (s_current >= 0 && s_frames > 0)
                SDL_Log("[PERF] without %-22s the world %5.2f ms, particles %5.2f, the rest of the pass %5.2f "
                        "(%u frames, wet %d)",
                        kPerfNames[s_current], s_world / s_frames, s_particles / s_frames, s_post / s_frames, s_frames,
                        int(draw.wetness > 0.0f));
            s_current = s_perfLeftOut;
            s_skip = 3;
            s_world = s_particles = s_post = 0.0;
            s_frames = 0;
        }
        if (s_skip > 0)
            --s_skip;
        else
        {
            s_world += s_gpu.world;
            s_particles += s_gpu.particles;
            s_post += s_gpu.post;
            ++s_frames;
        }
        s_gpu.world = s_gpu.particles = s_gpu.post = 0.0;
    }
    else if (timed)
    {
        s_gpu.post += gpuLap();
        s_gpu.frameStart = 0;
        ++s_gpu.frames;
        const Uint64 now = SDL_GetTicksNS();
        if (s_gpu.frames >= 30 && now - s_gpu.said >= SDL_NS_PER_SECOND)
        {
            const double n = double(s_gpu.frames);
            SDL_Log("[PERF] GPU a frame: the world %.2f ms (wet %d), particles %.2f, the rest of the pass %.2f",
                    s_gpu.world / n, int(draw.wetness > 0.0f), s_gpu.particles / n, s_gpu.post / n);
            const Uint64 said = now;
            s_gpu = GpuCost{};
            s_gpu.said = said;
        }
    }
    // Back to the world's state; the batch loop sets the rest as it goes.
    glUseProgram(m_program);
    glBindVertexArray(m_vertexArray);
    glEnable(GL_BLEND);
    if (m_detailTexture)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_detailTexture);
    }
    if (m_reflection)
    {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_reflection);
    }
    glActiveTexture(GL_TEXTURE0);
}

void ThrashRenderer::drawPending(const ThrashDraw& draw)
{
    if (perfTraced() && !m_sceneDrawn && s_gpu.frameStart == 0)
    {
        glFinish();
        s_gpu.frameStart = s_gpu.mark = SDL_GetTicksNS();
        if (perfCycling())
            s_perfLeftOut = int((s_gpu.frameStart / (2 * SDL_NS_PER_SECOND)) % kPerfCount);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    glBindVertexArray(m_vertexArray);
    glViewport(0, 0, draw.width, draw.height);
    glUseProgram(m_program);
    // glOrtho(0, w, 0, h, 0, -65536), as GlideRenderer builds it.
    const float w = float(draw.width), h = float(draw.height);
    const float matrix[16] = {
        2.0f / w, 0.0f,     0.0f,            0.0f,
        0.0f,     2.0f / h, 0.0f,            0.0f,
        0.0f,     0.0f,     2.0f / 65536.0f, 0.0f,
        -1.0f,    -1.0f,    -1.0f,           1.0f,
    };
    glUniformMatrix4fv(m_locations.transform, 1, GL_FALSE, matrix);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ThrashVertex) * draw.vertexCount, draw.vertices, GL_STREAM_DRAW);
    glVertexAttribPointer(m_locations.position, 3, GL_FLOAT, GL_FALSE, sizeof(ThrashVertex),
                          (const void*)offsetof(ThrashVertex, x));
    glEnableVertexAttribArray(m_locations.position);
    glVertexAttribPointer(m_locations.color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ThrashVertex),
                          (const void*)offsetof(ThrashVertex, color));
    glEnableVertexAttribArray(m_locations.color);
    glVertexAttribPointer(m_locations.texCoord, 3, GL_FLOAT, GL_FALSE, sizeof(ThrashVertex),
                          (const void*)offsetof(ThrashVertex, u));
    glEnableVertexAttribArray(m_locations.texCoord);
    glVertexAttribPointer(m_locations.combine, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(ThrashVertex),
                          (const void*)offsetof(ThrashVertex, combine));
    glEnableVertexAttribArray(m_locations.combine);
    glUniform3f(m_locations.fogColor, draw.fogColor[0], draw.fogColor[1], draw.fogColor[2]);
    glUniform1f(m_locations.alphaRef, draw.alphaTestRef);
    glUniform3i(m_locations.chromaKeyColor, GLint(draw.chromaKeyColor >> 16 & 0xff),
                GLint(draw.chromaKeyColor >> 8 & 0xff), GLint(draw.chromaKeyColor & 0xff));

    x86::reg32 first = 0;
    int fogTable = -1;
    const Batch* previous = nullptr;
    bool scissor = false;
    if (m_detailTexture)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_detailTexture);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform2f(m_locations.screen, float(draw.width), float(draw.height));
    // The haze's sky: this frame's once taken (takeSky), until then none.
    /* The player's headlights, this frame's (setHeadlights): as strong as
     * NFS_HEADLIGHT_OBJECTS, and only where it is dark -- by the fog's colour,
     * the light of the hour -- so a rainy afternoon's dipped lamps add next to
     * nothing. */
    float headlight = 0.0f;
    if (!m_hudBegun && draw.head[0] > 0.0f && m_headStrength > 0.0f && draw.head[7] != 0.0f && draw.head[8] != 0.0f)
    {
        const float sky = 0.299f * draw.fogColor[0] + 0.587f * draw.fogColor[1] + 0.114f * draw.fogColor[2];
        const float dark = 1.0f - SDL_min(1.0f, sky / 0.45f);
        headlight = m_headStrength * dark * dark;
        glUniform3f(m_locations.headOrigin, draw.head[1], draw.head[2], draw.head[3]);
        glUniform3f(m_locations.headDir, draw.head[4], draw.head[5], draw.head[6]);
        glUniform3f(m_locations.headUp, draw.head[11], draw.head[12], draw.head[13]);
        glUniform2f(m_locations.projection, draw.head[7], draw.head[8]);
        glUniform2f(m_locations.projCentre, draw.head[9], draw.head[10]);
    }
    float hazeSet = -1.0f;  // what u_haze holds now (per batch, below)
    glUniform1f(m_locations.haze, 0.0f);
    if (m_sky)
    {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_sky);
        glActiveTexture(GL_TEXTURE0);
    }
    /* A race only: a menu's turning car is 3D too, and the pass after it ran
     * the glow and FXAA over the menu's lettering, ghosting it. */
    const bool post = draw.race && m_compositeProgram
                      && (m_bloom > 0.0f || m_gradeOn || m_antialias || draw.sun[0] > 0.0f || draw.sun[11] > 0.0f || m_motionBlur > 0.0f
                          || m_lightGlow > 0.0f || m_lightTrails > 0.0f);
    const float haze = draw.race ? m_haze : 0.0f;
    const bool wet = draw.wetness > 0.0f && m_reflectionValid && m_reflectionW == int(draw.width)
                     && m_reflectionH == int(draw.height);
    glUniform1f(m_locations.wet, wet && !without(kPerfWet) ? draw.wetness : 0.0f);
    /* NFS_WET_TRACE: a wet frame drawn dry, and why. */
    const bool traced = wetTraced();
    if (traced)
    {
        ++m_traceDraws;
        m_traceWet = m_traceWet || wet;
        m_traceWetness = SDL_max(m_traceWetness, draw.wetness);
    }
    if (traced && draw.wetness > 0.0f && !wet)
        SDL_Log("[WET] a wet frame drawn dry: reflection %s, %dx%d against %ux%u",
                m_reflectionValid ? "kept" : "none", m_reflectionW, m_reflectionH, unsigned(draw.width),
                unsigned(draw.height));
    if (wet)
    {
        glUniform2f(m_locations.screen, float(draw.width), float(draw.height));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_reflection);
        glActiveTexture(GL_TEXTURE0);
    }
    for (const Batch& batch : draw.batches)
    {
        /* Its vertices passed over too: left in, the next batch drew them as
         * its own, by its state -- a skipped shadow as whatever came after it
         * (with NFS_CAR_SHADOW=0), a wet road's light by day as dark strips
         * (2026-11-17). */
        if (batch.skipDraw)
        {
            first = batch.end;
            continue;
        }
        /* The world drawn, the HUD to come: its first draw is unfogged and
         * writes no depth, after the world's fogged ones.  That is the picture the
         * next frame's wet road reflects, without the HUD in it.  Or a draw
         * that neither tests nor writes depth: in the rain the game left its
         * fog on into the HUD now and then, no draw after the world was
         * unfogged, and that frame had no whole-picture pass -- its rain, spray,
         * glow and grade gone for the frame, the "weather flicker" (2026-11-17,
         * NFS_WET_TRACE). */
        /* And (the port's haze) the sky as it stands before the world's first
         * draw; and (its whole-picture effects) the world, once, as the HUD
         * begins. */
        if (wet || draw.wetness > 0.0f || haze > 0.0f || post || headlight > 0.0f)
        {
            const ThrashVertex& corner = draw.vertices[first];
            /* Not a light on a wet road: the game lays the tail lights' red
             * strips after the road unfogged and writing no depth, as the HUD
             * draws -- taken for its start, the whole-picture pass (exposure,
             * glow, grade) came between a car's two strips, and the strip
             * after it was the darker half of the streak (2026-11-18). */
            const bool flat = (corner.combine[3] == 0 || !batch.zTest) && !batch.zBuffer && !batch.wetLight;
            if (traced && m_sceneDrawn && m_traceKinds.size() < 4000)
            {
                // F flat, f fogged without depth, t tested without depth writes, W world; a run once
                const char kind = flat ? (m_roadDrawn ? 'F' : 'e') : !batch.zBuffer ? (batch.zTest ? 't' : 'f') : 'W';
                if (m_traceKinds.empty() || m_traceKinds.back() != kind)
                    m_traceKinds.push_back(kind);
            }
            if (!flat)
            {
                if (!m_sceneDrawn && !m_skyTaken && haze > 0.0f)
                    takeSky(int(draw.width), int(draw.height));
                m_sceneDrawn = true;
                m_roadDrawn = m_roadDrawn || batch.detail;
            }
            /* Not before the road: in the rain the game draws something flat
             * after the sky and before the road, for frames on end -- taken for
             * the HUD's start, the reflection was the sky alone and the
             * whole-picture pass came before the road, which was then drawn
             * over the rain's spray and glow: the road dry for those frames
             * (2026-11-17, NFS_FRAME_DUMP).  A frame with no road at all keeps
             * its reflection from the end of the frame. */
            else if (m_sceneDrawn && m_roadDrawn)
            {
                // The HUD has begun; what follows (the mirror's own view among it) is
                // not lit by the lamps of this one.
                if (!m_hudBegun)
                {
                    m_hudBegun = true;
                    headlight = 0.0f;
                    glUniform1f(m_locations.headlight, 0.0f);
                }
                if (!m_reflectionTaken && (wet || draw.wetness > 0.0f))
                    takeReflection(int(draw.width), int(draw.height));
                if (post && !m_postDone)
                {
                    postProcess(draw);
                    previous = nullptr;   // every state set again below
                    scissor = false;
                    fogTable = -1;
                }
            }
        }
        /* The haze on what is blended over the picture, not on what is added
         * to it (alphaBlend off): a fogged additive layer -- the haze's own
         * sky added onto the sky -- only brightened it. */
        {
            const float hazeHere = m_skyTaken && m_skyW == int(draw.width) && m_skyH == int(draw.height)
                                   && batch.alphaBlend && !without(kPerfHaze) ? haze : 0.0f;
            if (hazeHere != hazeSet)
            {
                glUniform1f(m_locations.haze, hazeHere);
                hazeSet = hazeHere;
            }
        }
        if (m_detailTexture && (!previous || batch.detail != previous->detail))
            glUniform1f(m_locations.detail, batch.detail && !without(kPerfGrain) ? m_detailStrength : 0.0f);
        if (m_detailTexture && (!previous || batch.surface != previous->surface))
            glUniform1f(m_locations.surface, batch.surface && !without(kPerfGrain) ? m_surfaceStrength : 0.0f);
        if (!previous || batch.headlit != previous->headlit)
            glUniform1f(m_locations.headlight, batch.headlit && !without(kPerfHeadlights) ? headlight : 0.0f);
        if (!previous || batch.carShine != previous->carShine)
            glUniform1f(m_locations.carShine, batch.carShine ? m_carShine : 0.0f);
        if (!previous || batch.sharpen != previous->sharpen)
            glUniform1i(m_locations.sharpen, batch.sharpen && m_sharpen && !without(kPerfSharpen) ? 1 : 0);
        if (batch.fogTable != fogTable)
        {
            fogTable = batch.fogTable;
            glUniform1fv(m_locations.fogTable, 64, draw.fogTables[fogTable].data());
        }
        if (!previous || batch.texture != previous->texture)
        {
            glBindTexture(GL_TEXTURE_2D, batch.texture < m_glTextures.size() ? m_glTextures[batch.texture] : 0);
            if (batch.suspicious)
                dumpTexture(batch.texture, batch.texture < m_glTextures.size() ? m_glTextures[batch.texture] : 0);
            glUniform1f(m_locations.levels, float(batch.levels));
            glUniform1i(m_locations.format, batch.format);
        }
        if (!previous || batch.wrapS != previous->wrapS || batch.wrapT != previous->wrapT)
        {
            glBindSampler(0, m_samplers[(batch.wrapS ? 1 : 0) + (batch.wrapT ? 2 : 0)]);
            glUniform2f(m_locations.wrap, batch.wrapS ? 1.f : 0.f, batch.wrapT ? 1.f : 0.f);
        }
        if (!previous || batch.zTest != previous->zTest)
        {
            if (batch.zTest)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
        }
        // GR_CMP_ALWAYS keeps the test, and the write, on (the rear-view mirror).
        if (batch.zTest && (!previous || !previous->zTest || batch.zTestAlways != previous->zTestAlways))
            glDepthFunc(batch.zTestAlways ? GL_ALWAYS : GL_LEQUAL);
        /* A car's shadow lies on the road and leaves its depth as the road's:
         * written a little above it, its edge stood up as a step, and the
         * shadows of what stands by the road caught on it in a rim. */
        /* Nor a lamp's glow: light, not a thing, and (below) cut by nothing.
         * Nor a light on a wet road (wetLight), the same in all three. */
        const bool lit = batch.light || batch.wetLight;
        const bool litBefore = previous && (previous->light || previous->wetLight);
        if (!previous || batch.zBuffer != previous->zBuffer || batch.decal != previous->decal || lit != litBefore)
            glDepthMask(batch.zBuffer && !batch.decal && !lit ? GL_TRUE : GL_FALSE);
        /* A car's pixels leave 0 in the picture's alpha, everything else adds
         * its own to what is there: the shadows of what stands by the road are
         * not cast by the cars, which have their own (postProcess) -- and a
         * car's soft shadow, drawn over the road, keeps the road's 1 under it
         * (written as its own faint alpha it was taken for a car, and its rim
         * stood out light inside the shadow of a wall). */
        /* Nor does a lamp's glow touch it: its texture is 8888 or 4444, so its
         * batch counts as a car's, and since it is no longer cut (below) its
         * whole square -- and every square of a tail light's trail -- marked the
         * road under it as car: left out of the motion blur and the shadows, a
         * sharp piece of the car's own shadow stood in each, two broken shadows
         * behind the rear wheels (2026-11-17).  Light leaves the picture's alpha
         * as it was.  So does a light on a wet road: the tail lights' 4444 red
         * strips, written as a car's, marked the road under them as car --
         * left out of the haze and the shadows, darker solid patches round
         * the red, at night with the weather on (2026-11-18). */
        if (!previous || batch.alphaBlend != previous->alphaBlend || batch.car != previous->car
            || batch.decal != previous->decal || lit != litBefore)
        {
            const GLenum alphaSource = batch.car || batch.decal || lit ? GL_ZERO : GL_ONE;
            const GLenum alphaKept = batch.car && !lit ? GL_ZERO : GL_ONE;
            if (batch.alphaBlend)
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, alphaSource, alphaKept);
            else
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, alphaSource, alphaKept);
        }
        /* What a blended batch writes into the depth buffer: only where it
         * shows.  For the world's (not the cars') that is where it is half
         * opaque or more: a faint square of the world's -- a sprite of the
         * game's next to nothing in its picture or its vertices' alpha -- left
         * its whole square in the depth buffer, nearer than what is behind it,
         * and the port's spray and weather behind it were cut away in its
         * shape: the turned boxes in the rain and snow (2026-11-17, found by
         * drawing the particles with no depth test, and then with this cut for
         * the world's batches alone).  A car's keeps the low cut, its glass
         * being faint and still the car's.  The cut discards what is under
         * it, so a lamp's glow has none and writes no depth (above): the
         * softened glow is 8888, the world's by its format, and all but its
         * middle was cut away -- the tail and brake lights unlit by day
         * (2026-11-17). */
        if (!previous || batch.alphaBlend != previous->alphaBlend || batch.zBuffer != previous->zBuffer
            || batch.decal != previous->decal || batch.car != previous->car || lit != litBefore)
            glUniform1f(m_locations.depthCut, batch.zBuffer && !batch.decal && !lit
                                                  ? (batch.car ? kDepthCutCar : kDepthCutWorld) : 0.0f);
        if (!previous || batch.chromaKey != previous->chromaKey)
            glUniform1i(m_locations.chromaKey, batch.chromaKey ? 1 : 0);
        if (!previous || batch.chromaKeyColor != previous->chromaKeyColor)
            glUniform3i(m_locations.chromaKeyColor, GLint(batch.chromaKeyColor >> 16 & 0xff),
                        GLint(batch.chromaKeyColor >> 8 & 0xff), GLint(batch.chromaKeyColor & 0xff));
        if (!previous || batch.alphaRef != previous->alphaRef)
            glUniform1f(m_locations.alphaRef, batch.alphaRef);
        if (!previous || batch.gamma != previous->gamma)
            glUniform1f(m_locations.gamma, batch.gamma);
        if (!previous || batch.cull != previous->cull)
        {
            if (batch.cull)
            {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }
        }
        if (batch.cull && (!previous || !previous->cull || batch.cullMode != previous->cullMode))
            glFrontFace(batch.cullMode == 2 ? GL_CW : GL_CCW);
        // No Y flip: the game's y is GL's here; the blit turns the picture over.
        const bool clipped = batch.clipMaxX > batch.clipMinX && batch.clipMaxY > batch.clipMinY;
        if (!previous || clipped != scissor)
        {
            if (clipped)
                glEnable(GL_SCISSOR_TEST);
            else
                glDisable(GL_SCISSOR_TEST);
        }
        if (clipped && (!previous || !scissor || batch.clipMinX != previous->clipMinX
                        || batch.clipMinY != previous->clipMinY || batch.clipMaxX != previous->clipMaxX
                        || batch.clipMaxY != previous->clipMaxY))
            glScissor(batch.clipMinX, batch.clipMinY, batch.clipMaxX - batch.clipMinX,
                      batch.clipMaxY - batch.clipMinY);
        scissor = clipped;
        previous = &batch;
        glDrawArrays(GL_TRIANGLES, GLint(first), GLsizei(batch.end - first));
        /* A lamp's glow, drawn again onto the lamps' layer (allocateLightLayer)
         * -- behind what stands before it, by the same depth -- for the halo
         * and the trails postProcess makes of it. */
        if (batch.light && post && !m_postDone && m_lightLayer && m_postW == int(draw.width)
            && m_postH == int(draw.height))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_lightFramebuffer);
            glDrawArrays(GL_TRIANGLES, GLint(first), GLsizei(batch.end - first));
            glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        }
        first = batch.end;
    }
    glBindSampler(0, 0);
}

/* The port's overlay (lib/overlay.h), over a race's finished frame, HUD and
 * all: quads in the picture's pixels, each a panel worked out here, a
 * character of the HUD's font or the emblem.  The panel: the HUD's navy,
 * darker to its foot, its corners rounded, a yellow line round it and a glow
 * fading out from that (u_panel: half its width and height, the corners'
 * radius and the glow's reach, HUD units; u_unit: pixels a unit).  The
 * pictures are premultiplied, and so is what comes out. */
static const char g_overlayVertexShader[] =
"layout (location=0) in vec2 a_position;"
"layout (location=1) in vec2 a_uv;"
"layout (location=2) in vec4 a_color;"
"layout (location=3) in float a_kind;"
"out vec2 v_uv;"
"out vec4 v_color;"
"flat out int v_kind;"
"uniform vec2 u_size;"
"void main()"
"{"
"    v_uv = a_uv;"
"    v_color = a_color;"
"    v_kind = int(a_kind + 0.5);"
"    gl_Position = vec4(a_position / u_size * 2.0 - 1.0, 0.0, 1.0);"
"}";

static const char g_overlayShader[] =
"in vec2 v_uv;"
"in vec4 v_color;"
"flat in int v_kind;"
"uniform sampler2D u_font;"
"uniform sampler2D u_icon;"
"uniform vec4 u_panel;"
"uniform vec3 u_glow;"
"uniform float u_unit;"
"out vec4 o_color;"
"void main()"
"{"
"    if (v_kind == 1)"
"    {"
"        vec4 t = texture(u_font, v_uv);"
"        o_color = vec4(t.rgb * v_color.rgb, t.a) * v_color.a;"
"        return;"
"    }"
"    if (v_kind == 2)"
"    {"
"        o_color = texture(u_icon, v_uv) * v_color.a;"
"        return;"
"    }"
"    vec2 q = abs(v_uv) - (u_panel.xy - u_panel.z);"
"    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - u_panel.z;"
"    float down = clamp(v_uv.y / (2.0 * u_panel.y) + 0.5, 0.0, 1.0);"
"    vec3 fill = v_color.rgb * mix(1.0, 0.45, down);"
"    float fillA = clamp(0.5 - d * u_unit, 0.0, 1.0) * 0.78;"
"    float line = clamp(1.0 - (abs(d + 0.45) - 0.45) * u_unit, 0.0, 1.0);"
"    float glow = d > -0.5 ? exp(-max(d, 0.0) / (u_panel.w * 0.45)) * 0.55 : 0.0;"
"    float edge = clamp(line * 0.95 + glow, 0.0, 1.0);"
"    o_color = (vec4(fill * fillA, fillA) * (1.0 - edge) + vec4(u_glow * edge, edge)) * v_color.a;"
"}";

static unsigned int overlayTexture(int width, int height, const std::vector<std::uint8_t>& texels)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    int levels = 1;
    for (int size = SDL_max(width, height); size > 1; size >>= 1)
        ++levels;
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, width, height);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

/* Into the framebuffer as the frame left it (its picture bound); every state
 * touched put back as it was, the next frame's drawing assuming its own. */
void ThrashRenderer::drawOverlay(const Overlay& overlay, int width, int height)
{
    const std::shared_ptr<const OverlayArt>& art = overlay.art;
    constexpr GLint kFontUnit = 9, kIconUnit = 10;   // clear of every unit the frame uses
    if (m_overlayFailed || !art || overlay.quads.empty() || width <= 0 || height <= 0)
        return;
    if (!m_overlayProgram)
    {
        const GLuint vertex = compileShader(GL_VERTEX_SHADER, "", g_overlayVertexShader);
        const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, "", g_overlayShader);
        m_overlayProgram = glCreateProgram();
        glAttachShader(m_overlayProgram, vertex);
        glAttachShader(m_overlayProgram, fragment);
        glLinkProgram(m_overlayProgram);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(m_overlayProgram, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "[GFX] overlay: its shader did not link; none drawn");
            m_overlayFailed = true;
            return;
        }
        glGenVertexArrays(1, &m_overlayArray);
        glGenBuffers(1, &m_overlayBuffer);
    }
    GLint activeTexture = 0, program = 0, array = 0, buffer = 0, fontWas = 0, iconWas = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &array);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    glActiveTexture(GL_TEXTURE0 + kFontUnit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &fontWas);
    glActiveTexture(GL_TEXTURE0 + kIconUnit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &iconWas);
    if (m_overlayUploaded != art.get())
    {
        if (m_overlayFont)
            glDeleteTextures(1, &m_overlayFont);
        if (m_overlayIcon)
            glDeleteTextures(1, &m_overlayIcon);
        m_overlayFont = overlayTexture(art->fontW, art->fontH, art->font);
        m_overlayIcon = overlayTexture(art->iconW, art->iconH, art->icon);
        m_overlayUploaded = art.get();
    }
    glActiveTexture(GL_TEXTURE0 + kFontUnit);
    glBindTexture(GL_TEXTURE_2D, m_overlayFont);
    glActiveTexture(GL_TEXTURE0 + kIconUnit);
    glBindTexture(GL_TEXTURE_2D, m_overlayIcon);
    GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLint blendSrc = 0, blendDst = 0, blendSrcA = 0, blendDstA = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
                    blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);

    // Six corners a quad: position (pixels), uv, colour, kind.
    const float unit = float(height) / kOverlayUnits;
    std::vector<float> corners;
    corners.reserve(overlay.quads.size() * 6 * 9);
    for (const OverlayQuad& q : overlay.quads)
    {
        const float xs[2] = { q.x0 * unit, q.x1 * unit }, ys[2] = { q.y0 * unit, q.y1 * unit };
        const float us[2] = { q.u0, q.u1 }, vs[2] = { q.v0, q.v1 };
        static const int order[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
        for (const auto& c : order)
        {
            const float corner[9] = { xs[c[0]], ys[c[1]], us[c[0]], vs[c[1]], q.r, q.g, q.b, q.a, float(q.kind) };
            corners.insert(corners.end(), corner, corner + 9);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   // the picture's alpha is the frame's own
    glUseProgram(m_overlayProgram);
    glUniform2f(glGetUniformLocation(m_overlayProgram, "u_size"), float(width), float(height));
    glUniform1i(glGetUniformLocation(m_overlayProgram, "u_font"), kFontUnit);
    glUniform1i(glGetUniformLocation(m_overlayProgram, "u_icon"), kIconUnit);
    glUniform4f(glGetUniformLocation(m_overlayProgram, "u_panel"), overlay.panelHalfW, overlay.panelHalfH,
                overlay.panelRadius, SDL_max(overlay.panelGlow, 0.01f));
    glUniform3f(glGetUniformLocation(m_overlayProgram, "u_glow"), overlay.glowR, overlay.glowG, overlay.glowB);
    glUniform1f(glGetUniformLocation(m_overlayProgram, "u_unit"), unit);
    glBindVertexArray(m_overlayArray);
    glBindBuffer(GL_ARRAY_BUFFER, m_overlayBuffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(corners.size() * sizeof(float)), corners.data(), GL_STREAM_DRAW);
    const GLsizei stride = 9 * sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(2 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(4 * sizeof(float)));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (const void*)(8 * sizeof(float)));
    for (GLuint k = 0; k < 4; ++k)
        glEnableVertexAttribArray(k);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(corners.size() / 9));

    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glBlendFuncSeparate(GLenum(blendSrc), GLenum(blendDst), GLenum(blendSrcA), GLenum(blendDstA));
    (blend ? glEnable : glDisable)(GL_BLEND);
    (depth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (cull ? glEnable : glDisable)(GL_CULL_FACE);
    (scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(buffer));
    glBindVertexArray(GLuint(array));
    glUseProgram(GLuint(program));
    glActiveTexture(GL_TEXTURE0 + kIconUnit);
    glBindTexture(GL_TEXTURE_2D, GLuint(iconWas));
    glActiveTexture(GL_TEXTURE0 + kFontUnit);
    glBindTexture(GL_TEXTURE_2D, GLuint(fontWas));
    glActiveTexture(GLenum(activeTexture));
}

void ThrashRenderer::swap()
{
    /* NFS_PERF_TRACE: a frame slower than kSlowFrameMs on the game thread, said
     * at once with what the game uploaded in it. */
    if (perfTraced())
    {
        constexpr double kSlowFrameMs = 35.0;
        static Uint64 s_last = 0;
        const Uint64 now = SDL_GetTicksNS();
        const double ms = s_last ? double(now - s_last) * 1e-6 : 0.0;
        s_last = now;
        if (ms > kSlowFrameMs)
            SDL_Log("[PERF] slow frame: %.1f ms on the game thread; %u textures uploaded in it, %.0f KB, %.2f ms",
                    ms, s_uploads, s_uploadKB, s_uploadMs);
        s_uploads = 0;
        s_uploadKB = s_uploadMs = 0.0;
    }
    m_renderer->ensureFramePacing();
    /* While a race loads (Renderer::setUnpaced), its loading picture is
     * flipped again and again as the files come in: at most one of those
     * frames in 50 ms is drawn and shown, the rest let go -- each one shown
     * waited for the screen's refresh, and the loading with it. */
    if (m_renderer->unpaced())
    {
        const Uint64 now = SDL_GetTicksNS();
        if (m_lastShownNs && now - m_lastShownNs < 50 * SDL_NS_PER_MS)
        {
            if (m_vertexCount)
            {
                flush();
                m_batches.clear();
                m_vertexCount = 0;
                m_fogTables.front() = m_fogTables[m_fogTableIndex];
                m_fogTables.resize(1);
                m_fogTableIndex = 0;
                ++m_renderStamp;
            }
            return;
        }
        m_lastShownNs = now;
    }
    else
        m_lastShownNs = 0;
    renderPending();
    GlideCounters& counters = GlideRenderer::counters();
    ++counters.frames;
    counters.depth16Frames += m_depth16;
    m_renderer->paceFrame();
    const bool keepFrame = m_wetness > 0.0f && m_detailTexture;
    const int frameW = int(m_renderer->m_width), frameH = int(m_renderer->m_height);
    // The port's overlay, over a race's frame alone (setOverlay).
    std::shared_ptr<const Overlay> overlay = m_raceView ? m_overlay : nullptr;
    glthread::post(m_renderer, [this, keepFrame, frameW, frameH, overlay]() {
        /* The texture the next frame's wet road reflects (only while the road
         * is wet); drawPending fills it as the HUD begins. */
        if (keepFrame)
        {
            if (!m_reflection || m_reflectionW != frameW || m_reflectionH != frameH)
            {
                if (m_reflection)
                    glDeleteTextures(1, &m_reflection);
                glGenTextures(1, &m_reflection);
                glBindTexture(GL_TEXTURE_2D, m_reflection);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, frameW, frameH);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                m_reflectionW = frameW;
                m_reflectionH = frameH;
            }
            // A frame that never reached its HUD (a menu) keeps it whole.
            if (!m_reflectionTaken)
            {
                if (wetTraced() && m_sceneDrawn)
                    SDL_Log("[WET] a race frame with no HUD seen%s (%d draws; after the scene: %s): its reflection "
                            "taken at the end, HUD and all",
                            m_roadDrawn ? "" : " (no road drawn)", m_traceDraws, m_traceKinds.c_str());
                takeReflection(frameW, frameH);
            }
        }
        else
        {
            m_reflectionValid = false;
        }
        // Over the HUD, after the reflection was taken of the frame without it.
        if (overlay && m_sceneDrawn)
            drawOverlay(*overlay, frameW, frameH);
        /* NFS_FRAME_DUMP: NFS_FRAME_DUMP_COUNT race frames in a row (90, three
         * seconds) from the NFS_FRAME_DUMP_AT'th (600: past the countdown, the
         * car on its way), each with a line of how it was drawn.  A race's
         * frame: one with a whole-picture pass, or a wet road (the menus'
         * turning car has neither). */
        static const char* dumpTo = SDL_getenv("NFS_FRAME_DUMP");
        static const int dumpAt = SDL_getenv("NFS_FRAME_DUMP_AT") ? SDL_atoi(SDL_getenv("NFS_FRAME_DUMP_AT")) : 600;
        static const int dumpCount =
            SDL_getenv("NFS_FRAME_DUMP_COUNT") ? SDL_atoi(SDL_getenv("NFS_FRAME_DUMP_COUNT")) : 90;
        static int raceFrames = 0, dumped = 0;
        if (dumpTo && (SDL_getenv("NFS_FRAME_DUMP_ALL") != nullptr || (m_sceneDrawn && (m_postDone || m_traceWetness > 0.0f)))
            && dumped < dumpCount && ++raceFrames >= dumpAt)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_renderer->m_texture, 0);
            dumpFrame(dumpTo, dumped, frameW, frameH);
            SDL_Log("[FRAME] %05d: %d draws, wetness %.2f, wet %d, reflection taken at the HUD %d, post %d, "
                    "after the scene: %s",
                    dumped, m_traceDraws, double(m_traceWetness), int(m_traceWet), int(m_reflectionTaken && m_hudBegun),
                    int(m_postDone), m_traceKinds.c_str());
            ++dumped;
        }
        m_traceWet = false;
        m_traceWetness = 0.0f;
        m_reflectionTaken = false;
        m_roadDrawn = false;
        s_gpu.frameStart = 0;   // NFS_PERF_TRACE: a frame with no pass times nothing
        m_traceKinds.clear();
        m_traceDraws = 0;
        m_sceneDrawn = false;
        m_skyTaken = false;
        m_postDone = false;
        m_hudBegun = false;
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, m_renderer->m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    });
    m_renderer->present();
    m_vertexCount = 0;
}

/* No atlas to run out of: GL memory is the only limit. */
void ThrashRenderer::atlasFreeSpace(x86::reg32& texels, x86::reg32& wholeTiles, x86::reg32& total) const
{
    total = 4096 * 4096;
    texels = total;
    wholeTiles = 256;
}

/* What a texture takes of the Voodoo's texture memory: GlideRenderer's value,
 * so THRASH_talloc lays its addresses out as before -- each address is the
 * key of one texture here. */
x86::reg32 ThrashRenderer::getTextureMemSize(x86::reg32, x86::reg32, TextureFormat)
{
    return sizeof(void*);
}

void ThrashRenderer::setZWrite(bool enable)
{
    if (enable != m_zBuffer)
    {
        flush();
        m_zBuffer = enable;
    }
}

void ThrashRenderer::setZTest(bool enable)
{
    if (enable != m_zTest)
    {
        flush();
        m_zTest = enable;
    }
}

void ThrashRenderer::setDepthAlways(bool always)
{
    if (always != m_depthAlways)
    {
        flush();
        m_depthAlways = always;
    }
}

void ThrashRenderer::setAlphaBlendDst(AlphaBlend method)
{
    if (m_alphaBlend != (method == AB_1mSrcAlpha))
    {
        flush();
        m_alphaBlend = method == AB_1mSrcAlpha;
    }
}

// The target holds eight bits a channel: nothing to dither.
void ThrashRenderer::setDither(bool)
{
}

void ThrashRenderer::setCullMode(x86::reg32 mode)
{
    const bool enable = mode != 0;
    if (enable != m_cull || mode != m_cullMode)
    {
        flush();
        m_cull = enable;
        m_cullMode = mode;
    }
}

void ThrashRenderer::setChromakeyMode(bool enable)
{
    if (enable != m_chromaKey)
    {
        flush();
        m_chromaKey = enable;
    }
}

void ThrashRenderer::setCarSurface(bool car)
{
    if (car != m_carSurface)
    {
        flush();
        m_carSurface = car;
    }
}

/* Each batch is drawn with one sampler: a change ends the batch. */
void ThrashRenderer::setTexClampMode(bool wrapS, bool wrapT)
{
    if (wrapS != m_wrapS || wrapT != m_wrapT)
    {
        flush();
        m_wrapS = wrapS;
        m_wrapT = wrapT;
    }
}

void ThrashRenderer::setChromakeyValue(x86::reg32 color)
{
    /* Each of the game's pictures is cut out by its own colour -- the weather's
     * drops by their green, a cut-out's by black -- and the triangles already
     * queued were to be cut by the one in force as they came: a change ends the
     * batch. */
    if (color != m_chromaKeyColor)
    {
        flush();
        m_chromaKeyColor = color;
        /* NFS_CHROMA_TRACE: each colour the game's pictures are cut out by, once
         * -- the weather's drops are 565 with a green key round them, so more
         * than one is in use in a frame. */
        static const bool traced = SDL_getenv("NFS_CHROMA_TRACE") != nullptr;
        static x86::reg32 seen[8] = {};
        static int count = 0;
        if (traced)
        {
            bool known = false;
            for (int k = 0; k < count; ++k)
                known = known || seen[k] == color;
            if (!known && count < 8)
            {
                seen[count++] = color;
                SDL_Log("[CHROMA] keyed out: 0x%08x (%u,%u,%u)", color, color >> 16 & 0xff, color >> 8 & 0xff,
                        color & 0xff);
            }
        }
    }
}

void ThrashRenderer::setClipWindow(x86::reg32 minX, x86::reg32 minY, x86::reg32 maxX, x86::reg32 maxY)
{
    if (minX != m_clipMinX || minY != m_clipMinY || maxX != m_clipMaxX || maxY != m_clipMaxY)
    {
        flush();
        m_clipMinX = minX;
        m_clipMinY = minY;
        m_clipMaxX = maxX;
        m_clipMaxY = maxY;
    }
}

void ThrashRenderer::setGamma(float correction)
{
    if (correction != m_gamma)
    {
        flush();
        m_gamma = correction;
    }
}

void ThrashRenderer::setColorFactor(float colorFactor)
{
    m_colorFactor = colorFactor;
}

void ThrashRenderer::setAlphaFactor(float alphaFactor)
{
    m_alphaFactor = alphaFactor;
}

// Read as each triangle is taken (see GlideRenderer::setFogMode).
void ThrashRenderer::setFogMode(x86::reg32 mode)
{
    m_fogMode = mode;
}

void ThrashRenderer::setFogColor(x86::reg32 color)
{
    const float r = float((color >> 16) & 0xff) / 255.f;
    const float g = float((color >> 8) & 0xff) / 255.f;
    const float b = float(color & 0xff) / 255.f;
    if (r != m_fogColor[0] || g != m_fogColor[1] || b != m_fogColor[2])
    {
        flush();
        m_fogColor[0] = r;
        m_fogColor[1] = g;
        m_fogColor[2] = b;
    }
}

// A repeat of the table in force changes nothing (see GlideRenderer::setFogTable).
void ThrashRenderer::setFogTable(const x86::reg8* table)
{
    if (m_fogTableValid && std::memcmp(table, m_fogTableRaw, sizeof m_fogTableRaw) == 0)
        return;
    ++GlideRenderer::counters().fogTables;
    flush();
    std::memcpy(m_fogTableRaw, table, sizeof m_fogTableRaw);
    m_fogTableValid = true;
    std::array<float, 64> factors;
    for (int i = 0; i < 64; ++i)
        factors[i] = float(table[i]) / 255.f;
    m_fogTables.push_back(factors);
    m_fogTableIndex = int(m_fogTables.size()) - 1;
}

void ThrashRenderer::setAlphaTestRef(x86::reg32 value)
{
    const float ref = float(value & 0xff) / 255.f;
    if (ref != m_alphaTestRef)
    {
        flush();
        m_alphaTestRef = ref;
    }
}

/* The texture downloaded to this address, or the one-texel one downloaded to 0
 * when the window opened, for an address nothing was downloaded to. */
void ThrashRenderer::setTexture(x86::reg32, x86::reg32 address)
{
    auto found = m_textures.find(address);
    if (found == m_textures.end())
        found = m_textures.find(0);
    if (found == m_textures.end())
        return;
    if (found->second.index != m_texture)
    {
        flush();
        m_texture = found->second.index;
        m_textureLevels = found->second.levels;
        m_textureFormat = found->second.format;
        m_textureShadow = found->second.shadow;
        m_textureLight = found->second.light;
        m_textureWetStreak = found->second.wetStreak;
    }
    found->second.useStamp = m_renderStamp;
}

/* The game's texels as they are, every level it supplies: 565 and 4444 in
 * formats of their own (4444 with its channels put right by the texture's
 * swizzle), 8888 as bytes swizzled from B, G, R, A, and only 1555 turned
 * round a bit into OpenGL's 5551.  GlideRenderer widened every one of them to
 * RGBA8 on the CPU first.  The GL thread gives the texture storage of that
 * size, levels and format, or keeps what it had when they are the same. */
/* Port: three of the game's effect pictures (gamedata/render/pc/sfx.fsh),
 * made softer as they are uploaded.  Found by their texels, as the full-colour
 * 32-bit pictures arrive (a 16-bit one, NFS_TEXTURES32=0, is left as it is):
 *
 * - "glw2", the headlights' pool the projected headlights lay on the road:
 *   bright to within a few texels of its square's edge, so the lit road ended
 *   in a hard-edged box.  It now fades to nothing towards the edges, its reach
 *   down the road unchanged.
 * - "glw4", the lights' streak on a wet road: the same, at its sides.
 * - "shad", a car's shadow: opaque to three texels of its edge.  Its alpha now
 *   falls off over the outer third of each side, round at the corners.
 * - "glw0" and "glw3", the glow of a lamp -- head and tail lights, the
 *   street's lamps: a star of hard spikes round a white dot.  Now (NFS_LIGHT_GLOW)
 *   a soft round glow, a bright core in a wide falloff with a trace of the
 *   spikes left in it, its light in a hot core and a slow falloff.  And the
 *   textures are marked, for the light layer (initPost).
 *
 * Texels as setTextureData holds an 8888 one: B, G, R, A. */
enum class SoftenedPicture { None, Pool, Streak, Shadow, Glow };

static std::uint32_t texelHash(const x86::reg8* data, std::size_t bytes)
{
    std::uint32_t hash = 0x811c9dc5u;
    for (std::size_t i = 0; i < bytes; ++i)
        hash = (hash ^ data[i]) * 0x01000193u;
    return hash;
}

/* NFS_LIGHT_GLOW: the lamps' glow made soft and given a halo, how much of it
 * (1 by default; 0 or off for the game's star); NFS_LIGHT_TRAILS: how long the
 * lamps' trails at speed (1 by default; 0 or off for none). */
static float lightSetting(const char* name, float otherwise)
{
    const char* value = SDL_getenv(name);
    if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
        return otherwise;
    if (SDL_strcmp(value, "0") == 0 || SDL_strcasecmp(value, "off") == 0)
        return 0.0f;
    const float parsed = float(SDL_atof(value));
    return parsed > 0.0f ? SDL_min(parsed, 3.0f) : otherwise;
}

static float lightGlowSetting()
{
    static const float setting = lightSetting("NFS_LIGHT_GLOW", 1.0f);
    return setting;
}

static float lightTrailSetting()
{
    static const float setting = lightSetting("NFS_LIGHT_TRAILS", 1.0f);
    return setting;
}

// NFS_SOFT_EFFECTS=0 leaves the three as the game has them.
static bool softenEffects()
{
    static const bool on = [] {
        const char* value = SDL_getenv("NFS_SOFT_EFFECTS");
        return !(value && SDL_strcmp(value, "0") == 0);
    }();
    return on;
}

// The game's 4444 light on a wet road (the upload's own texels).
constexpr x86::reg32 kWetLightSize = 64;
constexpr std::uint32_t kWetLightHash = 0xc4bb8dc1u;

static SoftenedPicture recognise(const std::vector<x86::reg8>& texels, x86::reg32 size)
{
    if (size == 128 && texelHash(texels.data(), 128 * 128 * 4) == 0x617d2d72u)
        return SoftenedPicture::Pool;
    if (size == 64 && texelHash(texels.data(), 64 * 64 * 4) == 0xa072609fu)
        return SoftenedPicture::Streak;
    if (size == 32 && texelHash(texels.data(), 32 * 32 * 4) == 0x3da5cb05u)
        return SoftenedPicture::Shadow;
    if ((size == 64 && texelHash(texels.data(), 64 * 64 * 4) == 0xfe572da0u)
        || (size == 32 && texelHash(texels.data(), 32 * 32 * 4) == 0xe0a827c2u))
        return SoftenedPicture::Glow;
    return SoftenedPicture::None;
}

static float smoothStep(float edge0, float edge1, float x)
{
    const float t = std::min(1.0f, std::max(0.0f, (x - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

/* A lamp's glow made round: grey, its alpha the same as its grey (as the
 * game's is), so that what it adds is the square of a texel.  That, the light
 * it adds, is a hot core in a wide, slow falloff, gone before the circle's
 * edge -- the texel its square root -- with `spikes` of the original's star
 * left in.  Every level the game supplied is made again from the first. */
static void softenGlow(std::vector<x86::reg8>& texels, x86::reg32 size, x86::reg32 levels, float spikes)
{
    for (x86::reg32 y = 0; y < size; ++y)
        for (x86::reg32 x = 0; x < size; ++x)
        {
            x86::reg8* t = &texels[(std::size_t(y) * size + x) * 4];
            const float px = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float py = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float r = std::sqrt(px * px + py * py);
            const float light = (0.5f * std::exp(-(r / 0.10f) * (r / 0.10f)) + 0.5f * std::exp(-std::pow(r / 0.38f, 1.2f)))
                                * (1.0f - smoothStep(0.55f, 1.0f, r));
            const float soft = std::sqrt(std::min(1.0f, 1.25f * light)) * 255.0f;
            for (int c = 0; c < 4; ++c)
                t[c] = x86::reg8(std::lround(std::min(255.0f, soft * (1.0f - spikes) + float(t[c]) * spikes)));
        }
    // The smaller levels, each the average of four of the one above.
    std::size_t from = 0;
    for (x86::reg32 at = size, level = 1; level < levels && at > 1; at >>= 1, ++level)
    {
        const std::size_t to = from + std::size_t(at) * at * 4;
        const x86::reg32 half = at / 2;
        for (x86::reg32 y = 0; y < half; ++y)
            for (x86::reg32 x = 0; x < half; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    auto texel = [&](x86::reg32 tx, x86::reg32 ty) {
                        return int(texels[from + (std::size_t(ty) * at + tx) * 4 + c]);
                    };
                    const int sum = texel(2 * x, 2 * y) + texel(2 * x + 1, 2 * y) + texel(2 * x, 2 * y + 1)
                                    + texel(2 * x + 1, 2 * y + 1);
                    texels[to + (std::size_t(y) * half + x) * 4 + c] = x86::reg8((sum + 2) / 4);
                }
        from = to;
    }
}

/* Each smaller level the game supplied, again from the first: the average of
 * four of the one above (the GPU draws a picture seen small from these). */
static void rebuildLevels(std::vector<x86::reg8>& texels, x86::reg32 size, x86::reg32 levels)
{
    std::size_t from = 0;
    for (x86::reg32 at = size, level = 1; level < levels && at > 1; at >>= 1, ++level)
    {
        const std::size_t to = from + std::size_t(at) * at * 4;
        const x86::reg32 half = at / 2;
        if (to + std::size_t(half) * half * 4 > texels.size())
            return;
        for (x86::reg32 y = 0; y < half; ++y)
            for (x86::reg32 x = 0; x < half; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    auto texel = [&](x86::reg32 tx, x86::reg32 ty) {
                        return int(texels[from + (std::size_t(ty) * at + tx) * 4 + c]);
                    };
                    const int sum = texel(2 * x, 2 * y) + texel(2 * x + 1, 2 * y) + texel(2 * x, 2 * y + 1)
                                    + texel(2 * x + 1, 2 * y + 1);
                    texels[to + (std::size_t(y) * half + x) * 4 + c] = x86::reg8((sum + 2) / 4);
                }
        from = to;
    }
}

/* The headlights' pool (glw2) is the two lamps' hot spots in a wide glow:
 * laid on the road they were two narrow, brighter beams inside the pool.
 * Blurred (three passes of a box kPoolBlur texels wide each way, as near a
 * Gaussian as needed) the pool keeps its shape and its brightness overall and
 * loses the beams; the edge fade follows. */
constexpr int kPoolBlur = 13;

static void blurPool(std::vector<x86::reg8>& texels, x86::reg32 size)
{
    const int n = int(size);
    std::vector<float> a(std::size_t(n) * n * 4), b(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = float(texels[i]);
    auto pass = [&](const std::vector<float>& from, std::vector<float>& to, bool across) {
        const int r = kPoolBlur / 2;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    float sum = 0.0f;
                    for (int k = -r; k <= r; ++k)
                    {
                        const int sx = across ? std::clamp(x + k, 0, n - 1) : x;
                        const int sy = across ? y : std::clamp(y + k, 0, n - 1);
                        sum += from[(std::size_t(sy) * n + sx) * 4 + c];
                    }
                    to[(std::size_t(y) * n + x) * 4 + c] = sum / float(2 * r + 1);
                }
    };
    for (int round = 0; round < 3; ++round)
    {
        pass(a, b, true);
        pass(b, a, false);
    }
    for (std::size_t i = 0; i < a.size(); ++i)
        texels[i] = x86::reg8(std::lround(std::min(255.0f, a[i])));
}

static void soften(std::vector<x86::reg8>& texels, x86::reg32 size, x86::reg32 levels, SoftenedPicture picture)
{
    if (picture == SoftenedPicture::Glow)
    {
        softenGlow(texels, size, levels, 0.12f);
        return;
    }
    if (picture == SoftenedPicture::Pool)
        blurPool(texels, size);
    for (x86::reg32 y = 0; y < size; ++y)
        for (x86::reg32 x = 0; x < size; ++x)
        {
            // -1 to 1 across the picture, at the texels' centres.
            const float px = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float py = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            x86::reg8* t = &texels[(std::size_t(y) * size + x) * 4];
            if (picture == SoftenedPicture::Shadow)
            {
                /* A car's shadow: a soft **ellipse** in the middle of the
                 * picture, gone before the square's edge.  It was a rounded
                 * square until 2026-11-17 -- the picture is opaque to within
                 * three texels of its edge, and a squircle of it, however soft
                 * the rim, is still a square on the road: a turned, dark,
                 * translucent square the size of a car, which on a bright road
                 * reads as a rectangle of opaque paint rather than a shadow
                 * (TODOs.md).  The corners of the picture are cut away by the
                 * radius, so no angle of the quad can show one. */
                /* 2026-11-17 again: an ellipse gone by 0.55 of the radius left
                 * the cars all but without shadows -- the car stands over the
                 * whole of what was dark, and only its faint rim showed beside
                 * it.  So a rounder rectangle (a superellipse, kShadowRound),
                 * dark out to kShadowSolid of the way and gone at the edge's
                 * middle, its corners still cut away. */
                const float r = std::cbrt(std::pow(std::fabs(px), kShadowRound) + std::pow(std::fabs(py), kShadowRound));
                const float alpha = 1.0f - smoothStep(kShadowSolid, 1.0f, r);
                t[3] = x86::reg8(std::lround(255.0f * kShadowDarkness * alpha));
                continue;
            }
            // The pool and the streak: dimmed to nothing at the edge, the
            // pool across its sides and far end, the streak at its sides.
            const float across = std::fabs(px);
            const float along = picture == SoftenedPicture::Pool ? std::fabs(py) : 0.0f;
            const float fade = (1.0f - smoothStep(0.55f, 0.98f, across)) * (1.0f - smoothStep(0.6f, 0.98f, along));
            for (int c = 0; c < 4; ++c)
                t[c] = x86::reg8(std::lround(float(t[c]) * fade));
        }
    if (picture == SoftenedPicture::Pool)
        rebuildLevels(texels, size, levels);
}

void ThrashRenderer::setTextureData(x86::reg32 tmu, x86::reg32 address, const void* data, x86::reg32 largeMipmap,
                                    x86::reg32 smallMipmap, TextureFormat format)
{
    if (!perfTraced())
    {
        uploadTexture(tmu, address, data, largeMipmap, smallMipmap, format);
        return;
    }
    const Uint64 began = SDL_GetTicksNS();
    uploadTexture(tmu, address, data, largeMipmap, smallMipmap, format);
    ++s_uploads;
    const double side = double(256 >> largeMipmap);
    s_uploadKB += side * side * (format == TF_ARGB_8888 ? 4.0 : 2.0) / 1024.0;
    s_uploadMs += double(SDL_GetTicksNS() - began) * 1e-6;
}

void ThrashRenderer::uploadTexture(x86::reg32, x86::reg32 address, const void* data, x86::reg32 largeMipmap,
                                   x86::reg32 smallMipmap, TextureFormat format)
{
    const x86::reg32 size = 256 >> largeMipmap;
    const x86::reg32 smallest = 256 >> smallMipmap;
    /* The lamps' glow comes in 16 bits too (4444, the race's own copy of
     * gamedata/render/pc's pictures): widened to 32 here, so that its softened
     * falloff (softenGlow) has the steps it needs. */
    std::vector<x86::reg8> widened;
    const bool glow16 = format == TF_ARGB_4444 && largeMipmap == smallMipmap && (size == 32 || size == 64)
                        && softenEffects() && lightGlowSetting() > 0.0f
                        && texelHash(static_cast<const x86::reg8*>(data), std::size_t(size) * size * 2)
                               == (size == 32 ? 0xedcd8669u : 0x0ddae155u);
    if (glow16)
    {
        const x86::reg8* from = static_cast<const x86::reg8*>(data);
        widened.resize(std::size_t(size) * size * 4);
        for (std::size_t i = 0; i < std::size_t(size) * size; ++i)
        {
            const unsigned texel = unsigned(from[i * 2]) | unsigned(from[i * 2 + 1]) << 8;
            widened[i * 4 + 0] = x86::reg8((texel & 15) * 17);         // B
            widened[i * 4 + 1] = x86::reg8((texel >> 4 & 15) * 17);    // G
            widened[i * 4 + 2] = x86::reg8((texel >> 8 & 15) * 17);    // R
            widened[i * 4 + 3] = x86::reg8((texel >> 12 & 15) * 17);   // A
        }
        data = widened.data();
        format = TF_ARGB_8888;
    }
    const x86::reg32 texelBytes = format == TF_ARGB_8888 ? 4 : 2;
    x86::reg32 levels = 0;
    std::size_t texels = 0;
    for (x86::reg32 at = size; at >= smallest && at; at >>= 1, ++levels)
        texels += std::size_t(at) * at;
    if (!levels)
        return;

    auto found = m_textures.find(address);
    if (found == m_textures.end())
    {
        Texture texture = { m_nextTexture++, 0, 0, 0, 0, false, false };
        found = m_textures.emplace(address, texture).first;
    }
    Texture& texture = found->second;
    /* Triangles queued with this texture are drawn before it changes: they
     * were the old picture's on the Voodoo, rasterised as they came. */
    if (texture.useStamp == m_renderStamp && m_vertexCount)
        renderPending();
    // The chroma key's comparison (keyed() in the shader) by format.
    const int kind = format == TF_RGB_565 ? 1 : format == TF_ARGB_1555 ? 2 : format == TF_ARGB_4444 ? 3 : 0;
    const bool fresh = texture.size != size || texture.levels != levels || texture.format != kind;
    texture.size = size;
    texture.levels = levels;
    texture.format = kind;
    if (texture.index == m_texture)
    {
        m_textureLevels = levels;
        m_textureFormat = kind;
    }

    std::vector<x86::reg8> copied(static_cast<const x86::reg8*>(data),
                                  static_cast<const x86::reg8*>(data) + texels * texelBytes);
    texture.shadow = false;
    texture.light = false;
    texture.wetStreak = false;
    if (format == TF_ARGB_8888 && softenEffects())
    {
        const SoftenedPicture picture = glow16 ? SoftenedPicture::Glow : recognise(copied, size);
        texture.light = picture == SoftenedPicture::Glow;
        texture.wetStreak = picture == SoftenedPicture::Streak;
        if (picture != SoftenedPicture::None && (picture != SoftenedPicture::Glow || lightGlowSetting() > 0.0f))
        {
            soften(copied, size, levels, picture);
            static bool said[5] = {};
            if (!said[int(picture)])
            {
                said[int(picture)] = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[GFX] softened the game's %s",
                            picture == SoftenedPicture::Pool ? "headlights' pool"
                            : picture == SoftenedPicture::Streak ? "lights' streak on a wet road"
                            : picture == SoftenedPicture::Glow ? "lamps' glow" : "car shadow");
            }
            texture.shadow = picture == SoftenedPicture::Shadow;
        }
    }
    /* The game's other picture of a light on a wet road: a 4444 one, drawn
     * added (red under a tail light) and blended (dark under the body) in long
     * strips from each car down to the bottom of the view -- the red and dark
     * smears behind every car in the rain by day (2026-11-17).  Known as glw4
     * is, so native_thrash.cpp leaves it out by day as well. */
    if (format == TF_ARGB_4444 && size == kWetLightSize
        && texelHash(copied.data(), std::size_t(size) * size * texelBytes) == kWetLightHash)
        texture.wetStreak = true;
    if (texture.index == m_texture)
    {
        m_textureShadow = texture.shadow;
        m_textureLight = texture.light;
        m_textureWetStreak = texture.wetStreak;
    }
    if (format == TF_ARGB_1555)
    {
        // A1 R5 G5 B5 to R5 G5 B5 A1: the alpha bit from the top to the bottom.
        for (std::size_t i = 0; i < texels; ++i)
        {
            const x86::reg16 p = x86::reg16(copied[i * 2] | copied[i * 2 + 1] << 8);
            const x86::reg16 q = x86::reg16(p << 1 | p >> 15);
            copied[i * 2] = x86::reg8(q);
            copied[i * 2 + 1] = x86::reg8(q >> 8);
        }
    }
    const x86::reg32 index = texture.index;
    glthread::post(m_renderer, [this, index, size, levels, format, fresh, copied = std::move(copied)]() mutable {
        if (fresh && index < m_glTextures.size() && m_glTextures[index])
        {
            glDeleteTextures(1, &m_glTextures[index]);
            m_glTextures[index] = 0;
        }
        upload(index, size, levels, format, copied);
    });
}

/* The level below one of a texture's, for a texture with alpha: each texel the
 * four above it, their colours weighed by how opaque they are.  A plain average
 * (glGenerateMipmap) takes the colour of the see-through texels too, which the
 * game's art leaves black: a cut-out's smaller levels came out with a dark
 * fringe, and the smoke and sparks with dark specks where their edges passed
 * the alpha test.  Texels as upload() keeps them: 5551 (1555 turned round),
 * 4444 as A R G B, 8888 as B G R A. */
static void decodeTexel(const x86::reg8* p, TextureFormat format, float (&out)[4])
{
    if (format == TF_ARGB_8888)
    {
        out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f;
        return;
    }
    const unsigned v = unsigned(p[0] | p[1] << 8);
    if (format == TF_ARGB_1555)
    {
        out[0] = ((v >> 11) & 31) / 31.f; out[1] = ((v >> 6) & 31) / 31.f;
        out[2] = ((v >> 1) & 31) / 31.f; out[3] = float(v & 1);
    }
    else
    {
        out[3] = ((v >> 12) & 15) / 15.f; out[0] = ((v >> 8) & 15) / 15.f;
        out[1] = ((v >> 4) & 15) / 15.f; out[2] = (v & 15) / 15.f;
    }
}

static void encodeTexel(x86::reg8* p, TextureFormat format, const float (&in)[4])
{
    auto q = [](float f, float most) { return unsigned(SDL_clamp(f, 0.f, 1.f) * most + 0.5f); };
    if (format == TF_ARGB_8888)
    {
        p[2] = x86::reg8(q(in[0], 255)); p[1] = x86::reg8(q(in[1], 255));
        p[0] = x86::reg8(q(in[2], 255)); p[3] = x86::reg8(q(in[3], 255));
        return;
    }
    unsigned v;
    if (format == TF_ARGB_1555)
        v = q(in[0], 31) << 11 | q(in[1], 31) << 6 | q(in[2], 31) << 1 | (in[3] >= 0.5f ? 1u : 0u);
    else
        v = q(in[3], 15) << 12 | q(in[0], 15) << 8 | q(in[1], 15) << 4 | q(in[2], 15);
    p[0] = x86::reg8(v);
    p[1] = x86::reg8(v >> 8);
}

/* Appends to `texels` (level 0 of a size x size texture) every level below it. */
static void appendAlphaLevels(std::vector<x86::reg8>& texels, x86::reg32 size, TextureFormat format,
                              x86::reg32 texelBytes)
{
    std::size_t above = 0;
    for (x86::reg32 at = size; at > 1; at >>= 1)
    {
        const x86::reg32 below = at >> 1;
        const std::size_t start = texels.size();
        texels.resize(start + std::size_t(below) * below * texelBytes);
        for (x86::reg32 y = 0; y < below; ++y)
            for (x86::reg32 x = 0; x < below; ++x)
            {
                float sum[4] = { 0, 0, 0, 0 }, plain[3] = { 0, 0, 0 };
                for (x86::reg32 k = 0; k < 4; ++k)
                {
                    float t[4];
                    const std::size_t i = std::size_t(2 * y + (k >> 1)) * at + 2 * x + (k & 1);
                    decodeTexel(&texels[above + i * texelBytes], format, t);
                    for (int c = 0; c < 3; ++c)
                    {
                        sum[c] += t[c] * t[3];
                        plain[c] += t[c];
                    }
                    sum[3] += t[3];
                }
                float out[4];
                for (int c = 0; c < 3; ++c)
                    out[c] = sum[3] > 0.f ? sum[c] / sum[3] : plain[c] / 4.f;
                out[3] = sum[3] / 4.f;
                encodeTexel(&texels[start + (std::size_t(y) * below + x) * texelBytes], format, out);
            }
        above = start;
    }
}

void ThrashRenderer::upload(x86::reg32 index, x86::reg32 size, x86::reg32 levels, TextureFormat format,
                            std::vector<x86::reg8>& texels)
{
    GLenum internal = GL_RGBA8, layout = GL_RGBA, type = GL_UNSIGNED_BYTE;
    x86::reg32 texelBytes = 2;
    switch (format)
    {
    case TF_RGB_565:
        internal = GL_RGB565;
        layout = GL_RGB;
        type = GL_UNSIGNED_SHORT_5_6_5;
        break;
    case TF_ARGB_1555:
        internal = GL_RGB5_A1;
        type = GL_UNSIGNED_SHORT_5_5_5_1;
        break;
    case TF_ARGB_4444:
        internal = GL_RGBA4;
        type = GL_UNSIGNED_SHORT_4_4_4_4;
        break;
    default:
        texelBytes = 4;
        break;
    }
    if (index >= m_glTextures.size())
        m_glTextures.resize(index + 1, 0);
    // The levels the game left out, made below (see initGl).
    x86::reg32 storedLevels = levels;
    if (m_makeMipmaps && levels == 1)
        for (x86::reg32 at = size; at > 1; at >>= 1)
            ++storedLevels;
    GLuint& name = m_glTextures[index];
    if (!name)
    {
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexStorage2D(GL_TEXTURE_2D, GLsizei(storedLevels), internal, GLsizei(size), GLsizei(size));
        if (format == TF_ARGB_4444)
        {
            // OpenGL reads A R G B as R G B A: each channel from the next one along.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ALPHA);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
        }
        else if (format == TF_ARGB_8888)
        {
            // B, G, R, A in memory: red and blue change places.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, name);
    }
    // Opaque 565 through the GPU; anything with alpha weighed here.
    x86::reg32 sent = levels;
    if (storedLevels > levels && format != TF_RGB_565)
    {
        appendAlphaLevels(texels, size, format, texelBytes);
        sent = storedLevels;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const x86::reg8* level = texels.data();
    for (x86::reg32 at = size, lod = 0; lod < sent; ++lod, at >>= 1)
    {
        glTexSubImage2D(GL_TEXTURE_2D, GLint(lod), 0, 0, GLsizei(at), GLsizei(at), layout, type, level);
        level += std::size_t(at) * at * texelBytes;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (storedLevels > sent)
        glGenerateMipmap(GL_TEXTURE_2D);
}

/* THRASH_treset: every texture but the window's one-texel one given up --
 * after what is queued is drawn, which may still use them. */
void ThrashRenderer::resetTextures()
{
    renderPending();
    std::vector<x86::reg32> released;
    for (auto it = m_textures.begin(); it != m_textures.end();)
    {
        if (it->first == 0)
        {
            ++it;
            continue;
        }
        released.push_back(it->second.index);
        it = m_textures.erase(it);
    }
    glthread::post(m_renderer, [this, released]() {
        for (const x86::reg32 index : released)
            if (index < m_glTextures.size() && m_glTextures[index])
            {
                glDeleteTextures(1, &m_glTextures[index]);
                m_glTextures[index] = 0;
            }
    });
}

/* Far wrapped texture coordinates brought back by whole repeats, as
 * GlideRenderer does (texCoordRebase there; NFS_TEXCOORD_REBASE=0 too). */
static bool rebaseOn()
{
    static const bool on = []() {
        const char* value = SDL_getenv("NFS_TEXCOORD_REBASE");
        return !value || SDL_strcmp(value, "0") != 0;
    }();
    return on;
}

static void rebase(float coord[3], const float oow[3])
{
    const float limit = 1024.f;
    if (!(std::fabs(coord[0]) > limit * oow[0] || std::fabs(coord[1]) > limit * oow[1]
          || std::fabs(coord[2]) > limit * oow[2]))
        return;
    if (!(oow[0] > 0.f && oow[1] > 0.f && oow[2] > 0.f))
        return;
    const double lowest = std::min(std::min(double(coord[0]) / oow[0], double(coord[1]) / oow[1]),
                                   double(coord[2]) / oow[2]);
    const double shift = std::floor(lowest / 256.0) * 256.0;
    for (int i = 0; i < 3; ++i)
        coord[i] = float(double(coord[i]) - shift * double(oow[i]));
}

void ThrashRenderer::drawTriangle(const GrVertex* a, const GrVertex* b, const GrVertex* c)
{
    auto corner = [](const GrVertex* in) {
        return Corner{ in->x, in->y, in->ooz, in->oow, in->tmuvtx[0].sow, in->tmuvtx[0].tow,
                       x86::reg32(x86::reg8(in->r)) | x86::reg32(x86::reg8(in->g)) << 8
                       | x86::reg32(x86::reg8(in->b)) << 16 | x86::reg32(x86::reg8(in->a)) << 24 };
    };
    drawCorners(corner(a), corner(b), corner(c));
}

/* The native THRASH driver's triangles come here without a GrVertex between
 * the game's vertices and this buffer (glide2x::direct::thrashRenderer). */
void ThrashRenderer::drawCorners(const Corner& a, const Corner& b, const Corner& c)
{
    ThrashVertex* out = reserveTriangle();
    const Corner* const corners[3] = { &a, &b, &c };
    for (int i = 0; i < 3; ++i)
    {
        const Corner* in = corners[i];
        out[i].x = in->x;
        out[i].y = in->y;
        out[i].z = in->ooz;
        out[i].color = in->color;
        out[i].u = in->sow;
        out[i].v = in->tow;
        out[i].oow = in->oow;
        out[i].combine[2] = 0;
    }
    finishTriangle(out);
}

void ThrashRenderer::finishTriangle(ThrashVertex* out)
{
    const bool fogged = (m_fogMode & 0x3) == 0x2;
    const x86::reg8 combine[4] = { x86::reg8(m_colorFactor != 0.f), x86::reg8(m_alphaFactor != 0.f), 0,
                                   x86::reg8(fogged) };
    if ((m_wrapS || m_wrapT) && rebaseOn())
    {
        const float oow[3] = { out[0].oow, out[1].oow, out[2].oow };
        if (m_wrapS)
        {
            float sow[3] = { out[0].u, out[1].u, out[2].u };
            rebase(sow, oow);
            for (int i = 0; i < 3; ++i)
                out[i].u = sow[i];
        }
        if (m_wrapT)
        {
            float tow[3] = { out[0].v, out[1].v, out[2].v };
            rebase(tow, oow);
            for (int i = 0; i < 3; ++i)
                out[i].v = tow[i];
        }
    }
    for (int i = 0; i < 3; ++i)
    {
        const x86::reg8 facing = out[i].combine[2];  // the car's paint (thrashBatchVertex); 0 for the rest
        std::memcpy(out[i].combine, combine, sizeof combine);
        out[i].combine[2] = facing;
    }
    m_vertexCount += 3;
    GlideCounters& counters = GlideRenderer::counters();
    ++counters.triangles;
    if (fogged)
        ++counters.fogged;
}

}
