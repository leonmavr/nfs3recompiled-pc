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
"    return all(equal(bytes, u_chromaKeyBytes));"
"}"
"vec4 texel(vec2 at, float size, int level)"
"{"
"    vec2 t = mix(clamp(at, 0.0, size - 1.0), mod(at, size), u_wrap);"
"    return texelFetch(u_texture, ivec2(t), level);"
"}"
/* A texture seen larger than it is (initGl, NFS_TEXTURE_SHARP): Catmull-Rom
 * over the sixteen texels around the point, in nine bilinear reads, instead
 * of the GPU's bilinear over four -- the track's 64 x 64 pictures spread over
 * a wall or a hillside keep their edges instead of melting into a cross-hatched
 * blur -- in the world only (what is fogged): the menus' lettering, cut out
 * of its own small pictures, came out ringed and smeared.  Where a texel is a
 * pixel or smaller the GPU's own filtering (the
 * levels, trilinear, anisotropic) is the better one and is what is used; the
 * two are blended across the step, so no line shows where one gives way. */
"vec4 sharpTexture(vec2 uv, vec2 gx, vec2 gy)"
"{"
"    vec4 plain = textureGrad(u_texture, uv, gx, gy);"
"    vec2 size = vec2(textureSize(u_texture, 0));"
"    float rho = max(length(gx * size), length(gy * size));"
"    if (rho >= 1.0) return plain;"
"    vec2 p = uv * size;"
"    vec2 t1 = floor(p - 0.5) + 0.5;"
"    vec2 f = p - t1;"
"    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));"
"    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);"
"    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));"
"    vec2 w3 = f * f * (-0.5 + 0.5 * f);"
"    vec2 w12 = w1 + w2;"
"    vec2 a = (t1 - 1.0) / size, b = (t1 + w2 / w12) / size, c = (t1 + 2.0) / size;"
"    vec4 sum = textureLod(u_texture, vec2(a.x, a.y), 0.0) * w0.x * w0.y"
"             + textureLod(u_texture, vec2(b.x, a.y), 0.0) * w12.x * w0.y"
"             + textureLod(u_texture, vec2(c.x, a.y), 0.0) * w3.x * w0.y"
"             + textureLod(u_texture, vec2(a.x, b.y), 0.0) * w0.x * w12.y"
"             + textureLod(u_texture, vec2(b.x, b.y), 0.0) * w12.x * w12.y"
"             + textureLod(u_texture, vec2(c.x, b.y), 0.0) * w3.x * w12.y"
"             + textureLod(u_texture, vec2(a.x, c.y), 0.0) * w0.x * w3.y"
"             + textureLod(u_texture, vec2(b.x, c.y), 0.0) * w12.x * w3.y"
"             + textureLod(u_texture, vec2(c.x, c.y), 0.0) * w3.x * w3.y;"
"    return mix(plain, clamp(sum, 0.0, 1.0), smoothstep(1.0, 0.6, rho));"
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
"        float cone = smoothstep(0.82, 0.97, dot(-toLight, u_headDir));"
"        float reach = 1.0 / (1.0 + dist * dist / 400.0) * (1.0 - smoothstep(45.0, 70.0, dist));"
"        vec3 n = cross(dFdx(P), dFdy(P));"
"        vec3 nn = dot(n, n) > 0.0 ? normalize(n) : -toLight;"
"        float facing = abs(dot(nn, toLight));"
/* What lies level -- the road, which has the game's own patch of light, and
 * the ground -- gets a quarter of it; what stands up gets all of it. */
"        float level = smoothstep(0.6, 0.9, abs(dot(nn, u_headUp)));"
"        float lit = u_headlight * cone * reach * (0.3 + 0.7 * facing) * mix(1.0, 0.25, level);"
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
"                    color.rgb += seen * fresnel * wet * (0.35 + 0.65 * puddle);"
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
    float                               sun[11];
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
        for (unsigned int texture : { m_sky, m_postCopy, m_bright, m_postDepth })
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
    m_batches.push_back(batch);
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
    std::copy(m_sun, m_sun + 11, draw->sun);
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
"float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }"
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
"void main()"
"{"
"    vec3 c = u_aa ? fxaa(v_uv) : texture(u_source, v_uv).rgb;"
"    if (u_blur > 0.0)"
"        c = motionBlur(v_uv, c);"
"    if (u_shadow > 0.0)"
"        c *= 1.0 - u_shadow * sunShadow(v_uv);"
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

/* A grain (0): a clump with a ragged edge -- its outline a few waves round it,
 * turned by its seed -- lit a little from above.  A puff (1): a cloud of fine
 * dust, soft and uneven, faded where it meets what is behind it so it never
 * shows a hard line against the ground.  Both are the colour of the ground they
 * were thrown from (round v_from, the picture there -- the world's pixels
 * only, a car's being 0 in the alpha, and the car that threw them standing
 * over the very spot; or every pixel there, where too few are the world's),
 * a grain darker and a puff paler.  A spark (2): a streak from where it was to
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
"float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }"
"float noise(vec2 p)"
"{"
"    vec2 i = floor(p), f = fract(p);"
"    vec2 u = f * f * (3.0 - 2.0 * f);"
"    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);"
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
"    float behind = texture(u_depth, gl_FragCoord.xy / u_size).r;"
"    float zBehind = 1.0 / max(1.0 - behind, 1e-5);"
"    float room = zBehind - z;"
"    if (room < (type == 4 ? -0.4 : -0.05)) discard;"
"    float r = length(v_uv);"
"    if (type == 2) {"
"        float across = exp(-v_uv.y * v_uv.y * 5.0);"
"        float along = clamp(v_uv.x * 0.5 + 0.5, 0.0, 1.0);"
"        float a = across * mix(0.15, 1.0, along * along) * v_color.a;"
"        vec3 hot = mix(v_color.rgb, vec3(1.0, 0.97, 0.85), across * along * along * 0.6);"
"        o_color = vec4(hot, a);"
"        return;"
"    }"
"    if (type == 4) {"
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
"    if (type == 0 || type == 3) {"
"        vec3 g = ground(v_color.rgb);"
/* What comes off grass (3) is blades: long and thin, tapering, a little bent,
 * lying as they tumble (v_params.w), a greener and brighter shade of the
 * ground -- where the ground is green; off snow or earth, grains. */
"        if (type == 3 && g.g > g.r * 1.02 && g.g > g.b * 0.9) {"
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

static GLuint postProgram(GLuint (*compile)(GLenum, const std::string&, const char*), const char* fragmentBody)
{
    const GLuint vertex = compile(GL_VERTEX_SHADER, "", g_postVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, "", fragmentBody);
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

static float lightGlowSetting();
static float lightTrailSetting();
static float smoothStep(float edge0, float edge1, float x);

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
            const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, "", g_particleShader);
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
        m_compositeProgram = postProgram(compileShader, g_compositeShader);
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
void ThrashRenderer::drawParticles(const ThrashDraw& draw, int width, int height)
{
    struct Corner
    {
        float x, y, u, v, r, g, b, a, fx, fy, depth, seed, type, turn;
    };
    const std::vector<EffectParticle>& list = *draw.particles;
    std::vector<Corner> corners;
    corners.reserve(list.size() * 6);
    std::size_t blended = 0;
    for (const EffectParticle& p : list)
    {
        float ax, ay, bx, by;   // the quad's two axes, half its length and width
        if (p.type == EffectParticle::Spark)
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
            by = p.size;
            ++blended;
        }
        const float cx = p.type == EffectParticle::Spark ? 0.5f * (p.x + p.tx) : p.x;
        const float cy = p.type == EffectParticle::Spark ? 0.5f * (p.y + p.ty) : p.y;
        const float fromX = p.type == EffectParticle::Spark ? -1.0f : p.tx;
        auto corner = [&](float s, float t) {
            corners.push_back({ cx + ax * s + bx * t, cy + ay * s + by * t, s, t, p.r, p.g, p.b, p.a, fromX, p.ty,
                                p.depth, p.seed, p.type, p.turn });
        };
        corner(-1, -1); corner(1, -1); corner(1, 1);
        corner(-1, -1); corner(1, 1); corner(-1, 1);
    }
    glBindVertexArray(m_particleArray);
    glBindBuffer(GL_ARRAY_BUFFER, m_particleBuffer);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(corners.size() * sizeof(Corner)), corners.data(), GL_STREAM_DRAW);
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
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_postDepth);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_postCopy);
    glViewport(0, 0, width, height);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (blended)
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(blended * 6));
    if (corners.size() > blended * 6)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDrawArrays(GL_TRIANGLES, GLsizei(blended * 6), GLsizei(corners.size() - blended * 6));
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

void ThrashRenderer::postProcess(const ThrashDraw& draw)
{
    m_postDone = true;
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
    const bool needDepth = shadows || blur > 0.0f || particles;
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
        // Over the world, then the picture taken again with them in it.
        drawParticles(draw, width, height);
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
    if (m_bloom > 0.0f)
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
    glUniform1f(m_compositeBloom, m_bloom);
    glUniform3f(m_compositeGrade, draw.grade[3], draw.grade[4], m_gradeOn ? 1.0f : 0.0f);
    glUniform3f(m_compositeTint, draw.grade[0], draw.grade[1], draw.grade[2]);
    glUniform1i(m_compositeAa, m_antialias ? 1 : 0);
    glUniform1f(m_compositeShadow, shadows ? draw.sun[0] : 0.0f);
    glUniform1f(m_compositeBlur, blur);
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
    glUniform1f(m_compositeHalo, lights ? m_lightGlow * 0.8f : 0.0f);
    glUniform1f(m_compositeTrailOn, trails);
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
                      && (m_bloom > 0.0f || m_gradeOn || m_antialias || draw.sun[0] > 0.0f || m_motionBlur > 0.0f
                          || m_lightGlow > 0.0f || m_lightTrails > 0.0f);
    const float haze = draw.race ? m_haze : 0.0f;
    const bool wet = draw.wetness > 0.0f && m_reflectionValid && m_reflectionW == int(draw.width)
                     && m_reflectionH == int(draw.height);
    glUniform1f(m_locations.wet, wet ? draw.wetness : 0.0f);
    if (wet)
    {
        glUniform2f(m_locations.screen, float(draw.width), float(draw.height));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_reflection);
        glActiveTexture(GL_TEXTURE0);
    }
    for (const Batch& batch : draw.batches)
    {
        /* The world drawn, the HUD to come: its first draw is unfogged and
         * writes no depth, after the world's fogged ones.  That is the picture the
         * next frame's wet road reflects, without the HUD in it. */
        /* And (the port's haze) the sky as it stands before the world's first
         * draw; and (its whole-picture effects) the world, once, as the HUD
         * begins. */
        if (wet || draw.wetness > 0.0f || haze > 0.0f || post || headlight > 0.0f)
        {
            const ThrashVertex& corner = draw.vertices[first];
            const bool flat = corner.combine[3] == 0 && !batch.zBuffer;
            if (!flat)
            {
                if (!m_sceneDrawn && !m_skyTaken && haze > 0.0f)
                    takeSky(int(draw.width), int(draw.height));
                m_sceneDrawn = true;
            }
            else if (m_sceneDrawn)
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
                                   && batch.alphaBlend ? haze : 0.0f;
            if (hazeHere != hazeSet)
            {
                glUniform1f(m_locations.haze, hazeHere);
                hazeSet = hazeHere;
            }
        }
        if (m_detailTexture && (!previous || batch.detail != previous->detail))
            glUniform1f(m_locations.detail, batch.detail ? m_detailStrength : 0.0f);
        if (m_detailTexture && (!previous || batch.surface != previous->surface))
            glUniform1f(m_locations.surface, batch.surface ? m_surfaceStrength : 0.0f);
        if (!previous || batch.headlit != previous->headlit)
            glUniform1f(m_locations.headlight, batch.headlit ? headlight : 0.0f);
        if (!previous || batch.carShine != previous->carShine)
            glUniform1f(m_locations.carShine, batch.carShine ? m_carShine : 0.0f);
        if (!previous || batch.sharpen != previous->sharpen)
            glUniform1i(m_locations.sharpen, batch.sharpen && m_sharpen ? 1 : 0);
        if (batch.fogTable != fogTable)
        {
            fogTable = batch.fogTable;
            glUniform1fv(m_locations.fogTable, 64, draw.fogTables[fogTable].data());
        }
        if (!previous || batch.texture != previous->texture)
        {
            glBindTexture(GL_TEXTURE_2D, batch.texture < m_glTextures.size() ? m_glTextures[batch.texture] : 0);
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
        if (!previous || batch.zBuffer != previous->zBuffer || batch.decal != previous->decal)
            glDepthMask(batch.zBuffer && !batch.decal ? GL_TRUE : GL_FALSE);
        /* A car's pixels leave 0 in the picture's alpha, everything else adds
         * its own to what is there: the shadows of what stands by the road are
         * not cast by the cars, which have their own (postProcess) -- and a
         * car's soft shadow, drawn over the road, keeps the road's 1 under it
         * (written as its own faint alpha it was taken for a car, and its rim
         * stood out light inside the shadow of a wall). */
        if (!previous || batch.alphaBlend != previous->alphaBlend || batch.car != previous->car
            || batch.decal != previous->decal)
        {
            const GLenum alphaSource = batch.car || batch.decal ? GL_ZERO : GL_ONE;
            const GLenum alphaKept = batch.car ? GL_ZERO : GL_ONE;
            if (batch.alphaBlend)
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, alphaSource, alphaKept);
            else
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, alphaSource, alphaKept);
        }
        if (!previous || batch.alphaBlend != previous->alphaBlend || batch.zBuffer != previous->zBuffer
            || batch.decal != previous->decal)
            glUniform1f(m_locations.depthCut, batch.zBuffer && !batch.decal ? 0.08f : 0.0f);
        if (!previous || batch.chromaKey != previous->chromaKey)
            glUniform1i(m_locations.chromaKey, batch.chromaKey ? 1 : 0);
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

void ThrashRenderer::swap()
{
    m_renderer->ensureFramePacing();
    renderPending();
    GlideCounters& counters = GlideRenderer::counters();
    ++counters.frames;
    counters.depth16Frames += m_depth16;
    m_renderer->paceFrame();
    const bool keepFrame = m_wetness > 0.0f && m_detailTexture;
    const int frameW = int(m_renderer->m_width), frameH = int(m_renderer->m_height);
    glthread::post(m_renderer, [this, keepFrame, frameW, frameH]() {
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
                takeReflection(frameW, frameH);
            }
        }
        else
        {
            m_reflectionValid = false;
        }
        m_reflectionTaken = false;
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
    m_chromaKeyColor = color;
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

static void soften(std::vector<x86::reg8>& texels, x86::reg32 size, x86::reg32 levels, SoftenedPicture picture)
{
    if (picture == SoftenedPicture::Glow)
    {
        softenGlow(texels, size, levels, 0.12f);
        return;
    }
    for (x86::reg32 y = 0; y < size; ++y)
        for (x86::reg32 x = 0; x < size; ++x)
        {
            // -1 to 1 across the picture, at the texels' centres.
            const float px = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float py = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
            x86::reg8* t = &texels[(std::size_t(y) * size + x) * 4];
            if (picture == SoftenedPicture::Shadow)
            {
                // A rounded square's distance from the edge, the alpha
                // falling over the outer third.
                const float corner = 0.45f;
                const float qx = std::max(std::fabs(px) - (1.0f - corner), 0.0f);
                const float qy = std::max(std::fabs(py) - (1.0f - corner), 0.0f);
                const float inside = 1.0f - (std::sqrt(qx * qx + qy * qy) + std::min(std::max(std::fabs(px), std::fabs(py)) - (1.0f - corner), 0.0f)) / corner;
                const float alpha = smoothStep(0.0f, 0.75f, inside);
                t[3] = x86::reg8(std::lround(255.0f * 0.9f * alpha));
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
}

void ThrashRenderer::setTextureData(x86::reg32, x86::reg32 address, const void* data, x86::reg32 largeMipmap,
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
    if (format == TF_ARGB_8888 && softenEffects())
    {
        const SoftenedPicture picture = glow16 ? SoftenedPicture::Glow : recognise(copied, size);
        texture.light = picture == SoftenedPicture::Glow;
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
    if (texture.index == m_texture)
    {
        m_textureShadow = texture.shadow;
        m_textureLight = texture.light;
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
