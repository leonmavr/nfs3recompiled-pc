#include <nfs3hp.h>
#include <lib/overlay.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/* Port: the port's own minimap, in place of the game's (NFS_MINIMAP; TODOs.md,
 * Minimap revamp; TECHNICAL.md, "The minimap").
 *
 * The game's own HUD options drive it: Map display (Off, Stationary,
 * Rotating) and Map Scale (1x to 4x), and the keys that toggle and size the
 * map, all kept in the player's HUD layout (kLayout: kLayoutMapMode,
 * kLayoutMapScale).  NFS_MINIMAP=off keeps the game's own map;
 * NFS_MINIMAP_SCALE multiplies the menu's zoom (1, 2 twice as close).
 *
 * Where.  Into the rectangle the game lays its own map out in each frame:
 * HUD element 17 (22 when the player drives a cop) of the layout's table of
 * pixel rectangles (0x749758, 23 of four ints a player; sub_480910), so the
 * HUD's layout and size options move and scale it as they do the game's.  The
 * game's own (sub_48d4c0) is then not called (hudNative's map step).
 *
 * What.  A translucent panel of the HUD's navy with faint lighter grid lines,
 * fixed to the world so they scroll with it; the road, from the track's nodes
 * ([0x5dd948], 0x24 bytes each, 16.16 places), light grey between two darker
 * outlines, lighter where the track is higher (each node's height against the
 * track's lowest and highest); the cars, each a prism (the sprite atlas,
 * tools/generate_minimap_art.py) pointing the way it faces, in its driver's
 * colour -- the colour the game gives its dot ([0x793098 + 4 x car[+0x1f0]],
 * 0x..RRGGBB) -- the player's 1.25 times the others' and drawn last; and a
 * metal corner piece at each of the four corners.
 *
 * Which cars, as the game's map has them: the cops of 0x5efa88 ([0x5efd88]
 * of them) chasing someone ([+0x624]), flashing red and blue by the race's
 * clock and their number as the game's dots do; the racers of 0x5efc48
 * ([0x5efda0]) but the player, less those the game leaves off; the player's
 * own (0x5efac8 by [0x6fd4f4]).
 *
 * The view: the player in the middle.  Stationary, the map is fixed (the
 * world's +z up, +x right) and scrolls under the player's prism, which turns
 * with the car; rotating, the map turns under the player so the way the car
 * faces is always up (the heading eased over a few frames), the player's
 * prism pointing straight up.  kRange / (Map Scale x NFS_MINIMAP_SCALE)
 * metres from the middle to the top and bottom edges.
 *
 * Entered from the top of the game's map, sub_48d4c0 (native_render.cpp's
 * `minimap`, tools/apply_native_render.py), whichever way the HUD is drawn.
 * Drawn as a layer of the port's overlay (lib/overlay.h, OverlayMinimap) set
 * for one frame at a time: a frame whose HUD has no map step (paused, a
 * replay with the HUD off) shows none.  Split screen, and a build without the
 * sprites (Android, for now), keep the game's own map.
 *
 * NFS_MINIMAP_TRACE=1: once a second, where the map is and how the track's
 * nodes lie against the player's car (minimapTrace). */
namespace nfs3hp
{

namespace
{

namespace minimap
{

// The game's.
constexpr x86::reg32 kNodes = 0x5dd948;          // the track's nodes
constexpr x86::reg32 kNodeCount = 0x5dd940;
constexpr x86::reg32 kNodeSize = 0x24;
constexpr x86::reg32 kNodeStepX = 0x14;          // s8: sar 0x18 of the dword at +0x11
constexpr x86::reg32 kNodeStepZ = 0x16;
constexpr float kFixed = 65536.0f;               // 16.16
constexpr float kStepUnit = 512.0f / kFixed;     // the game's map: a step byte << 9
constexpr x86::reg32 kPlayerIndex = 0x6fd4f4;
constexpr x86::reg32 kPlayerCars = 0x5efa48;     // a car per player
constexpr x86::reg32 kMapPlayerCars = 0x5efac8;  // the map's: the player's car by [kPlayerIndex]
constexpr x86::reg32 kCops = 0x5efa88, kCopCount = 0x5efd88;
constexpr x86::reg32 kRacers = 0x5efc48, kRacerCount = 0x5efda0;
constexpr x86::reg32 kMaxCars = 16;
constexpr x86::reg32 kCarPos = 0x98;             // floats x, y, z
constexpr x86::reg32 kCarForward = 0xd8;
constexpr x86::reg32 kCarNode = 0x1c;
constexpr x86::reg32 kCarFlags = 0x200;          // & 0x20: a cop's
constexpr x86::reg8 kCopBit = 0x20;
constexpr x86::reg8 kFlagsBit0 = 0x01;           // with [+0x2c4] == 4: left off the map (as the game)
constexpr x86::reg32 kCarState = 0x2c4;
constexpr x86::reg32 kCarChasing = 0x624;        // a cop's: nonzero while chasing
constexpr x86::reg32 kCarNumber = 0x1f0;         // its place in the colour table
constexpr x86::reg32 kCarRecord = 0x220;         // its race record; +0x14 the car's id
constexpr x86::reg32 kRecordId = 0x14;
constexpr x86::reg32 kHiddenId = 0x6fdc30;       // a car of this id is left off (unless the player's is it)
constexpr x86::reg32 kColours = 0x793098;        // a driver's dot colour, 0xAARRGGBB, by car[kCarNumber]
constexpr std::uint32_t kCopRed = 0xf80000, kCopBlue = 0x0000f8;
constexpr x86::reg32 kRaceClock = 0x7d3684;      // 64 a second
constexpr int kCopFlashShift = 5;                // the clock >> 5: every half second
constexpr x86::reg32 kHudRects = 0x749758;       // 23 rects a player, 16 bytes each
constexpr x86::reg32 kHudRectsPerPlayer = 23;
constexpr x86::reg32 kMapElement = 0x11;
constexpr x86::reg32 kCopMapElement = 0x16;
constexpr x86::reg32 kGameMapRect = 0x7925d8;    // floats, 16 bytes a player
constexpr x86::reg32 kGameMapCentreX = 0x792888; // a float a player
constexpr x86::reg32 kGameMapCentreY = 0x792890;
constexpr x86::reg32 kGameMapRadius = 0x792878;
constexpr x86::reg32 kLayout = 0x6fbc60;         // + view * 0x348 + cop * 0x1a4
constexpr x86::reg32 kLayoutView = 0x348, kLayoutCop = 0x1a4;
constexpr x86::reg32 kLayoutMapScale = 0x0;      // float: Map Scale, 1 to 4 (1x to 4x)
constexpr x86::reg32 kLayoutMapMode = 0x10;      // Map display: 0 off (no map step at all), 1 rotating, else stationary
constexpr x86::reg32 kMapRotating = 1;
constexpr float kMenuScaleLeast = 0.5f, kMenuScaleMost = 8.0f;   // past these the field is not a scale
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;    // 1: split screen
constexpr x86::reg32 kScreenHeight = 0x7cdae4;   // the screen mode's, the rectangles' pixels
constexpr Uint64 kTraceEveryMs = 1000;

// The sprites (tools/generate_minimap_art.py: TILE, the prism's corners).
constexpr const char* kAtlasFile = "assets/minimap/minimap-atlas.png";
constexpr int kAtlasW = 256, kAtlasH = 128;
constexpr float kTile = 128.0f;
constexpr float kPrismU0 = 0.0f, kPrismU1 = 0.5f;     // tile 0
constexpr float kCornerU0 = 0.5f, kCornerU1 = 1.0f;   // tile 1
constexpr float kPrismCentreY = (4.0f + 118.0f + 118.0f) / 3.0f;   // the triangle's centroid, tile pixels

// The port's look, as fractions of the map's smaller side unless said.
constexpr float kRange = 400.0f;          // metres, middle to top edge, at Map Scale 1x
constexpr float kScaleLeast = 0.1f, kScaleMost = 5.0f;   // NFS_MINIMAP_SCALE's bounds
constexpr float kHeadingEase = 0.12f;     // rotating: seconds the heading takes to follow the car's
constexpr float kRoadWidth = 1.0f / 12.0f;
constexpr float kRoadOutline = 0.020f;    // each side
constexpr float kRoadLow[3] = { 0x64 / 255.0f, 0x64 / 255.0f, 0x64 / 255.0f };    // its darkest, #646464
constexpr float kRoadHigh[3] = { 0xcc / 255.0f, 0xca / 255.0f, 0xca / 255.0f };   // its lightest, #cccaca
constexpr bool kRoadBySlope = true;       // its colour by the road's slope (else by the node's height)
constexpr float kSlopeMostDeg = 30.0f;    // downhill this steep or more darkest, uphill lightest
constexpr float kOutlineGrey = 0.30f;
constexpr float kRoadAlpha = 0.95f;
constexpr float kPanel[3] = { 0.16f, 0.13f, 0.30f };   // the HUD's navy (native_now_playing's kFill)
constexpr float kPanelAlpha = 0.62f;
constexpr float kGridMetres = 50.0f;      // the grid's spacing, world metres
constexpr float kGridLine = 0.006f;       // its lines' width
constexpr float kGrid[3] = { 0.55f, 0.55f, 0.80f };
constexpr float kGridAlpha = 0.35f;
constexpr float kPrism = 0.15f;           // a car's sprite, its tile's side
constexpr float kPlayerScale = 1.35f;
constexpr float kCorner = 0.17f;          // a corner piece's side
constexpr float kCornerGrey = 0.5f;       // drawn as baked
constexpr float kCarAlpha = 1.0f;
constexpr float kReachMargin = 1.5f;      // nodes this far past the edge, in ranges, still laid
constexpr float kMiterMost = 2.0f;        // a joint's widening, at most
constexpr float kLoopGap = 4.0f;          // a track closes when its ends are this many steps apart
constexpr float kBucket = 64.0f;          // the nodes' index: metres a cell
/* The finish line: a chequered band across the road at the track's first node
 * (its last, point to point), as wide as the road and kFinishRows squares deep
 * along it, so it scales with the map as the road does. */
constexpr int kFinishRows = 4;            // along the road
constexpr int kFinishColumns = 6;         // across it
constexpr float kFinishDark[3] = { 0x30 / 255.0f, 0x30 / 255.0f, 0x30 / 255.0f };    // #303030
constexpr float kFinishLight[3] = { 1.0f, 1.0f, 1.0f };                             // #ffffff
constexpr float kFinishAlpha = 1.0f;
const float kNoClip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

bool tracing()
{
    static const bool on = [] {
        const char* v = SDL_getenv("NFS_MINIMAP_TRACE");
        return v && *v && *v != '0';
    }();
    return on;
}

float nodeAt(win32::WinApplication* app, x86::reg32 node, int axis)
{
    const x86::reg32 base = app->getMemory<x86::reg32>(kNodes);
    return float(x86::sreg32(app->getMemory<x86::reg32>(base + node * kNodeSize + x86::reg32(axis) * 4))) / kFixed;
}

void traceTrack(win32::WinApplication* app, x86::sreg32 count)
{
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    double stepSum = 0.0, byteErr = 0.0;
    int matched = 0;
    const x86::reg32 base = app->getMemory<x86::reg32>(kNodes);
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        for (int a = 0; a < 3; ++a)
        {
            const float v = nodeAt(app, x86::reg32(i), a);
            lo[a] = SDL_min(lo[a], v);
            hi[a] = SDL_max(hi[a], v);
        }
        if (i + 1 < count)
        {
            const float dx = nodeAt(app, x86::reg32(i + 1), 0) - nodeAt(app, x86::reg32(i), 0);
            const float dz = nodeAt(app, x86::reg32(i + 1), 2) - nodeAt(app, x86::reg32(i), 2);
            stepSum += std::sqrt(dx * dx + dz * dz);
            const x86::reg32 rec = base + x86::reg32(i) * kNodeSize;
            const float bx = float(x86::sreg32(std::int8_t(app->getMemory<x86::reg8>(rec + kNodeStepX)))) * kStepUnit;
            const float bz = float(x86::sreg32(std::int8_t(app->getMemory<x86::reg8>(rec + kNodeStepZ)))) * kStepUnit;
            byteErr += std::sqrt((bx - dx) * (bx - dx) + (bz - dz) * (bz - dz));
            ++matched;
        }
    }
    const x86::reg32 r0 = base;
    SDL_Log("[MINIMAP] track: %d nodes, x %.0f..%.0f, y (height) %.1f..%.1f, z %.0f..%.0f m; mean step %.2f m; "
            "step bytes vs places: mean miss %.2f m; node 0 bytes +0x10..+0x17: %02x %02x %02x %02x %02x %02x %02x %02x",
            int(count), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], matched ? stepSum / matched : 0.0,
            matched ? byteErr / matched : 0.0,
            unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x10))), unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x11))),
            unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x12))), unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x13))),
            unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x14))), unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x15))),
            unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x16))), unsigned(x86::reg8(app->getMemory<x86::reg8>(r0 + 0x17))));
}

/* NFS_MINIMAP: on (the default: the port's, as the HUD options say) or off
 * (0): the game's own map. */
bool enabled()
{
    static const bool on = [] {
        const char* v = SDL_getenv("NFS_MINIMAP");
        return !(v && (SDL_strcmp(v, "0") == 0 || SDL_strcasecmp(v, "off") == 0));
    }();
    return on;
}

/* NFS_MINIMAP_SCALE: a zoom on top of the menu's Map Scale, 1 the default,
 * 2 twice as close, 0.5 twice as far; kScaleLeast to kScaleMost. */
float extraZoom()
{
    static const float zoom = [] {
        const char* v = SDL_getenv("NFS_MINIMAP_SCALE");
        const float parsed = v && *v ? float(SDL_atof(v)) : 1.0f;
        return parsed >= kScaleLeast && parsed <= kScaleMost ? parsed : 1.0f;
    }();
    return zoom;
}

/* The sprites, read once beside the executable: premultiplied RGBA. */
std::shared_ptr<const OverlayArt> art()
{
    static std::shared_ptr<const OverlayArt> s_art;
    static bool s_tried = false;
    if (s_tried)
        return s_art;
    s_tried = true;
    const char* base = SDL_GetBasePath();
    const std::string path = std::string(base ? base : "") + kAtlasFile;
    SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
    if (!loaded)
    {
        SDL_Log("[MINIMAP] cannot load %s (%s): the game's own map is kept", path.c_str(), SDL_GetError());
        return s_art;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba || rgba->w != kAtlasW || rgba->h != kAtlasH)
    {
        SDL_Log("[MINIMAP] %s is not %dx%d RGBA: the game's own map is kept", path.c_str(), kAtlasW, kAtlasH);
        if (rgba)
            SDL_DestroySurface(rgba);
        return s_art;
    }
    auto made = std::make_shared<OverlayArt>();
    made->mapW = kAtlasW;
    made->mapH = kAtlasH;
    made->map.resize(std::size_t(kAtlasW) * kAtlasH * 4);
    for (int y = 0; y < kAtlasH; ++y)
        std::memcpy(made->map.data() + std::size_t(y) * kAtlasW * 4,
                    static_cast<const std::uint8_t*>(rgba->pixels) + std::size_t(y) * rgba->pitch, kAtlasW * 4);
    SDL_DestroySurface(rgba);
    for (std::size_t i = 0; i < made->map.size(); i += 4)
        for (std::size_t c = 0; c < 3; ++c)
            made->map[i + c] = std::uint8_t((unsigned(made->map[i + c]) * made->map[i + 3] + 127) / 255);
    s_art = made;
    SDL_Log("[MINIMAP] sprites loaded from %s", path.c_str());
    return s_art;
}

/* The track as the map wants it: each node's ground place and height, the
 * heights' range, whether it closes on itself.  Made again when the game's
 * nodes change (another track). */
struct Track
{
    x86::reg32 nodes = 0;
    x86::sreg32 count = 0;
    std::vector<float> x, y, z;
    std::vector<float> shade;               // each node's colour, three floats, worked out once (kRoadBySlope)
    float low = 0.0f, high = 1.0f;
    bool loop = false;
    // the index: the nodes by kBucket-metre cell of the ground, cells x-major
    float originX = 0.0f, originZ = 0.0f;
    int cellsX = 0, cellsZ = 0;
    std::vector<std::uint32_t> cellStart;   // cellsX * cellsZ + 1 offsets into cellNodes
    std::vector<std::uint32_t> cellNodes;
};

const Track& track(win32::WinApplication* app)
{
    static Track t;
    const x86::reg32 nodes = app->getMemory<x86::reg32>(kNodes);
    const x86::sreg32 count = x86::sreg32(app->getMemory<x86::reg32>(kNodeCount));
    if (nodes == t.nodes && count == t.count)
        return t;
    t = Track();
    t.nodes = nodes;
    t.count = count;
    if (!nodes || count <= 1)
        return t;
    t.x.resize(std::size_t(count));
    t.y.resize(std::size_t(count));
    t.z.resize(std::size_t(count));
    t.low = 1e30f;
    t.high = -1e30f;
    double steps = 0.0;
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        const std::size_t k = std::size_t(i);
        t.x[k] = nodeAt(app, x86::reg32(i), 0);
        t.y[k] = nodeAt(app, x86::reg32(i), 1);
        t.z[k] = nodeAt(app, x86::reg32(i), 2);
        t.low = SDL_min(t.low, t.y[k]);
        t.high = SDL_max(t.high, t.y[k]);
        if (i)
            steps += std::hypot(t.x[k] - t.x[k - 1], t.z[k] - t.z[k - 1]);
    }
    // the index: count a cell's nodes, then lay them out cell after cell
    float loX = 1e30f, hiX = -1e30f, loZ = 1e30f, hiZ = -1e30f;
    for (std::size_t k = 0; k < t.x.size(); ++k)
    {
        loX = SDL_min(loX, t.x[k]);
        hiX = SDL_max(hiX, t.x[k]);
        loZ = SDL_min(loZ, t.z[k]);
        hiZ = SDL_max(hiZ, t.z[k]);
    }
    t.originX = loX;
    t.originZ = loZ;
    t.cellsX = int((hiX - loX) / kBucket) + 1;
    t.cellsZ = int((hiZ - loZ) / kBucket) + 1;
    auto cellOf = [&](std::size_t k) {
        return std::size_t(int((t.x[k] - loX) / kBucket)) * std::size_t(t.cellsZ) + std::size_t(int((t.z[k] - loZ) / kBucket));
    };
    t.cellStart.assign(std::size_t(t.cellsX) * std::size_t(t.cellsZ) + 1, 0);
    for (std::size_t k = 0; k < t.x.size(); ++k)
        ++t.cellStart[cellOf(k) + 1];
    for (std::size_t c = 1; c < t.cellStart.size(); ++c)
        t.cellStart[c] += t.cellStart[c - 1];
    t.cellNodes.resize(t.x.size());
    {
        std::vector<std::uint32_t> fill(t.cellStart.begin(), t.cellStart.end() - 1);
        for (std::size_t k = 0; k < t.x.size(); ++k)
            t.cellNodes[fill[cellOf(k)]++] = std::uint32_t(k);
    }
    const float step = float(steps / double(count - 1));
    const float gap = std::hypot(t.x.back() - t.x.front(), t.z.back() - t.z.front());
    t.loop = step > 0.0f && gap < step * kLoopGap;
    if (!(t.high > t.low))
        t.high = t.low + 1.0f;
    /* Each node's grey, once: by the slope through it -- the height gained
     * from the node before to the node after over the ground between them,
     * the way the track runs (uphill lighter), -kSlopeMostDeg to
     * kSlopeMostDeg darkest to lightest -- or by its height between the
     * track's lowest and highest. */
    t.shade.resize(std::size_t(count) * 3);
    const float most = std::tan(kSlopeMostDeg * 3.14159265f / 180.0f);
    for (std::size_t k = 0; k < std::size_t(count); ++k)
    {
        float h;
        if (kRoadBySlope)
        {
            const std::size_t last = std::size_t(count) - 1;
            const std::size_t a = k > 0 ? k - 1 : (t.loop ? last : k);
            const std::size_t c = k < last ? k + 1 : (t.loop ? 0 : k);
            const float ground = std::hypot(t.x[c] - t.x[a], t.z[c] - t.z[a]);
            const float rise = ground > 0.01f ? (t.y[c] - t.y[a]) / ground : 0.0f;   // tan of the slope
            h = 0.5f + 0.5f * SDL_clamp(rise / most, -1.0f, 1.0f);
        }
        else
            h = (t.y[k] - t.low) / (t.high - t.low);
        h = SDL_clamp(h, 0.0f, 1.0f);
        for (std::size_t c = 0; c < 3; ++c)
            t.shade[k * 3 + c] = kRoadLow[c] + (kRoadHigh[c] - kRoadLow[c]) * h;
    }
    SDL_Log("[MINIMAP] track: %d nodes, heights %.1f to %.1f m, %s", int(count), double(t.low), double(t.high),
            t.loop ? "a circuit" : "point to point");
    return t;
}

/* The overlay being made: triangles, and helpers for quads. */
struct Builder
{
    Overlay& out;

    void vertex(float x, float y, float u, float v, const float rgb[3], float a, int kind)
    {
        out.vertices.push_back({ x, y, u, v, rgb[0], rgb[1], rgb[2], a, kind });
    }
    /* A quad by its corners in order round it, each with its uv. */
    void quad(const float (&p)[4][2], const float (&uv)[4][2], const float rgb[3], float a, int kind)
    {
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k : order)
            vertex(p[k][0], p[k][1], uv[k][0], uv[k][1], rgb, a, kind);
    }
    /* A quad with a colour at each corner (the road's heights). */
    void quad(const float (&p)[4][2], const float (&uv)[4][2], const float (&rgb)[4][3], float a, int kind)
    {
        static const int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k : order)
            vertex(p[k][0], p[k][1], uv[k][0], uv[k][1], rgb[k], a, kind);
    }
    void rect(float x0, float y0, float x1, float y1, const float rgb[3], float a)
    {
        const float p[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
        const float uv[4][2] = {};
        quad(p, uv, rgb, a, OverlayQuad::Flat);
    }
    /* A line, smooth-edged (Flat's u across it). */
    void line(float ax, float ay, float bx, float by, float width, const float rgb[3], float a)
    {
        const float dx = bx - ax, dy = by - ay, length = std::hypot(dx, dy);
        if (!(length > 0.0f))
            return;
        const float nx = -dy / length * width * 0.5f, ny = dx / length * width * 0.5f;
        const float p[4][2] = { { ax + nx, ay + ny }, { bx + nx, by + ny }, { bx - nx, by - ny }, { ax - nx, ay - ny } };
        const float uv[4][2] = { { -1, 0 }, { -1, 0 }, { 1, 0 }, { 1, 0 } };
        quad(p, uv, rgb, a, OverlayQuad::Flat);
    }
    void batch(const float (&clip)[4])
    {
        OverlayBatch b{ out.vertices.size(), { clip[0], clip[1], clip[2], clip[3] } };
        out.batches.push_back(b);
    }
};

/* The road on the map: only the nodes within `reach` of the player, found
 * through the track's index, each placed on the map once; the segments to
 * and from them laid as a strip, mitred at each node, twice -- the darker
 * outline, wider, then the road over it shaded by slope (Track::shade).  Buffers kept from
 * frame to frame. */
template <typename ToMap>
void road(win32::WinApplication* app, Builder& b, float metres, float meX, float meZ, float side, const ToMap& toMap)
{
    const Track& t = track(app);
    if (t.count <= 1)
        return;
    const std::size_t n = std::size_t(t.count);
    static std::vector<std::uint32_t> placed, inReach;   // the frame each was placed / found in reach
    static std::vector<float> sx, sy;
    static std::vector<std::pair<std::uint32_t, std::uint32_t>> segments;
    static std::uint32_t frame = 0;
    if (placed.size() != n)
    {
        placed.assign(n, 0);
        inReach.assign(n, 0);
        sx.resize(n);
        sy.resize(n);
        frame = 0;
    }
    if (++frame == 0)
    {
        std::fill(placed.begin(), placed.end(), 0);
        std::fill(inReach.begin(), inReach.end(), 0);
        frame = 1;
    }
    auto next = [&](std::size_t i) -> long { return i + 1 < n ? long(i + 1) : (t.loop ? 0L : -1L); };
    auto prev = [&](std::size_t i) -> long { return i > 0 ? long(i - 1) : (t.loop ? long(n - 1) : -1L); };
    auto place = [&](std::size_t i) {
        if (placed[i] != frame)
        {
            toMap(t.x[i], t.z[i], sx[i], sy[i]);
            placed[i] = frame;
        }
    };

    // the nodes in reach, by the cells the reach covers
    const float reach = metres * kReachMargin * 1.42f;
    segments.clear();
    const int cx0 = SDL_max(0, int((meX - reach - t.originX) / kBucket));
    const int cx1 = SDL_min(t.cellsX - 1, int((meX + reach - t.originX) / kBucket));
    const int cz0 = SDL_max(0, int((meZ - reach - t.originZ) / kBucket));
    const int cz1 = SDL_min(t.cellsZ - 1, int((meZ + reach - t.originZ) / kBucket));
    for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz)
        {
            const std::size_t c = std::size_t(cx) * std::size_t(t.cellsZ) + std::size_t(cz);
            for (std::uint32_t at = t.cellStart[c]; at < t.cellStart[c + 1]; ++at)
            {
                const std::uint32_t i = t.cellNodes[at];
                const float dx = t.x[i] - meX, dz = t.z[i] - meZ;
                if (dx * dx + dz * dz < reach * reach)
                    inReach[i] = frame;
            }
        }
    for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz)
        {
            const std::size_t c = std::size_t(cx) * std::size_t(t.cellsZ) + std::size_t(cz);
            for (std::uint32_t at = t.cellStart[c]; at < t.cellStart[c + 1]; ++at)
            {
                const std::uint32_t i = t.cellNodes[at];
                if (inReach[i] != frame)
                    continue;
                const long j = next(i), h = prev(i);
                if (j >= 0)
                    segments.push_back({ i, std::uint32_t(j) });
                if (h >= 0 && inReach[std::size_t(h)] != frame)
                    segments.push_back({ std::uint32_t(h), i });   // the one coming in from out of reach
            }
        }
    if (segments.empty())
        return;

    // a node's unit normal across the road, mitred, on the map
    auto across = [&](std::size_t i, float& nx, float& ny) {
        place(i);
        const long a = prev(i), c = next(i);
        float dx = 0.0f, dy = 0.0f, ex = 0.0f, ey = 0.0f, el = 0.0f;
        if (a >= 0)
        {
            place(std::size_t(a));
            const float fx = sx[i] - sx[std::size_t(a)], fy = sy[i] - sy[std::size_t(a)], l = std::hypot(fx, fy);
            if (l > 0.0f) { dx += fx / l; dy += fy / l; }
        }
        if (c >= 0)
        {
            place(std::size_t(c));
            ex = sx[std::size_t(c)] - sx[i];
            ey = sy[std::size_t(c)] - sy[i];
            el = std::hypot(ex, ey);
            if (el > 0.0f) { dx += ex / el; dy += ey / el; }
        }
        const float l = std::hypot(dx, dy);
        if (!(l > 0.0f)) { nx = 0.0f; ny = 0.0f; return; }
        nx = -dy / l;
        ny = dx / l;
        if (el > 0.0f)
        {
            const float cosine = SDL_fabsf(nx * (-ey / el) + ny * (ex / el));
            const float m = cosine > 1.0f / kMiterMost ? 1.0f / cosine : kMiterMost;
            nx *= m;
            ny *= m;
        }
    };
    static std::vector<float> corners;   // per segment: both ends' place and normal
    corners.resize(segments.size() * 8);
    for (std::size_t s = 0; s < segments.size(); ++s)
    {
        float* c = &corners[s * 8];
        const std::size_t i = segments[s].first, k = segments[s].second;
        across(i, c[2], c[3]);
        across(k, c[6], c[7]);
        c[0] = sx[i]; c[1] = sy[i];
        c[4] = sx[k]; c[5] = sy[k];
    }
    auto grey = [&](std::size_t i, float (&rgb)[3]) {
        rgb[0] = t.shade[i * 3];
        rgb[1] = t.shade[i * 3 + 1];
        rgb[2] = t.shade[i * 3 + 2];
    };
    b.out.vertices.reserve(b.out.vertices.size() + segments.size() * 12);
    static const float uv[4][2] = { { -1, 0 }, { -1, 0 }, { 1, 0 }, { 1, 0 } };
    for (int pass = 0; pass < 2; ++pass)
    {
        const float half = side * (kRoadWidth * 0.5f + (pass == 0 ? kRoadOutline : 0.0f));
        for (std::size_t s = 0; s < segments.size(); ++s)
        {
            const float* c = &corners[s * 8];
            const float p[4][2] = { { c[0] + c[2] * half, c[1] + c[3] * half },
                                    { c[4] + c[6] * half, c[5] + c[7] * half },
                                    { c[4] - c[6] * half, c[5] - c[7] * half },
                                    { c[0] - c[2] * half, c[1] - c[3] * half } };
            float rgb[4][3];
            if (pass == 0)
                for (auto& one : rgb)
                    one[0] = one[1] = one[2] = kOutlineGrey;
            else
            {
                grey(segments[s].first, rgb[0]);
                grey(segments[s].second, rgb[1]);
                grey(segments[s].second, rgb[2]);
                grey(segments[s].first, rgb[3]);
            }
            b.quad(p, uv, rgb, kRoadAlpha, OverlayQuad::Flat);
        }
    }

    // The finish line, over the road: centred on its node, square to the road there.
    const std::size_t finish = t.loop ? 0 : n - 1;
    if (inReach[finish] == frame)
    {
        float nx, ny;
        across(finish, nx, ny);
        const float l = std::hypot(nx, ny);
        if (l > 0.0f)
        {
            nx /= l;
            ny /= l;
            const float ax = ny, ay = -nx;   // along the road
            const float cell = side * kRoadWidth / float(kFinishColumns);
            const float ox = sx[finish], oy = sy[finish];
            static const float flat[4][2] = {};
            for (int r = 0; r < kFinishRows; ++r)
                for (int c = 0; c < kFinishColumns; ++c)
                {
                    const float a0 = (float(r) - kFinishRows * 0.5f) * cell, a1 = a0 + cell;
                    const float c0 = (float(c) - kFinishColumns * 0.5f) * cell, c1 = c0 + cell;
                    const float q[4][2] = { { ox + ax * a0 + nx * c0, oy + ay * a0 + ny * c0 },
                                            { ox + ax * a0 + nx * c1, oy + ay * a0 + ny * c1 },
                                            { ox + ax * a1 + nx * c1, oy + ay * a1 + ny * c1 },
                                            { ox + ax * a1 + nx * c0, oy + ay * a1 + ny * c0 } };
                    b.quad(q, flat, (r + c) % 2 ? kFinishLight : kFinishDark, kFinishAlpha, OverlayQuad::Flat);
                }
        }
    }
}

/* A driver's colour, 0 to 1 each, out of 0x..RRGGBB. */
void colourOf(std::uint32_t argb, float (&rgb)[3])
{
    rgb[0] = float((argb >> 16) & 0xff) / 255.0f;
    rgb[1] = float((argb >> 8) & 0xff) / 255.0f;
    rgb[2] = float(argb & 0xff) / 255.0f;
}

struct Car
{
    x86::reg32 address;
    float rgb[3];
    float scale;
};

}  // namespace minimap

}  // namespace

/* hudNative's map step, in place of the game's map (sub_48d4c0): the port's
 * own drawn for HUD player `player`; false where it is not (switched off,
 * split screen, no sprites, no rectangle yet) and the game's is wanted. */
bool minimapDraw(win32::WinApplication* app, x86::reg32 player)
{
    using namespace minimap;
    if (!enabled() || player != 0 || app->getMemory<x86::reg32>(kSplitScreen) == 1)
        return false;
    const std::shared_ptr<const OverlayArt> sprites = art();
    if (!sprites)
        return false;
    auto u32 = [app](x86::reg32 a) -> x86::reg32 { return app->getMemory<x86::reg32>(a); };
    auto f32 = [app](x86::reg32 a) -> float { return app->getMemory<float>(a); };
    const x86::reg32 index = u32(kPlayerIndex);
    if (index >= kMaxCars)
        return false;
    const x86::reg32 me = u32(kMapPlayerCars + index * 4);
    if (!me)
        return false;
    const bool cop = (app->getMemory<x86::reg8>(me + kCarFlags) & kCopBit) != 0;

    // Where: the element's rectangle, the screen's pixels into HUD units.
    const x86::reg32 rect = kHudRects + player * kHudRectsPerPlayer * 16 + (cop ? kCopMapElement : kMapElement) * 16;
    const float px0 = float(x86::sreg32(u32(rect))), py0 = float(x86::sreg32(u32(rect + 4)));
    const float px1 = float(x86::sreg32(u32(rect + 8))), py1 = float(x86::sreg32(u32(rect + 12)));
    const x86::sreg32 screenH = x86::sreg32(u32(kScreenHeight));
    if (!(px1 > px0) || !(py1 > py0) || screenH <= 0)
        return false;
    const float unit = kOverlayUnits / float(screenH);
    const float x0 = px0 * unit, y0 = py0 * unit, x1 = px1 * unit, y1 = py1 * unit;
    const float side = SDL_min(x1 - x0, y1 - y0);
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    // The HUD options: Map display and Map Scale, of the layout this view and car use.
    const x86::reg32 layout = kLayout + (player + (u32(kSplitScreen) == 1 ? 1u : 0u)) * kLayoutView + (cop ? kLayoutCop : 0u);
    const bool rotating = u32(layout + kLayoutMapMode) == kMapRotating;
    float menuScale = f32(layout + kLayoutMapScale);
    if (!(menuScale >= kMenuScaleLeast && menuScale <= kMenuScaleMost))
        menuScale = 1.0f;
    const float metres = kRange / (menuScale * extraZoom());
    {
        // said when it changes: what the HUD options give the map (the menu's raw field with it)
        static float s_scale = -1.0f;
        static int s_rotating = -1;
        static x86::reg32 s_layout = 0;
        if (menuScale != s_scale || int(rotating) != s_rotating || layout != s_layout)
        {
            SDL_Log("[MINIMAP] layout %#x: Map display %u (%s), Map Scale %.2f (field %#010x): %.0f m to the edge",
                    unsigned(layout), unsigned(u32(layout + kLayoutMapMode)), rotating ? "rotating" : "stationary",
                    double(menuScale), unsigned(u32(layout + kLayoutMapScale)), double(metres));
            s_scale = menuScale;
            s_rotating = int(rotating);
            s_layout = layout;
        }
    }
    const float scale = (side * 0.5f) / metres;   // HUD units a metre
    const float meX = f32(me + kCarPos), meZ = f32(me + kCarPos + 8);
    /* The map's axes in the world: up the map, and to its right (x right, z
     * forward, so a heading's right is (z, -x)).  Stationary: +z and +x.
     * Rotating: the player's heading, eased. */
    float upX = 0.0f, upZ = 1.0f;
    if (rotating)
    {
        static float s_heading = 0.0f;
        static Uint64 s_then = 0;
        static x86::reg32 s_car = 0;
        const float hx = f32(me + kCarForward), hz = f32(me + kCarForward + 8);
        if (hx * hx + hz * hz > 1e-6f)
        {
            const float heading = std::atan2(hx, hz);
            const Uint64 now = SDL_GetTicks();
            if (s_car != me || !s_then || now - s_then > 500)
                s_heading = heading;   // a new race, or back after a while: at once
            else
            {
                const float dt = float(now - s_then) / 1000.0f;
                float d = heading - s_heading;
                d -= 6.2831853f * std::floor((d + 3.14159265f) / 6.2831853f);
                s_heading += d * SDL_min(1.0f, dt / kHeadingEase);
            }
            s_then = now;
            s_car = me;
        }
        upX = std::sin(s_heading);
        upZ = std::cos(s_heading);
    }
    const float rightX = upZ, rightZ = -upX;
    auto toMap = [&](float wx, float wz, float& sx, float& sy) {
        const float dx = wx - meX, dz = wz - meZ;
        sx = cx + (dx * rightX + dz * rightZ) * scale;
        sy = cy - (dx * upX + dz * upZ) * scale;
    };

    auto overlay = std::make_shared<Overlay>();
    overlay->art = sprites;
    static std::size_t s_lastVertices = 2048;   // last frame's, so the vertices grow at most once
    overlay->vertices.reserve(s_lastVertices + s_lastVertices / 4);
    overlay->batches.reserve(4);
    Builder b{ *overlay };
    const float clip[4] = { x0, y0, x1, y1 };

    // The panel; then, cut to it, its grid, fixed to the world.
    b.rect(x0, y0, x1, y1, kPanel, kPanelAlpha);
    b.batch(kNoClip);
    {
        const float lineW = side * kGridLine;
        const float span = std::hypot(x1 - x0, y1 - y0) * 0.5f / scale;   // the map's corners, turned or not
        for (float gx = std::floor((meX - span) / kGridMetres) * kGridMetres; gx <= meX + span; gx += kGridMetres)
        {
            float ax, ay, bx, by;
            toMap(gx, meZ - span, ax, ay);
            toMap(gx, meZ + span, bx, by);
            b.line(ax, ay, bx, by, lineW, kGrid, kGridAlpha);
        }
        for (float gz = std::floor((meZ - span) / kGridMetres) * kGridMetres; gz <= meZ + span; gz += kGridMetres)
        {
            float ax, ay, bx, by;
            toMap(meX - span, gz, ax, ay);
            toMap(meX + span, gz, bx, by);
            b.line(ax, ay, bx, by, lineW, kGrid, kGridAlpha);
        }
    }

    // The road: the nodes in reach, each joint mitred, an outline under the road.
    road(app, b, metres, meX, meZ, side, toMap);

    // The cars: as the game's map has them, the player's last.
    static std::vector<Car> cars;
    cars.clear();
    const Uint64 clock = Uint64(u32(kRaceClock));
    for (x86::reg32 c = 0; c < u32(kCopCount) && c < kMaxCars; ++c)
    {
        const x86::reg32 car = u32(kCops + c * 4);
        if (!car || !u32(car + kCarChasing))
            continue;
        const bool red = ((x86::sreg32(clock >> kCopFlashShift) + x86::sreg32(u32(car + kCarNumber))) % 2) == 0;
        Car one{ car, {}, 1.0f };
        colourOf(red ? kCopRed : kCopBlue, one.rgb);
        cars.push_back(one);
    }
    const x86::reg32 hidden = u32(kHiddenId);
    const bool meHidden = u32(u32(me + kCarRecord) + kRecordId) == hidden;
    for (x86::reg32 c = 0; c < u32(kRacerCount) && c < kMaxCars; ++c)
    {
        const x86::reg32 car = u32(kRacers + c * 4);
        if (!car || car == me)
            continue;
        if (u32(u32(car + kCarRecord) + kRecordId) == hidden && !meHidden)
            continue;
        if ((app->getMemory<x86::reg8>(car + kCarFlags) & kFlagsBit0) && u32(car + kCarState) == 4
            && (!(app->getMemory<x86::reg8>(me + kCarFlags) & kFlagsBit0) || u32(me + kCarState) != 4))
            continue;
        Car one{ car, {}, 1.0f };
        colourOf(u32(kColours + u32(car + kCarNumber) * 4), one.rgb);
        cars.push_back(one);
    }
    {
        Car one{ me, {}, kPlayerScale };
        colourOf(u32(kColours + u32(me + kCarNumber) * 4), one.rgb);
        cars.push_back(one);
    }
    for (const Car& car : cars)
    {
        float sx, sy;
        toMap(f32(car.address + kCarPos), f32(car.address + kCarPos + 8), sx, sy);
        const float wx = f32(car.address + kCarForward), wz = f32(car.address + kCarForward + 8);
        float fx = wx * rightX + wz * rightZ, fy = -(wx * upX + wz * upZ);
        const float l = std::hypot(fx, fy);
        if (!(l > 0.0f))
        {
            fx = 0.0f;
            fy = -1.0f;
        }
        else
        {
            fx /= l;
            fy /= l;
        }
        const float rx = -fy, ry = fx;   // the car's right, on the map
        const float size = side * kPrism * car.scale;
        // the tile's corners, about the triangle's centroid
        const float tx[4] = { 0.0f, kTile, kTile, 0.0f }, ty[4] = { 0.0f, 0.0f, kTile, kTile };
        float p[4][2], uv[4][2];
        for (int k = 0; k < 4; ++k)
        {
            const float across = (tx[k] - kTile * 0.5f) / kTile * size, ahead = (kPrismCentreY - ty[k]) / kTile * size;
            p[k][0] = sx + rx * across + fx * ahead;
            p[k][1] = sy + ry * across + fy * ahead;
            uv[k][0] = k == 1 || k == 2 ? kPrismU1 : kPrismU0;
            uv[k][1] = ty[k] / kTile;
        }
        b.quad(p, uv, car.rgb, kCarAlpha, OverlayQuad::Map);
    }
    b.batch(clip);

    // The frame: a corner piece at each corner, the tile turned by its uv.
    {
        const float cs = side * kCorner;
        const float grey[3] = { kCornerGrey, kCornerGrey, kCornerGrey };
        for (int corner = 0; corner < 4; ++corner)
        {
            const bool right = corner & 1, bottom = corner & 2;
            const float qx0 = right ? x1 - cs : x0, qy0 = bottom ? y1 - cs : y0;
            const float u0 = right ? kCornerU1 : kCornerU0, u1 = right ? kCornerU0 : kCornerU1;
            const float v0 = bottom ? 1.0f : 0.0f, v1 = bottom ? 0.0f : 1.0f;
            const float p[4][2] = { { qx0, qy0 }, { qx0 + cs, qy0 }, { qx0 + cs, qy0 + cs }, { qx0, qy0 + cs } };
            const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            b.quad(p, uv, grey, 1.0f, OverlayQuad::Map);
        }
    }
    s_lastVertices = overlay->vertices.size();
    return win32::glide2x::overlayFrame(OverlayMinimap, std::move(overlay));
}

/* hudNative's map step, after the game's map (sub_48d4c0): the trace, when
 * asked for. */
void minimapTrace(win32::WinApplication* app, x86::reg32 player)
{
    using namespace minimap;
    if (!tracing() || player > 1)
        return;
    static Uint64 s_last[2] = { 0, 0 };
    static x86::reg32 s_trackNodes = 0;
    const Uint64 now = SDL_GetTicks();
    if (now - s_last[player] < kTraceEveryMs)
        return;
    s_last[player] = now;
    auto u32 = [app](x86::reg32 a) -> x86::reg32 { return app->getMemory<x86::reg32>(a); };
    auto f32 = [app](x86::reg32 a) { return double(app->getMemory<float>(a)); };

    const x86::sreg32 count = x86::sreg32(u32(kNodeCount));
    if (count > 0 && u32(kNodes) != s_trackNodes)
    {
        s_trackNodes = u32(kNodes);
        traceTrack(app, count);
    }

    const x86::reg32 index = player == 0 ? u32(kPlayerIndex) : player;
    const x86::reg32 car = index < 16 ? u32(kPlayerCars + index * 4) : 0;
    const bool cop = car && (app->getMemory<x86::reg8>(car + kCarFlags) & kCopBit);
    const x86::reg32 rects = kHudRects + player * kHudRectsPerPlayer * 16;
    auto rect = [&](x86::reg32 element, int k) { return int(x86::sreg32(u32(rects + element * 16 + x86::reg32(k) * 4))); };
    const x86::reg32 view = player + (u32(kSplitScreen) == 1 ? 1u : 0u);
    const x86::reg32 game = kGameMapRect + player * 16;
    SDL_Log("[MINIMAP] p%u: element17 (%d %d %d %d) element22 (%d %d %d %d) game's (%.1f %.1f %.1f %.1f) "
            "centre (%.1f %.1f) radius %.2f layoutZoom %.3f/%#x cop %d split %u",
            unsigned(player), rect(kMapElement, 0), rect(kMapElement, 1), rect(kMapElement, 2), rect(kMapElement, 3),
            rect(kCopMapElement, 0), rect(kCopMapElement, 1), rect(kCopMapElement, 2), rect(kCopMapElement, 3),
            f32(game), f32(game + 4), f32(game + 8), f32(game + 12),
            f32(kGameMapCentreX + player * 4), f32(kGameMapCentreY + player * 4), f32(kGameMapRadius + player * 4),
            f32(kLayout + view * kLayoutView + (cop ? kLayoutCop : 0)), unsigned(u32(kLayout + view * kLayoutView + (cop ? kLayoutCop : 0))),
            int(cop), unsigned(u32(kSplitScreen)));

    if (!car || count <= 0)
        return;
    const float px = float(f32(car + kCarPos)), py = float(f32(car + kCarPos + 4)), pz = float(f32(car + kCarPos + 8));
    const x86::sreg32 node = x86::sreg32(u32(car + kCarNode));
    float off = -1.0f, roadDeg = 0.0f;
    if (node >= 0 && node < count)
    {
        const float dx = nodeAt(app, x86::reg32(node), 0) - px, dz = nodeAt(app, x86::reg32(node), 2) - pz;
        off = std::sqrt(dx * dx + dz * dz);
        const x86::reg32 next = x86::reg32((node + 1) % count);
        roadDeg = std::atan2(nodeAt(app, next, 0) - nodeAt(app, x86::reg32(node), 0),
                             nodeAt(app, next, 2) - nodeAt(app, x86::reg32(node), 2)) * 57.2958f;
    }
    x86::sreg32 nearest = -1;
    float best = 1e30f;
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        const float dx = nodeAt(app, x86::reg32(i), 0) - px, dz = nodeAt(app, x86::reg32(i), 2) - pz;
        const float d = dx * dx + dz * dz;
        if (d < best)
        {
            best = d;
            nearest = i;
        }
    }
    const float carDeg = std::atan2(float(f32(car + kCarForward)), float(f32(car + kCarForward + 8))) * 57.2958f;
    SDL_Log("[MINIMAP] p%u car %#x at (%.1f %.1f %.1f) node %d %.1f m off (height %.1f); nearest node %d at %.1f m; "
            "heading car %.0f road %.0f",
            unsigned(player), unsigned(car), px, py, pz, int(node), off,
            node >= 0 && node < count ? nodeAt(app, x86::reg32(node), 1) : 0.0f, int(nearest), std::sqrt(best), carDeg, roadDeg);
}

}  // namespace nfs3hp
