#include <nfs3hp.h>
#include "native_guest.h"
#include "native_radio.h"
#include "native_settings.h"
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
 * HUD and all (ThrashRenderer::drawOverlay; include/lib/overlay.h).
 *
 * The next song (NFS_NEXT_SONG_KEY, ] by default; Controls lists it as Next
 * Song, native_frontend_controls.cpp): pressed in a race, its music stops and
 * the next of the ten starts from the top, as the game starts a race's music
 * -- faded out over the frames that follow, sub_4109d0 then, and
 * sub_4107c0 with the track, the race music's volume and its form, as the
 * audio screen does when it turns the music back up -- and
 * the name it builds is that song's instead of the track's own
 * (nowPlayingOverride, read by musicIndexOpened): its index file and its
 * stream both.  The panel shows it at once.  Calling the game needs the
 * guest's own stack: done right after a frame's swap, from the swap's call
 * (glide2x::setSwapGuestObserver), every register put back after.
 *
 * The radio (native_radio.h): with the player's own MP3s in USER/MUSIC the
 * music played is one of theirs, and it is that track the panel names -- its
 * artist and song as its tags or its file name give them -- and the key moves
 * on to the next of the folder's, not to the next of the game's ten.  Every
 * track it starts is shown, mid-race too: one playing out brings the next,
 * and the panel says which.  With none of the folder's playing -- no MP3s, a
 * README among them, or NFS_RADIO=0 -- the game's ten songs are as they were,
 * names and all. */
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
// The next song.
constexpr x86::reg32 kKeys = 0x564818;          // a byte a scan code: held
constexpr x86::reg32 kTrack = 0x6fd4ac;
constexpr x86::reg32 kRaceMusicVolume = 0x6fbbf0;
constexpr x86::reg32 kSoundOutput = 0x6fbbe4;   // above 2, the race's music in its other form
constexpr x86::reg32 kMusicStreaming = 0x5525a0;   // set by sub_4107c0, cleared by sub_4109d0
constexpr x86::reg32 kStopMusic = 0x4109d0;
constexpr x86::reg32 kStartRaceMusic = 0x4107c0;   // eax the track, edx the volume, ebx the form
constexpr x86::reg32 kStreamVolume = 0x4e7470;     // SNDstreamautovol: eax the stream, edx the volume, faded to
constexpr x86::reg32 kStreamLevel = 0x4e74f0;      // the music's volume as it fades; 0 or less when out
constexpr x86::reg32 kMusicStream = 0x64;          // the race music's stream, as sub_4109d0 names it
constexpr Uint64 kFadeMostNs = 3 * SDL_NS_PER_SECOND;   // a fade not done by then, cut
constexpr x86::reg32 kKeyboard = 4;             // a binding's low byte: the keyboard's
constexpr x86::reg32 kDefaultKey = 0x5d001b04;  // ']': its character, its scan code, the keyboard
constexpr const char* kKeySetting = "NFS_NEXT_SONG_KEY";
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
std::string s_artist, s_title;    // what the panel says: the radio's track's, or that song's
unsigned s_generation = 0;        // the radio's, as last shown
const char* s_override = nullptr; // the next song's file, for the one start of the music that asks for it
bool s_keyHeld = false;           // the next song's key, as last seen
int s_skips = 0;                  // songs on from this one, the music fading out meanwhile
Uint64 s_fadeStartNs = 0;
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
void show(const Art& a, const char* artist, const char* title, float shown)
{
    using namespace nowplaying;
    auto overlay = std::make_shared<Overlay>();
    overlay->art = a.pictures;
    const float scale = kCapHeight / a.capTexels;   // HUD units a texel
    const OverlayArt& pics = *a.pictures;
    struct Run { const char* text; const float* colour; };
    std::vector<Run> runs;
    if (artist && *artist)
    {
        runs.push_back({ artist, kYellow });
        runs.push_back({ " - ", kWhite });
    }
    runs.push_back({ title ? title : "", kWhite });
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

/* The binding, as the game writes one: the character, the scan code, the device. */
x86::reg32 readKey()
{
    const char* value = SDL_getenv(nowplaying::kKeySetting);
    if (!value || !*value)
        return nowplaying::kDefaultKey;
    return x86::reg32(SDL_strtoul(value, nullptr, 0));
}

x86::reg32 s_key = 0;
bool s_keyRead = false;

}  // namespace

x86::reg32 nextSongKey()
{
    if (!s_keyRead)
    {
        s_key = readKey();
        s_keyRead = true;
    }
    return s_key;
}

void setNextSongKey(x86::reg32 code)
{
    s_key = code;
    s_keyRead = true;
    char text[16];
    SDL_snprintf(text, sizeof text, "0x%08x", unsigned(code));
    settingsSet(nowplaying::kKeySetting, text);
    settingsSave();
}

/* The song to open instead of the track's, while the next one starts. */
const char* nowPlayingOverride()
{
    return s_override;
}

/* Right after a frame's swap, from the swap's call into the port: the next
 * song's key pressed in a race whose clock runs, its music on, fades the
 * music out over the next frames -- the game's own stop waits for that fade
 * (sub_4109d0: SNDstreamautovol to nothing, then the timers run until it is
 * done), which held the game a second -- and once it is out stops it, the
 * wait done with, and starts the next song.  Pressed again meanwhile, the
 * song after that.  With the radio playing it is the next of the player's own
 * tracks, and nothing of the game's: the one playing gives way at once, its
 * frames the port's, and no fade is the game's to wait for. */
void nowPlayingSwap(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace nowplaying;
    const x86::reg32 key = nextSongKey();
    const bool keyboard = (key & 0xff) == kKeyboard;
    const bool held = keyboard && app->getMemory<x86::reg8>(kKeys + ((key >> 8) & 0xff)) != 0;
    const bool pressed = held && !s_keyHeld;
    s_keyHeld = held;
    const bool paused = SDL_GetTicksNS() - s_clockMovedNs > kPausedNs;
    /* The radio's next track, which needs nothing of the game's: not the
     * stream it never reads, nor the flag that says it is streaming. */
    if (raceIsRunning() && radio::wanted())
    {
        if (pressed && !paused)
            radio::skip();   // the folder's next: the panel shows it, its own track
        return;
    }
    if (!raceIsRunning() || !s_song || app->getMemory<x86::reg32>(kMusicStreaming) == 0)
    {
        s_skips = 0;
        return;
    }
    const x86::CPU kept = cpu;
    if (pressed && !paused)
    {
        if (s_skips == 0)
        {
            cpu.eax = kMusicStream;
            cpu.edx = 0;
            if (!guest::call(app, cpu, kStreamVolume))
                return;   // the game stopped in it: as it is
            cpu = kept;
            s_fadeStartNs = SDL_GetTicksNS();
        }
        ++s_skips;
    }
    if (s_skips == 0)
        return;
    // Faded out yet?  The stream's volume, as the game's stop reads it.
    if (!guest::call(app, cpu, kStreamLevel))
        return;
    const bool out = x86::sreg32(cpu.eax) <= 0 || SDL_GetTicksNS() - s_fadeStartNs > kFadeMostNs;
    cpu = kept;
    if (!out)
        return;
    std::size_t at = 0;
    for (std::size_t k = 0; k < std::size(kSongs); ++k)
        if (&kSongs[k] == s_song)
            at = k;
    s_override = kSongs[(at + std::size_t(s_skips)) % std::size(kSongs)].file;
    s_skips = 0;
    const x86::reg32 otherForm = x86::sreg32(app->getMemory<x86::reg32>(kSoundOutput)) > 2 ? 1 : 0;
    bool ok = guest::call(app, cpu, kStopMusic);
    if (ok)
    {
        cpu.eax = app->getMemory<x86::reg32>(kTrack);
        cpu.edx = app->getMemory<x86::reg32>(kRaceMusicVolume);
        cpu.ebx = otherForm;
        ok = guest::call(app, cpu, kStartRaceMusic);
    }
    s_override = nullptr;
    if (!ok)
        return;
    cpu = kept;
    s_clockReset = true;   // shown now, not at the next race's start
}

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

/* The song the music is playing now: the radio's track where it has one, else
 * the game's own, named as its stream's file named it.  False when there is
 * neither to name. */
bool names()
{
    if (radio::wanted())
    {
        s_artist = radio::artist();
        s_title = radio::title();
    }
    else if (s_song)
    {
        s_artist = s_song->artist;
        s_title = s_song->title;
    }
    else
        return false;
    return !s_artist.empty() || !s_title.empty();
}

/* Once a frame (onSwap): when to show the panel, and where it is. */
void nowPlayingTick(win32::WinApplication* app)
{
    using namespace nowplaying;
    radio::tick();   // the player's own tracks, if a race is to have them
    const float hold = holdSeconds();
    const bool running = raceIsRunning();
    if (running)
    {
        // Whether the race's clock runs (the next song waits out a pause too).
        const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
        if (clock != s_lastClock)
            s_clockMovedNs = SDL_GetTicksNS();
    }
    if (hold <= 0.0f || !running)
    {
        if (!running)
            s_showing = false;
        else
            s_lastClock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
        s_wasRunning = running;
        hide();
        return;
    }
    if (!s_wasRunning)
    {
        // A race begins: its song shown, whether it named it now or before.
        s_armed = s_song != nullptr || radio::wanted();
        s_clockReset = false;
        s_showing = false;
    }
    s_wasRunning = true;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const Uint64 now = SDL_GetTicksNS();
    if (clock < s_lastClock && (s_song || radio::wanted()))
    {
        s_armed = true;   // restarted
        s_showing = false;
    }
    /* Another of the radio's tracks -- the next one asked for, or the one
     * before it played out: named as it starts, mid-race as at a race's. */
    if (const unsigned generation = radio::generation(); generation != s_generation)
    {
        s_generation = generation;
        if (radio::wanted())
        {
            s_armed = true;
            s_clockReset = true;
            s_showing = false;
        }
    }
    s_lastClock = clock;
    /* The last race's clock stands until the new race sets it going again:
     * shown only once it has been seen back under kDelay. */
    if (float(clock) < kDelay * kTicksPerSecond)
        s_clockReset = true;
    if (s_armed && s_clockReset && float(clock) >= kDelay * kTicksPerSecond)
    {
        s_armed = false;
        if (names())
        {
            s_showing = true;
            s_start = clock;
            SDL_Log("[MUSIC] now playing shown: %s - %s", s_artist.c_str(), s_title.c_str());
        }
    }
    else if (s_showing && !names())
        s_showing = false;   // the music stopped: nothing to name
    const Art* a = s_showing ? art() : nullptr;
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
    show(*a, s_artist.c_str(), s_title.c_str(), shown);
}

}  // namespace nfs3hp
