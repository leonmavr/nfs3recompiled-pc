#include <nfs3hp.h>
#include "native_hud_font.h"
#include <lib/overlay.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* Port: the slipstream's icon and its time (NFS_SLIP_ICON, on).
 *
 * A car tucked in close behind another meets less air and is pushed on
 * (slipstream, native_surface.cpp; NFS_SLIPSTREAM).  While the player's car is
 * in one, a small icon shows up at the bottom left, its time printed over it:
 * slip_icon.png, a car following another through its wake seen from above,
 * white by day and bright green at night ([0x6fd4c8]), pulsing softly -- faded,
 * as the TODO asked -- from kPulseLeast to kPulseMost of its opacity every
 * kPulsePeriod.  "12.3" and SEC under it in the HUD's own font
 * (native_hud_font.cpp) stay at kTextAlpha, so the time reads while the icon
 * fades under it.
 *
 * At night it has a faint halo: the picture's alpha blurred twice along each
 * way (kGlowBlur), baked beside it in the same picture as a second tile and
 * drawn kGlowScale bigger, so the glow costs a quad rather than a pass.  Its
 * tint is the icon's, and the picture is kept white, so the shader's colour is
 * all that makes it white or green.
 *
 * Where.  kLeft and kBottom from the picture's edges, kHeight tall and as wide
 * as the picture is (762 by 1580 texels), drawn as a layer of the port's own
 * overlay (lib/overlay.h, OverlaySlip) set again for each frame it is in
 * (glide2x::overlayFrame): a menu, a pause or a race's end shows none.
 *
 * When.  The player's slipstream (slipstreamOf, 0 to 1) at kStartSlip or more
 * shows the icon, and it stays until it falls under kKeepSlip.  A spell's time
 * is the race's clock's ([0x7d3684], 64 a second), so a pause does not count
 * it, and it starts again with every spell; it stops at kMostSeconds.  In and
 * out over kFadeIn and kFadeOut.  NFS_SLIP_TEST makes the slipstream itself
 * (native_surface.cpp's), so it shows the icon too.
 *
 * The picture's grey is kept white -- kInk at its darkest, so the outlines
 * stay outlines -- and at a third of its texels: it is drawn about 40 by 84
 * HUD units.  It is read from beside the executable (the desktop build copies
 * assets/slipstream there) or, on Android, from the app's assets. */
namespace nfs3hp
{

bool raceIsRunning();                                            // nfs3hp_main.cpp
float slipstreamOf(win32::WinApplication* app, x86::reg32 car);   // native_surface.cpp

namespace
{

namespace slip
{
// The icon.
constexpr const char* kIconFile = "assets/slipstream/slip_icon.png";   // beside the executable
constexpr const char* kAssetPath = "slipstream/slip_icon.png";         // in an Android build's assets
constexpr float kPictureW = 762.0f, kPictureH = 1580.0f;
constexpr int kIconW = 254, kIconH = 527;   // texels it is kept at, a third of the picture's
constexpr int kGutter = 8;                  // clear texels round the atlas's two tiles
constexpr int kAtlasW = kIconW * 2 + kGutter * 3, kAtlasH = kIconH + kGutter * 2;
constexpr float kIconU0 = float(kGutter) / float(kAtlasW);
constexpr float kIconU1 = float(kGutter + kIconW) / float(kAtlasW);
constexpr float kGlowU0 = float(kGutter * 2 + kIconW) / float(kAtlasW);
constexpr float kGlowU1 = float(kGutter * 2 + kIconW * 2) / float(kAtlasW);
constexpr float kV0 = float(kGutter) / float(kAtlasH), kV1 = float(kGutter + kIconH) / float(kAtlasH);
constexpr int kGlowBlur = 10;               // texels the halo spreads, each way, twice
constexpr float kGlowScale = 1.15f;         // how much bigger than the icon the halo is drawn
constexpr float kGlowLeast = 0.20f, kGlowMost = 0.42f;   // its opacity at night, with the pulse
constexpr float kHeight = 84.0f;            // HUD units; its width follows the picture's shape
constexpr float kLeft = 12.0f;              // from the picture's left edge
constexpr float kBottom = 14.0f;            // from its bottom edge
constexpr float kInk = 0.10f;               // its darkest, the picture's black lines kept dark
constexpr float kWhite[3] = { 1.0f, 1.0f, 1.0f };        // by day
constexpr float kGreen[3] = { 0.35f, 1.0f, 0.45f };      // at night, the halo too
// The time, printed over it.
constexpr float kNumberCap = 11.5f;         // HUD units, a capital's height
constexpr float kUnitCap = 6.5f;
constexpr float kNumberTop = 0.40f;         // the number's capital a share of the icon's height down
constexpr float kTextGap = 1.5f;            // between it and SEC
constexpr float kTextAlpha = 0.95f;
constexpr float kMostSeconds = 99.9f;
// When.
constexpr float kStartSlip = 0.30f;   // the slipstream that shows it
constexpr float kKeepSlip = 0.15f;    // and that keeps it, until
constexpr float kFadeIn = 0.20f;      // seconds either fade takes
constexpr float kFadeOut = 0.30f;
constexpr float kPulsePeriod = 1.3f;  // seconds of the pulse
constexpr float kPulseLeast = 0.30f, kPulseMost = 0.80f;   // its opacity, the icon's own
constexpr float kTicksPerSecond = 64.0f;
constexpr x86::sreg32 kMostTicks = 64;   // more clock than this between two frames: not counted
constexpr x86::reg32 kPlayerCars = 0x5efa48;   // a car per player; the first player's
constexpr x86::reg32 kRaceClock = 0x7d3684;
constexpr x86::reg32 kNight = 0x6fd4c8;        // nonzero at night
}  // namespace slip

struct Art
{
    std::shared_ptr<OverlayArt> pictures;   // the icon and the HUD's font
    HudFont font;
};

std::unique_ptr<Art> s_art;
bool s_artTried = false;
bool s_showing = false;      // in a slipstream now
bool s_wasShowing = false;
float s_seconds = 0.0f;      // this spell's, shown as XX.Y
float s_shown = 0.0f;        // 0 to 1, its fade
x86::sreg32 s_lastClock = -1;
Uint64 s_clockMovedNs = 0;

bool enabled()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_SLIP_ICON");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return on;
}

/* The picture, kept white at kIconW by kIconH and a soft halo of it beside
 * it, both in the atlas the layer uploads: the icon's grey kept -- its
 * transparency weighting it first, so its clear parts do not darken its edges
 * -- and the halo its alpha blurred kGlowBlur texels along each way twice. */
bool readIcon(OverlayArt& out)
{
    using namespace slip;
    const char* base = SDL_GetBasePath();
    const std::string beside = std::string(base ? base : "") + kIconFile;
    const char* tries[] = { beside.c_str(), kAssetPath };
    SDL_Surface* loaded = nullptr;
    for (const char* path : tries)
        if ((loaded = SDL_LoadPNG(path)) != nullptr)
            break;
    if (!loaded)
    {
        SDL_Log("[SLIP] the icon %s not read (%s)", beside.c_str(), SDL_GetError());
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!rgba || rgba->w <= 0 || rgba->h <= 0)
    {
        SDL_Log("[SLIP] the icon %s is no RGBA picture (%s)", beside.c_str(), SDL_GetError());
        if (rgba)
            SDL_DestroySurface(rgba);
        return false;
    }
    const int sw = rgba->w, sh = rgba->h;
    out.iconW = kAtlasW;
    out.iconH = kAtlasH;
    out.icon.assign(std::size_t(kAtlasW) * kAtlasH * 4, 0);
    std::vector<float> alpha(std::size_t(kIconW) * kIconH);
    const std::uint8_t* pixels = static_cast<const std::uint8_t*>(rgba->pixels);
    for (int y = 0; y < kIconH; ++y)
    {
        const int sy0 = y * sh / kIconH, sy1 = SDL_max(sy0 + 1, (y + 1) * sh / kIconH);
        for (int x = 0; x < kIconW; ++x)
        {
            const int sx0 = x * sw / kIconW, sx1 = SDL_max(sx0 + 1, (x + 1) * sw / kIconW);
            float grey = 0.0f, sum = 0.0f;
            int n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx)
                {
                    const std::uint8_t* s = pixels + std::size_t(sy) * rgba->pitch + std::size_t(sx) * 4;
                    const float a = float(s[3]) / 255.0f;
                    grey += float(s[0]) / 255.0f * a;
                    sum += a;
                    ++n;
                }
            const float a = sum / float(n);
            const float l = sum > 0.0f ? grey / sum : 0.0f;   // the picture is grey with its alpha
            const float shade = (kInk + (1.0f - kInk) * l) * a;
            std::uint8_t* o = &out.icon[(std::size_t(kGutter + y) * kAtlasW + kGutter + x) * 4];
            o[0] = o[1] = o[2] = std::uint8_t(shade * 255.0f + 0.5f);
            o[3] = std::uint8_t(a * 255.0f + 0.5f);
            alpha[std::size_t(y) * kIconW + x] = a;
        }
    }
    SDL_DestroySurface(rgba);

    // The halo: its alpha blurred along each way twice, a running sum a texel.
    std::vector<float> pass(alpha.size());
    const float window = float(2 * kGlowBlur + 1);
    auto at = [](std::vector<float>& v, int x, int y) -> float& {
        return v[std::size_t(SDL_clamp(y, 0, kIconH - 1)) * kIconW + SDL_clamp(x, 0, kIconW - 1)];
    };
    for (int twice = 0; twice < 2; ++twice)
    {
        for (int y = 0; y < kIconH; ++y)   // along x
        {
            float sum = 0.0f;
            for (int x = -kGlowBlur; x <= kGlowBlur; ++x)
                sum += at(alpha, x, y);
            for (int x = 0; x < kIconW; ++x)
            {
                at(pass, x, y) = sum / window;
                sum += at(alpha, x + kGlowBlur + 1, y) - at(alpha, x - kGlowBlur, y);
            }
        }
        for (int x = 0; x < kIconW; ++x)   // and along y
        {
            float sum = 0.0f;
            for (int y = -kGlowBlur; y <= kGlowBlur; ++y)
                sum += at(pass, x, y);
            for (int y = 0; y < kIconH; ++y)
            {
                at(alpha, x, y) = sum / window;
                sum += at(pass, x, y + kGlowBlur + 1) - at(pass, x, y - kGlowBlur);
            }
        }
    }
    for (int y = 0; y < kIconH; ++y)
        for (int x = 0; x < kIconW; ++x)
        {
            const float g = alpha[std::size_t(y) * kIconW + x];
            std::uint8_t* o = &out.icon[(std::size_t(kGutter + y) * kAtlasW + kGutter * 2 + kIconW + x) * 4];
            o[0] = o[1] = o[2] = std::uint8_t(g * 255.0f + 0.5f);
            o[3] = std::uint8_t(g * 255.0f + 0.5f);
        }
    SDL_Log("[SLIP] the icon read, %dx%d texels with its halo, from %s", kIconW, kIconH, beside.c_str());
    return true;
}

const Art* art()
{
    if (s_art || s_artTried)
        return s_art.get();
    s_artTried = true;
    auto made = std::make_unique<Art>();
    made->pictures = std::make_shared<OverlayArt>();
    if (!readHudFont(*made->pictures, made->font) || !readIcon(*made->pictures))
    {
        SDL_Log("[SLIP] the icon or the HUD's font could not be read; no icon");
        return nullptr;
    }
    s_art = std::move(made);
    return s_art.get();
}

/* The icon and its time, `shown` 0 to 1 of the way in: the number over the two
 * cars, SEC under it, both centred on the icon; white by day, green at night,
 * with the halo round it. */
void show(const Art& a, float shown, bool night)
{
    using namespace slip;
    auto overlay = std::make_shared<Overlay>();
    overlay->art = a.pictures;
    const float width = kHeight * kPictureW / kPictureH;
    const float left = kLeft;
    const float top = kOverlayUnits - kBottom - kHeight;
    // The pulse: a sine over the icon, faded all the while.
    const float wave = 0.5f - 0.5f * std::cos(s_seconds * 2.0f * SDL_PI_F / kPulsePeriod);
    const float* tint = night ? kGreen : kWhite;
    if (night)
    {
        const float growX = width * (kGlowScale - 1.0f) * 0.5f, growY = kHeight * (kGlowScale - 1.0f) * 0.5f;
        const float glow = (kGlowLeast + (kGlowMost - kGlowLeast) * wave) * shown;
        overlay->quads.push_back({ left - growX, top - growY, left + width + growX, top + kHeight + growY,
                                   kGlowU0, kV0, kGlowU1, kV1, tint[0], tint[1], tint[2], glow, OverlayQuad::Icon });
    }
    const float alpha = (kPulseLeast + (kPulseMost - kPulseLeast) * wave) * shown;
    overlay->quads.push_back({ left, top, left + width, top + kHeight, kIconU0, kV0, kIconU1, kV1, tint[0], tint[1],
                               tint[2], alpha, OverlayQuad::Icon });
    char number[8];
    SDL_snprintf(number, sizeof number, "%.1f", SDL_min(s_seconds, kMostSeconds));
    auto centred = [&](const char* text, float capHeight, float capTop) {
        const float scale = capHeight / a.font.capTexels;
        const float lineTop = top + capTop - a.font.capTop * scale;
        const float x = left + (width - hudTextAdvance(a.font, text) * scale) * 0.5f;
        addHudText(overlay->quads, *a.pictures, a.font, x, lineTop, scale, text, kWhite, kTextAlpha * shown);
    };
    centred(number, kNumberCap, kHeight * kNumberTop);
    centred("SEC", kUnitCap, kHeight * kNumberTop + kNumberCap + kTextGap);
    win32::glide2x::overlayFrame(OverlaySlip, std::move(overlay));
}

}  // namespace

/* Once a frame (onSwap), the player's slipstream and the icon it shows. */
void slipstreamTick(win32::WinApplication* app)
{
    using namespace slip;
    if (!enabled() || !raceIsRunning())
    {
        s_showing = s_wasShowing = false;
        s_seconds = s_shown = 0.0f;
        s_lastClock = -1;
        return;
    }
    // The race's clock: how much of it has gone at those cars since the last frame.
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    if (clock != s_lastClock)
        s_clockMovedNs = SDL_GetTicksNS();
    float dt = 0.0f;
    const bool restart = s_lastClock < 0 || clock < s_lastClock;
    if (!restart && clock - s_lastClock <= kMostTicks)
        dt = float(clock - s_lastClock) / kTicksPerSecond;
    s_lastClock = clock;
    if (restart)
    {
        s_seconds = s_shown = 0.0f;
        s_showing = s_wasShowing = false;
    }
    const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCars);
    const float strength = car ? slipstreamOf(app, car) : 0.0f;
    if (strength < kKeepSlip)
        s_showing = false;
    else if (strength >= kStartSlip)
        s_showing = true;
    if (s_showing && !s_wasShowing)
        s_seconds = 0.0f;   // a new spell
    s_wasShowing = s_showing;
    if (s_showing)
        s_seconds += dt;
    s_shown = SDL_clamp(s_shown + (s_showing ? dt / kFadeIn : -dt / kFadeOut), 0.0f, 1.0f);
    if (s_shown <= 0.0f)
        return;   // nothing this frame: the layer set last frame is spent
    if (SDL_GetTicksNS() - s_clockMovedNs > 150 * SDL_NS_PER_MS)
        return;   // the clock standing: a pause, its menu over the picture
    if (const Art* a = art())
        show(*a, s_shown, app->getMemory<x86::reg32>(kNight) != 0);
}

}  // namespace nfs3hp
