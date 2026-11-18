#ifndef LIB_OVERLAY_H_
#define LIB_OVERLAY_H_

#include <cstdint>
#include <memory>
#include <vector>

/* Port: what the port lays over a finished race frame, HUD and all
 * (ThrashRenderer::drawOverlay) -- the song now playing
 * (src/nfs3hp/native_now_playing.cpp).  Laid out in HUD units: the picture is
 * kOverlayUnits high, x from its left edge, y down from its top, whatever its
 * size in pixels; the renderer scales them by the picture's height. */
constexpr float kOverlayUnits = 480.0f;

/* Its pictures, made once on the game thread and handed over whole: each RGBA,
 * 8 bits a channel, premultiplied by its alpha. */
struct OverlayArt
{
    int fontW = 0, fontH = 0;
    std::vector<std::uint8_t> font;   // the HUD's font, white, its dark edge kept
    int iconW = 0, iconH = 0;
    std::vector<std::uint8_t> icon;   // the NFS emblem
};

/* One quad: where (HUD units), what of its picture (texels), its colour (each
 * channel 0 to 1, multiplying the picture's; a its opacity).  A panel's u and
 * v are its corners' places from the panel's centre, in HUD units. */
struct OverlayQuad
{
    enum Kind { Panel = 0, Glyph = 1, Icon = 2 };
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float r, g, b, a;
    int kind;
};

/* A frame's overlay: its pictures (shared from frame to frame; the renderer is
 * made again for each race, and uploads them again), its quads in the order
 * drawn, and the one panel's shape --
 * half its width and height, the corners' radius and how far its glow
 * reaches, in HUD units -- with the glow's colour. */
struct Overlay
{
    std::shared_ptr<const OverlayArt> art;
    std::vector<OverlayQuad> quads;
    float panelHalfW = 0.0f, panelHalfH = 0.0f, panelRadius = 0.0f, panelGlow = 0.0f;
    float glowR = 1.0f, glowG = 0.8f, glowB = 0.1f;
};

#endif
