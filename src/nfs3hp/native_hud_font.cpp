#include <nfs3hp.h>
#include "native_hud_font.h"
#include <lib/file.h>
#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* Port: the HUD's own font (gamedata/dashhud/cp16bi.ffn), read for the
 * overlay's text -- the song now playing's panel (native_now_playing.cpp) and
 * the slipstream's timer (native_slipstream.cpp).
 *
 * The file: "FNTF", its characters' count (+8), the line's height (+0x12),
 * the picture's offset (+0x1c); from +0x20 a character each 11 bytes -- its
 * code, width and height, where in the picture, its advance and its offsets
 * from the pen and from the line's top; the picture a 16-byte header (0x7a,
 * width +4, height +6) and 4 bits a texel, the first in the high half.  Laid
 * out again here kAtlasPad apart, white, its alpha whether it is drawn at
 * all: the low values are the characters' dark edges, the outline the HUD's
 * own text has. */
namespace nfs3hp
{
namespace
{

constexpr const char* kFontFile = "gamedata/dashhud/cp16bi.ffn";
constexpr int kAtlasWidth = 512;   // the font's characters, laid out again apart
constexpr int kAtlasPad = 4;       // texels round each, so its smaller levels never bleed

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) { return std::uint32_t(p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24); }

std::vector<std::uint8_t> loadFile(const char* name)
{
    const std::string path = win32::File::dataDirectory() + name;
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    std::vector<std::uint8_t> bytes;
    if (data)
    {
        bytes.assign(static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
        SDL_free(data);
    }
    else
        SDL_Log("[HUD] the font: %s not read (%s)", path.c_str(), SDL_GetError());
    return bytes;
}

}  // namespace

bool readHudFont(OverlayArt& out, HudFont& font)
{
    const std::vector<std::uint8_t> d = loadFile(kFontFile);
    if (d.size() < 0x20 || SDL_memcmp(d.data(), "FNTF", 4) != 0)
        return false;
    const int count = le16(&d[8]);
    const std::size_t picture = le32(&d[0x1c]);
    if (0x20 + std::size_t(count) * 11 > d.size() || picture + 16 > d.size() || d[picture] != 0x7a)
        return false;
    const int pw = le16(&d[picture + 4]), ph = le16(&d[picture + 6]);
    if (picture + 16 + std::size_t(pw) * ph / 2 > d.size())
        return false;
    auto texel = [&](int x, int y) {
        const std::size_t i = std::size_t(y) * pw + x;
        const std::uint8_t b = d[picture + 16 + i / 2];
        return (i & 1) ? (b & 15) : (b >> 4);
    };
    // Shelves of characters, as wide as the atlas.
    int penX = kAtlasPad, penY = kAtlasPad, shelf = 0;
    struct Place { int code, sx, sy; };
    std::vector<Place> places;
    for (int k = 0; k < count; ++k)
    {
        const std::uint8_t* r = &d[0x20 + std::size_t(k) * 11];
        const int code = le16(r);
        if (code < 0 || code > 255)
            continue;
        HudGlyph& g = font.glyphs[code];
        g.w = r[2];
        g.h = r[3];
        g.advance = std::int8_t(r[8]);
        g.left = std::int8_t(r[9]);
        g.top = std::int8_t(r[10]);
        g.known = true;
        const int sx = le16(r + 4), sy = le16(r + 6);
        if (g.w <= 1 || g.h <= 1 || sx + g.w > pw || sy + g.h > ph)
        {
            g.w = g.h = 0;   // a space: an advance alone
            continue;
        }
        if (penX + g.w + kAtlasPad > kAtlasWidth)
        {
            penX = kAtlasPad;
            penY += shelf + kAtlasPad;
            shelf = 0;
        }
        g.x = penX;
        g.y = penY;
        penX += g.w + kAtlasPad;
        shelf = SDL_max(shelf, g.h);
        places.push_back({ code, sx, sy });
    }
    int height = 1;
    while (height < penY + shelf + kAtlasPad)
        height <<= 1;
    out.fontW = kAtlasWidth;
    out.fontH = height;
    out.font.assign(std::size_t(kAtlasWidth) * height * 4, 0);
    for (const Place& p : places)
    {
        const HudGlyph& g = font.glyphs[p.code];
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                const int v = texel(p.sx + x, p.sy + y);
                std::uint8_t* o = &out.font[(std::size_t(g.y + y) * kAtlasWidth + g.x + x) * 4];
                o[0] = o[1] = o[2] = std::uint8_t(v * 17);
                o[3] = v ? 255 : 0;
            }
    }
    const HudGlyph& a = font.glyphs['A'];
    if (a.known && a.h > 0)
    {
        font.capTexels = float(a.h);
        font.capTop = float(a.top);
    }
    return true;
}

float hudTextAdvance(const HudFont& font, const char* text)
{
    float width = 0.0f;
    for (const char* c = text; *c; ++c)
    {
        const HudGlyph& g = font.glyphs[std::uint8_t(*c)].known ? font.glyphs[std::uint8_t(*c)] : font.glyphs[' '];
        width += float(g.advance);
    }
    return width;
}

void addHudText(std::vector<OverlayQuad>& quads, const OverlayArt& art, const HudFont& font, float x, float y,
                float scale, const char* text, const float colour[3], float alpha)
{
    const float fw = float(art.fontW), fh = float(art.fontH);
    float pen = x;
    for (const char* c = text; *c; ++c)
    {
        const HudGlyph& g = font.glyphs[std::uint8_t(*c)].known ? font.glyphs[std::uint8_t(*c)] : font.glyphs[' '];
        if (g.w > 0)
        {
            const float x0 = pen + float(g.left) * scale, y0 = y + float(g.top) * scale;
            quads.push_back({ x0, y0, x0 + float(g.w) * scale, y0 + float(g.h) * scale,
                              float(g.x) / fw, float(g.y) / fh, float(g.x + g.w) / fw, float(g.y + g.h) / fh,
                              colour[0], colour[1], colour[2], alpha, OverlayQuad::Glyph });
        }
        pen += float(g.advance) * scale;
    }
}

}  // namespace nfs3hp
