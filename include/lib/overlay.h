#ifndef LIB_OVERLAY_H_
#define LIB_OVERLAY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/* Port: what the port lays over a finished race frame, HUD and all
 * (ThrashRenderer::drawOverlay) -- the minimap (src/nfs3hp/native_minimap.cpp),
 * the opponents' list (src/nfs3hp/native_standings.cpp), the slipstream's icon
 * (src/nfs3hp/native_slipstream.cpp) and the song now playing
 * (src/nfs3hp/native_now_playing.cpp), each a layer of its own
 * (OverlayLayer).  Laid out in HUD units: the picture is kOverlayUnits high, x
 * from its left edge, y down from its top, whatever its size in pixels; the
 * renderer scales them by the picture's height. */
constexpr float kOverlayUnits = 480.0f;

/* The overlay's layers, drawn in this order: each source sets its own.  The
 * opponents' list is over the minimap and under the slipstream's icon, which
 * shares the picture's bottom left with it, and the song's panel slides over
 * them both. */
enum OverlayLayer
{
    OverlayMinimap = 0,
    OverlayStandings = 1,
    OverlaySlip = 2,
    OverlayNowPlaying = 3,
    OverlayLayerCount
};

/* Its pictures, made once on the game thread and handed over whole: each RGBA,
 * 8 bits a channel, premultiplied by its alpha. */
struct OverlayArt
{
    int fontW = 0, fontH = 0;
    std::vector<std::uint8_t> font;   // the HUD's font, white, its dark edge kept
    int iconW = 0, iconH = 0;
    std::vector<std::uint8_t> icon;   // a layer's own icon: the NFS emblem, the slipstream's two cars
    int mapW = 0, mapH = 0;
    std::vector<std::uint8_t> map;    // the minimap's sprites (assets/minimap/minimap-atlas.png)
};

/* One quad: where (HUD units), what of its picture (texels), its colour (each
 * channel 0 to 1, multiplying the picture's; a its opacity).  A panel's u and
 * v are its corners' places from the panel's centre, in HUD units. */
struct OverlayQuad
{
    enum Kind { Panel = 0, Glyph = 1, Icon = 2, Map = 3, Flat = 4 };
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float r, g, b, a;
    int kind;
};

/* One corner of a free-form triangle (Overlay::vertices): where (HUD units),
 * its uv and colour, and its kind --
 * - Map: a texel of the minimap's sprites (OverlayArt::map), tinted by the
 *   colour: a sprite's mid grey is the colour itself, darker greys darken it
 *   to black and lighter ones lighten it to white (the cars' prisms take
 *   their drivers' colours so; the frame's corners, at 0.5 grey, keep their
 *   own);
 * - Flat: the colour alone, premultiplied here; u across a strip from -1 to 1
 *   fades its two edges over a pixel (a line drawn smooth), 0 for none. */
struct OverlayVertex
{
    float x, y;
    float u, v;
    float r, g, b, a;
    int kind;
};

/* The vertices up to `end` (from the previous batch's end), cut to a
 * rectangle (HUD units, x0 y0 x1 y1; none where x1 <= x0). */
struct OverlayBatch
{
    std::size_t end;
    float clip[4];
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
    std::vector<OverlayVertex> vertices;   // triangles, three corners each, after the quads
    std::vector<OverlayBatch> batches;     // over `vertices`; none: all of them, uncut
    float panelHalfW = 0.0f, panelHalfH = 0.0f, panelRadius = 0.0f, panelGlow = 0.0f;
    float glowR = 1.0f, glowG = 0.8f, glowB = 0.1f;
};

#endif
