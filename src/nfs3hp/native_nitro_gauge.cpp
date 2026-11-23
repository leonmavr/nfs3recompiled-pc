#include <nfs3hp.h>
#include "native_nitro.h"
#include <lib/overlay.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/* Port: the nitro bottle on the HUD (NFS_NITRO_UI, on).
 *
 * The picture is baked from the drawing the player made, assets/nitro/
 * nos_bottle.svg, by tools/generate_nitro_art.py: one atlas with two tiles of
 * the same size -- the bottle's silhouette filled solid, and the bottle as
 * drawn (grey outline, blue valve, the label whose window is cut out).  The
 * first is the liquid's mask: what is painted is inside the outline and
 * nothing else, and the label's window and the lettering cut out of it let the
 * liquid show through as a real bottle's do.  The second goes over it, so the
 * label's semi-transparent blue tints the liquid behind it and its opaque
 * parts, the white border and the outline, hide it.
 *
 * How full.  nitroCharge(), 0 to 1: the liquid rises from the bottle's bottom,
 * a straight surface across it, by cutting both the quad's rectangle and its
 * texels -- the mask's bottom `level` of itself, so the shape is never
 * squashed.  Its colour runs across the bottle, #b8523b at the left edge to
 * #47e000 at the right, and the right end fades back towards the left one as
 * the bottle empties, so a nearly-empty bottle is a warm red at a glance and a
 * full one is green through and through (kLow, kHigh).  `kSlices` columns make
 * the gradient without a new shader: each is one tinted quad of the mask.
 *
 * Where.  Right of the slipstream's icon, as tall as it (kHeight, kLeft,
 * kBottom), drawn as a layer of the port's own overlay (lib/overlay.h,
 * OverlayNitro) set again for each frame it is in.  Fixed there for now: a
 * movable element of its own comes later (TODOs.md, the nitro milestone's
 * second step).  Split screen shows none: the bottle there is the game's to
 * give, as its map is.  NFS_NITRO_CHARGE pins the level, to look at the bottle
 * full, half and empty without a race to fill it.
 *
 * The picture is kept white in its mask and as it was drawn in its art, read
 * from beside the executable (the desktop build copies assets/nitro there) or,
 * on Android, from the app's assets. */
namespace nfs3hp
{

bool raceIsRunning();   // nfs3hp_main.cpp

namespace
{
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;   // 1: two players, and no nitro of the port's
}

namespace
{

namespace gauge
{
constexpr const char* kAtlasFile = "assets/nitro/nitro-atlas.png";   // beside the executable
constexpr const char* kAssetPath = "nitro/nitro-atlas.png";          // in an Android build's assets
constexpr int kTileW = 184, kTileH = 465;   // the atlas's two tiles, texels
constexpr int kGutter = 8;                  // clear texels round them
constexpr int kAtlasW = kTileW * 2 + kGutter * 3, kAtlasH = kTileH + kGutter * 2;
constexpr float kMaskU0 = float(kGutter) / float(kAtlasW);
constexpr float kMaskU1 = float(kGutter + kTileW) / float(kAtlasW);
constexpr float kArtU0 = float(kGutter * 2 + kTileW) / float(kAtlasW);
constexpr float kArtU1 = float(kGutter * 2 + kTileW * 2) / float(kAtlasW);
constexpr float kV0 = float(kGutter) / float(kAtlasH), kV1 = float(kGutter + kTileH) / float(kAtlasH);
// Where it stands, HUD units: right of the slipstream's icon (12 and 84 tall
// there, 40 wide), its bottom on the same line.
constexpr float kHeight = 84.0f;
constexpr float kLeft = 62.0f;
constexpr float kBottom = 14.0f;
constexpr int kSlices = 16;                              // the gradient's columns
constexpr float kLow[3] = { 184.0f / 255.0f, 82.0f / 255.0f, 59.0f / 255.0f };    // #b8523b, nearly empty
constexpr float kHigh[3] = { 71.0f / 255.0f, 224.0f / 255.0f, 0.0f / 255.0f };    // #47e000, full
constexpr float kFillAlpha = 0.92f;
constexpr float kLeastLevel = 0.004f;                    // below this nothing is drawn
}  // namespace gauge

struct Art
{
    std::shared_ptr<OverlayArt> pictures;   // the atlas
};

std::unique_ptr<Art> s_art;
bool s_artTried = false;

bool enabled()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_NITRO_UI");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return on;
}

/* The atlas read into the layer's picture: the mask tile kept white with its
 * own alpha, the art tile as drawn, both premultiplied by that alpha. */
bool readAtlas(OverlayArt& out)
{
    using namespace gauge;
    const char* base = SDL_GetBasePath();
    const std::string beside = std::string(base ? base : "") + kAtlasFile;
    const char* tries[] = { beside.c_str(), kAssetPath };
    SDL_Surface* loaded = nullptr;
    for (const char* path : tries)
        if ((loaded = SDL_LoadPNG(path)) != nullptr)
            break;
    if (!loaded)
    {
        SDL_Log("[NITRO] the bottle %s not read (%s)", beside.c_str(), SDL_GetError());
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba || rgba->w != kAtlasW || rgba->h != kAtlasH)
    {
        SDL_Log("[NITRO] the bottle %s is not %dx%d (%s)", beside.c_str(), kAtlasW, kAtlasH, SDL_GetError());
        if (rgba)
            SDL_DestroySurface(rgba);
        return false;
    }
    out.iconW = kAtlasW;
    out.iconH = kAtlasH;
    out.icon.assign(std::size_t(kAtlasW) * kAtlasH * 4, 0);
    const std::uint8_t* pixels = static_cast<const std::uint8_t*>(rgba->pixels);
    for (int y = 0; y < kAtlasH; ++y)
    {
        for (int x = 0; x < kAtlasW; ++x)
        {
            const std::uint8_t* s = pixels + std::size_t(y) * rgba->pitch + std::size_t(x) * 4;
            std::uint8_t* o = &out.icon[(std::size_t(y) * kAtlasW + std::size_t(x)) * 4];
            const float a = float(s[3]) / 255.0f;
            const bool mask = x >= kGutter && x < kGutter + kTileW;
            if (mask)
            {
                // The mask: white, weighted by its own alpha, for the tint to colour.
                o[0] = o[1] = o[2] = std::uint8_t(a * 255.0f + 0.5f);
                o[3] = s[3];
            }
            else
            {
                o[0] = std::uint8_t(float(s[0]) * a + 0.5f);
                o[1] = std::uint8_t(float(s[1]) * a + 0.5f);
                o[2] = std::uint8_t(float(s[2]) * a + 0.5f);
                o[3] = s[3];
            }
        }
    }
    SDL_DestroySurface(rgba);
    SDL_Log("[NITRO] the bottle read, %dx%d texels from %s", kTileW, kTileH, beside.c_str());
    return true;
}

const Art* art()
{
    if (s_art || s_artTried)
        return s_art.get();
    s_artTried = true;
    auto made = std::make_unique<Art>();
    made->pictures = std::make_shared<OverlayArt>();
    if (!readAtlas(*made->pictures))
        return nullptr;
    s_art = std::move(made);
    return s_art.get();
}

float level()
{
    static const float pinned = [] {
        const char* text = SDL_getenv("NFS_NITRO_CHARGE");
        if (!text || !*text)
            return -1.0f;
        const float parsed = float(SDL_atof(text));
        return parsed >= 0.0f && parsed <= 1.0f ? parsed : -1.0f;
    }();
    return pinned >= 0.0f ? pinned : SDL_clamp(nitroCharge(), 0.0f, 1.0f);
}

void draw(const Art& a, float charge)
{
    using namespace gauge;
    auto overlay = std::make_shared<Overlay>();
    overlay->art = a.pictures;
    const float width = kHeight * float(kTileW) / float(kTileH);
    const float left = kLeft;
    const float bottom = kOverlayUnits - kBottom;
    const float top = bottom - kHeight;
    if (charge > kLeastLevel)
    {
        /* The liquid: the mask's bottom `charge` of itself, in columns whose
         * tint runs from kLow at the bottle's left to as much of kHigh as the
         * level has got to at its right. */
        const float surface = bottom - kHeight * charge;
        const float v0 = kV0 + (1.0f - charge) * (kV1 - kV0);
        for (int i = 0; i < kSlices; ++i)
        {
            const float across = (float(i) + 0.5f) / float(kSlices);
            const float x0 = left + width * float(i) / float(kSlices);
            const float x1 = left + width * float(i + 1) / float(kSlices);
            const float u0 = kMaskU0 + (kMaskU1 - kMaskU0) * float(i) / float(kSlices);
            const float u1 = kMaskU0 + (kMaskU1 - kMaskU0) * float(i + 1) / float(kSlices);
            float tint[3];
            for (int c = 0; c < 3; ++c)
            {
                const float right = kLow[c] + (kHigh[c] - kLow[c]) * charge;
                tint[c] = kLow[c] + (right - kLow[c]) * across;
            }
            overlay->quads.push_back({ x0, surface, x1, bottom, u0, v0, u1, kV1, tint[0], tint[1], tint[2], kFillAlpha,
                                       OverlayQuad::Icon });
        }
    }
    overlay->quads.push_back({ left, top, left + width, bottom, kArtU0, kV0, kArtU1, kV1, 1.0f, 1.0f, 1.0f, 1.0f,
                               OverlayQuad::Icon });
    win32::glide2x::overlayFrame(OverlayNitro, std::move(overlay));
}

}  // namespace

/* Once a frame (onSwap): the bottle, as full as the nitro is. */
void nitroGaugeTick(win32::WinApplication* app)
{
    if (!enabled() || !raceIsRunning() || app->getMemory<x86::reg32>(kSplitScreen) == 1)
        return;
    const Art* a = art();
    if (!a)
        return;
    draw(*a, level());
}

}  // namespace nfs3hp
