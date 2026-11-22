#ifndef NFS3HP_NATIVE_HUD_FONT_H_
#define NFS3HP_NATIVE_HUD_FONT_H_

#include <lib/overlay.h>
#include <vector>

/* Port: the HUD's own font, for every overlay that writes in it
 * (native_hud_font.cpp) -- the song now playing's panel
 * (native_now_playing.cpp) and the slipstream's timer (native_slipstream.cpp).
 * Each overlay keeps its own picture of it, in its own OverlayArt, as its
 * layer has its own textures. */
namespace nfs3hp
{

struct HudGlyph
{
    int w = 0, h = 0;                      // texels
    int x = 0, y = 0;                      // where in the picture
    int advance = 0, left = 0, top = 0;    // texels: to the next, and its offset from the pen and the line's top
    bool known = false;
};

struct HudFont
{
    HudGlyph glyphs[256];
    float capTexels = 18.0f, capTop = 5.0f;   // 'A': its height and its offset from the line's top
};

/* The game's font read into `out`'s picture (fontW, fontH, font) and `font`;
 * false when its file cannot be read. */
bool readHudFont(OverlayArt& out, HudFont& font);

/* How far `text` runs, texels -- scale it by the HUD units a texel. */
float hudTextAdvance(const HudFont& font, const char* text);

/* `text` in `colour` (each channel 0 to 1) at `alpha`, its line's top at y
 * and its pen at x, `scale` HUD units a texel: glyph quads appended to
 * `quads`, drawn in that order. */
void addHudText(std::vector<OverlayQuad>& quads, const OverlayArt& art, const HudFont& font, float x, float y,
                float scale, const char* text, const float colour[3], float alpha);

}  // namespace nfs3hp

#endif
