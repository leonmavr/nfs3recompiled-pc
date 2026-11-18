#include <nfs3hp.h>
#include <lib/file.h>
#include <lib/overlay.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/* Port: the song a race plays, shown as it starts (NFS_NOW_PLAYING).
 *
 * A second into the race, a small panel slides in at the bottom left of the
 * picture -- the HUD's own navy, a yellow glow round its edge, the NFS emblem
 * of the game's HUD at its left -- with the artist in yellow and the song in
 * white, in the HUD's own font; it stays a few seconds and slides back out.
 *
 * Which song.  A race streams one file of gamedata/audio/pc,
 * <track><rock|tech>.mus (tools/apply_music_stream.py), named by the game
 * itself as it opens it: musicStreamFile hands the name here
 * (nowPlayingStream).  The ten files are the game's ten race songs; their
 * titles are the game's own (fedata/text/text.eng, lines 4066 to 4075), their
 * artists the soundtrack's credits.  No music, no panel: a race the game plays
 * without music never names a file.
 *
 * When.  By the race's clock ([0x7d3684], 64 a second): kDelay after it starts
 * running, so after the countdown; again if the clock goes back (the race
 * restarted), and at every race's start -- the same song again too: a race on
 * the track just raced may not name its file again, the stream already open,
 * so the last song named is kept from race to race.  The clock standing (a pause) hides the panel where it is.
 *
 * Its pictures, read once from the game's files: the HUD's font
 * (gamedata/dashhud/cp16bi.ffn -- a 4-bit picture of every character, the
 * edges dark) and the emblem (gamedata/dashhud/hud98.fsh, the picture "4444",
 * cut out round).  The renderer draws the panel over the finished frame,
 * HUD and all (ThrashRenderer::drawOverlay; include/lib/overlay.h). */
namespace nfs3hp
{

bool raceIsRunning();   // nfs3hp_main.cpp

namespace
{

namespace nowplaying
{
// Timing, seconds of the race's clock.
constexpr float kDelay = 1.0f;         // after the race's clock starts
constexpr float kSlideIn = 0.45f;
constexpr float kHoldDefault = 5.0f;   // NFS_NOW_PLAYING: how long it stays
constexpr float kHoldMost = 30.0f;
constexpr float kSlideOut = 0.45f;
constexpr Uint64 kPausedNs = 150 * SDL_NS_PER_MS;   // the clock standing this long: a pause
// Layout, HUD units (kOverlayUnits high).
constexpr float kLeft = 12.0f;         // the panel's left edge from the picture's
constexpr float kBottom = 14.0f;       // its bottom edge from the picture's
constexpr float kHeight = 22.0f;
constexpr float kRadius = 5.0f;        // its corners
constexpr float kGlow = 5.0f;          // how far its glow reaches out
constexpr float kIcon = 38.0f;         // the emblem's diameter
constexpr float kIconInto = 0.30f;     // its centre this much of its diameter into the panel
constexpr float kTextGap = 5.0f;       // between the emblem and the text
constexpr float kRightPad = 12.0f;     // after the text
constexpr float kCapHeight = 9.5f;     // a capital's height
// Colours, 0 to 1.
constexpr float kYellow[3] = { 1.0f, 0.80f, 0.12f };   // the artist, and the glow
constexpr float kWhite[3] = { 1.0f, 1.0f, 1.0f };      // the song
constexpr float kFill[3] = { 0.16f, 0.13f, 0.30f };    // the panel at its top (the HUD's navy)
constexpr float kOpacity = 0.7f;   // all of it -- panel, glow, emblem, text -- over the race
// The race.
constexpr x86::reg32 kRaceClock = 0x7d3684;
constexpr float kTicksPerSecond = 64.0f;
// The pictures.
constexpr const char* kFontFile = "gamedata/dashhud/cp16bi.ffn";
constexpr const char* kHudFile = "gamedata/dashhud/hud98.fsh";
constexpr int kEmblemX = 130, kEmblemY = 65, kEmblemSize = 64;   // in hud98.fsh's "4444"
constexpr float kEmblemRadius = 31.5f;   // texels; its edge eased over one
constexpr int kAtlasWidth = 512;         // the font's characters, laid out again apart
constexpr int kAtlasPad = 4;             // texels round each, so its smaller levels never bleed
}  // namespace nowplaying

struct Song
{
    const char* file;   // the stream's name, without its folder and extension
    const char* artist;
    const char* title;  // the game's spelling
};

constexpr Song kSongs[] = {
    { "homerock", "Saki Kaskas", "Little Sweaty Sow" },
    { "hometech", "Rom Di Prisco", "Hydrus 606" },
    { "lostrock", "Matt Ragan", "Snorkeling Cactus Weasels" },
    { "losttech", "Rom Di Prisco", "Cetus 808" },
    { "atlarock", "Matt Ragan", "Rear Flutterblast #19" },
    { "atlatech", "Rom Di Prisco", "Aquilla 303" },
    { "alpirock", "Matt Ragan", "Snow Bags" },
    { "alpitech", "Saki Kaskas", "Knossos" },
    { "emprrock", "Saki Kaskas", "Flimsy" },
    { "emprtech", "Alistair Hirst", "Warped" },
};

/* NFS_NOW_PLAYING: seconds it stays (on: kHoldDefault), 0 or off for never. */
float holdSeconds()
{
    static const float hold = [] {
        const char* value = SDL_getenv("NFS_NOW_PLAYING");
        if (!value || !*value || SDL_strcasecmp(value, "on") == 0)
            return nowplaying::kHoldDefault;
        if (SDL_strcasecmp(value, "off") == 0)
            return 0.0f;
        const float parsed = float(SDL_atof(value));
        if (parsed == 1.0f)
            return nowplaying::kHoldDefault;   // "1" is on
        return parsed >= 0.0f && parsed <= nowplaying::kHoldMost ? parsed : nowplaying::kHoldDefault;
    }();
    return hold;
}

struct Glyph
{
    int w = 0, h = 0;         // texels
    int x = 0, y = 0;         // where in the atlas
    int advance = 0, left = 0, top = 0;   // texels: to the next, and its offset from the pen and the line's top
    bool known = false;
};

struct Art
{
    std::shared_ptr<OverlayArt> pictures;
    Glyph glyphs[256];
    float capTexels = 18.0f, capTop = 5.0f;   // 'A': its height and offset from the line's top
};

const Song* s_song = nullptr;     // the race's, from its stream's name
bool s_armed = false;             // to be shown when the clock gets to kDelay
bool s_clockReset = false;        // the clock seen under kDelay since armed: this race's, not the last's
bool s_showing = false;
x86::sreg32 s_start = 0;          // the clock as it was shown
x86::sreg32 s_lastClock = 0;
Uint64 s_clockMovedNs = 0;
bool s_wasRunning = false;
bool s_overlaySet = false;
std::unique_ptr<Art> s_art;
bool s_artFailed = false;

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
        SDL_Log("[MUSIC] now playing: %s not read (%s)", path.c_str(), SDL_GetError());
    return bytes;
}

/* An .ffn: "FNTF", its characters' count (+8), the line's height (+0x12), the
 * picture's offset (+0x1c); from +0x20 a character each 11 bytes -- its code,
 * width and height, where in the picture, its advance and its offsets from
 * the pen and from the line's top; the picture a 16-byte header (0x7a, width
 * +4, height +6) and 4 bits a texel, the first in the high half.  Laid out
 * again here kAtlasPad apart, white, its alpha whether it is drawn at all:
 * the low values are the characters' dark edges. */
bool readFont(Art& art, OverlayArt& out)
{
    using namespace nowplaying;
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
        Glyph& g = art.glyphs[code];
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
        const Glyph& g = art.glyphs[p.code];
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
            {
                const int v = texel(p.sx + x, p.sy + y);
                std::uint8_t* o = &out.font[(std::size_t(g.y + y) * kAtlasWidth + g.x + x) * 4];
                o[0] = o[1] = o[2] = std::uint8_t(v * 17);
                o[3] = v ? 255 : 0;
            }
    }
    const Glyph& a = art.glyphs['A'];
    if (a.known && a.h > 0)
    {
        art.capTexels = float(a.h);
        art.capTop = float(a.top);
    }
    return true;
}

/* An .fsh ("SHPI"): its pictures' count (+8), from +16 a name and an offset
 * each 8 bytes; a picture's header 16 bytes -- 0x7d for 32-bit BGRA, its width
 * +4 and height +6.  The emblem cut out of "4444", round, premultiplied. */
bool readEmblem(OverlayArt& out)
{
    using namespace nowplaying;
    const std::vector<std::uint8_t> d = loadFile(kHudFile);
    if (d.size() < 16 || SDL_memcmp(d.data(), "SHPI", 4) != 0)
        return false;
    const int count = int(le32(&d[8]));
    std::size_t at = 0;
    for (int k = 0; k < count && 16 + std::size_t(k + 1) * 8 <= d.size(); ++k)
        if (SDL_memcmp(&d[16 + std::size_t(k) * 8], "4444", 4) == 0)
            at = le32(&d[20 + std::size_t(k) * 8]);
    if (!at || at + 16 > d.size() || (d[at] & 0x7f) != 0x7d)
        return false;
    const int w = le16(&d[at + 4]), h = le16(&d[at + 6]);
    if (at + 16 + std::size_t(w) * h * 4 > d.size() || kEmblemX + kEmblemSize > w || kEmblemY + kEmblemSize > h)
        return false;
    out.iconW = out.iconH = kEmblemSize;
    out.icon.assign(std::size_t(kEmblemSize) * kEmblemSize * 4, 0);
    const float centre = kEmblemSize * 0.5f;
    for (int y = 0; y < kEmblemSize; ++y)
        for (int x = 0; x < kEmblemSize; ++x)
        {
            const std::uint8_t* s = &d[at + 16 + (std::size_t(kEmblemY + y) * w + kEmblemX + x) * 4];
            const float r = std::hypot(x + 0.5f - centre, y + 0.5f - centre);
            const float a = float(s[3]) / 255.0f * SDL_clamp(kEmblemRadius - r + 0.5f, 0.0f, 1.0f);
            std::uint8_t* o = &out.icon[(std::size_t(y) * kEmblemSize + x) * 4];
            o[0] = std::uint8_t(s[2] * a + 0.5f);
            o[1] = std::uint8_t(s[1] * a + 0.5f);
            o[2] = std::uint8_t(s[0] * a + 0.5f);
            o[3] = std::uint8_t(a * 255.0f + 0.5f);
        }
    return true;
}

const Art* art()
{
    if (s_art || s_artFailed)
        return s_art.get();
    auto made = std::make_unique<Art>();
    made->pictures = std::make_shared<OverlayArt>();
    if (!readFont(*made, *made->pictures) || !readEmblem(*made->pictures))
    {
        SDL_Log("[MUSIC] now playing: the HUD's font or emblem could not be read; no panel");
        s_artFailed = true;
        return nullptr;
    }
    s_art = std::move(made);
    return s_art.get();
}

float easeOut(float t) { t = 1.0f - SDL_clamp(t, 0.0f, 1.0f); return 1.0f - t * t * t; }

void hide()
{
    if (s_overlaySet)
        win32::glide2x::overlay(nullptr);
    s_overlaySet = false;
}

/* The panel, its emblem and its text; shown 0 to 1: from past the picture's
 * left edge to where it rests. */
void show(const Art& a, const Song& song, float shown)
{
    using namespace nowplaying;
    auto overlay = std::make_shared<Overlay>();
    overlay->art = a.pictures;
    const float scale = kCapHeight / a.capTexels;   // HUD units a texel
    const OverlayArt& pics = *a.pictures;
    struct Run { const char* text; const float* colour; };
    const Run runs[] = { { song.artist, kYellow }, { " - ", kWhite }, { song.title, kWhite } };
    float textW = 0.0f;
    for (const Run& run : runs)
        for (const char* c = run.text; *c; ++c)
            textW += float(a.glyphs[std::uint8_t(*c)].known ? a.glyphs[std::uint8_t(*c)].advance
                                                             : a.glyphs[' '].advance) * scale;
    const float textAt = kIcon * kIconInto + kIcon * 0.5f + kTextGap;   // from the panel's left edge
    const float width = textAt + textW + kRightPad;
    const float away = kLeft + width + kGlow * 2.0f;   // all of it past the edge, glow and all
    const float left = kLeft - (1.0f - shown) * away;
    const float textX = left + textAt;
    const float top = kOverlayUnits - kBottom - kHeight;
    const float cx = left + width * 0.5f, cy = top + kHeight * 0.5f;
    overlay->panelHalfW = width * 0.5f;
    overlay->panelHalfH = kHeight * 0.5f;
    overlay->panelRadius = kRadius;
    overlay->panelGlow = kGlow;
    overlay->glowR = kYellow[0];
    overlay->glowG = kYellow[1];
    overlay->glowB = kYellow[2];
    const float reach = kGlow * 2.0f;
    overlay->quads.push_back({ left - reach, top - reach, left + width + reach, top + kHeight + reach,
                               left - reach - cx, top - reach - cy, left + width + reach - cx, top + kHeight + reach - cy,
                               kFill[0], kFill[1], kFill[2], kOpacity, OverlayQuad::Panel });
    const float ix = left + kIcon * kIconInto;
    overlay->quads.push_back({ ix - kIcon * 0.5f, cy - kIcon * 0.5f, ix + kIcon * 0.5f, cy + kIcon * 0.5f,
                               0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, kOpacity, OverlayQuad::Icon });
    // The line's top such that a capital sits in the panel's middle.
    const float lineTop = cy - kCapHeight * 0.5f - a.capTop * scale;
    float pen = textX;
    const float fw = float(pics.fontW), fh = float(pics.fontH);
    for (const Run& run : runs)
        for (const char* c = run.text; *c; ++c)
        {
            const Glyph& g = a.glyphs[std::uint8_t(*c)].known ? a.glyphs[std::uint8_t(*c)] : a.glyphs[' '];
            if (g.w > 0)
            {
                const float x0 = pen + float(g.left) * scale, y0 = lineTop + float(g.top) * scale;
                overlay->quads.push_back({ x0, y0, x0 + float(g.w) * scale, y0 + float(g.h) * scale,
                                           float(g.x) / fw, float(g.y) / fh, float(g.x + g.w) / fw,
                                           float(g.y + g.h) / fh, run.colour[0], run.colour[1], run.colour[2], kOpacity,
                                           OverlayQuad::Glyph });
            }
            pen += float(g.advance) * scale;
        }
    win32::glide2x::overlay(std::move(overlay));
    s_overlaySet = true;
}

}  // namespace

/* The stream's file as the game names it (musicStreamFile): which song. */
void nowPlayingStream(const char* path)
{
    const char* name = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/' || *c == '\\' || *c == ':')
            name = c + 1;
    char stem[32] = {};
    std::size_t n = 0;
    for (; name[n] && name[n] != '.' && n + 1 < sizeof stem; ++n)
        stem[n] = char(SDL_tolower(static_cast<unsigned char>(name[n])));
    s_song = nullptr;
    for (const Song& song : kSongs)
        if (SDL_strcmp(stem, song.file) == 0)
            s_song = &song;
    s_armed = s_song != nullptr;
    s_clockReset = false;
    s_showing = false;
    if (s_song)
        SDL_Log("[MUSIC] now playing: %s - %s", s_song->artist, s_song->title);
}

/* Once a frame (onSwap): when to show the panel, and where it is. */
void nowPlayingTick(win32::WinApplication* app)
{
    using namespace nowplaying;
    const float hold = holdSeconds();
    const bool running = raceIsRunning();
    if (hold <= 0.0f || !running)
    {
        if (!running)
            s_showing = false;
        s_wasRunning = running;
        hide();
        return;
    }
    if (!s_wasRunning)
    {
        // A race begins: its song shown, whether it named it now or before.
        s_armed = s_song != nullptr;
        s_clockReset = false;
        s_showing = false;
    }
    s_wasRunning = true;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const Uint64 now = SDL_GetTicksNS();
    if (clock != s_lastClock)
        s_clockMovedNs = now;
    if (clock < s_lastClock && s_song)
    {
        s_armed = true;   // restarted
        s_showing = false;
    }
    s_lastClock = clock;
    /* The last race's clock stands until the new race sets it going again:
     * shown only once it has been seen back under kDelay. */
    if (float(clock) < kDelay * kTicksPerSecond)
        s_clockReset = true;
    if (s_armed && s_clockReset && float(clock) >= kDelay * kTicksPerSecond)
    {
        s_armed = false;
        s_showing = true;
        s_start = clock;
        SDL_Log("[MUSIC] now playing shown: %s - %s", s_song ? s_song->artist : "?", s_song ? s_song->title : "?");
    }
    const Art* a = s_showing && s_song ? art() : nullptr;
    if (!a)
    {
        hide();
        return;
    }
    const float t = float(clock - s_start) / kTicksPerSecond;
    if (t > kSlideIn + hold + kSlideOut)
    {
        s_showing = false;
        hide();
        return;
    }
    if (now - s_clockMovedNs > kPausedNs)
    {
        hide();   // paused: back where it was when the clock runs again
        return;
    }
    // In from the left easing to rest, out to the left gathering speed.
    const float in = easeOut(t / kSlideIn);
    const float out = 1.0f - easeOut(1.0f - (t - kSlideIn - hold) / kSlideOut);
    const float shown = t < kSlideIn + hold ? in : 1.0f - out;
    show(*a, *s_song, shown);
}

}  // namespace nfs3hp
