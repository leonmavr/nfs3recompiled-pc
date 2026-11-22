#include <nfs3hp.h>
#include "car_slots.h"
#include "native_hud_font.h"
#include <lib/file.h>
#include <lib/overlay.h>
#include <winapi/glide2x.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* Port: the opponents' list, the port's own (NFS_OPPONENT_LIST, on).
 *
 * Where the game draws its standings table -- the panel of sub_481c50 and its
 * rows of sub_48ae70 -- the port draws a panel of its own: the minimap's navy
 * and faint grid, its metal corner pieces (native_minimap.cpp's furniture and
 * sprites), and a row a racer, in the game's own order -- by place, first
 * first -- with the place, who it is and the time it is behind.
 *
 * What it says.  The rows are the game's own table (0x7254f8 rows from
 * 0x7254fc, 24 bytes each): a racer's name, the colour the game gives it (its
 * driver's), the gap to the player in 1/64 s ticks (sub_485a60, from the eighth
 * of a second the race has run) with the colour the game gives it -- 0xffa01010
 * for the cars ahead of the player, 0xff008c00 for those behind, 0xfff0f0f0 for
 * the player, whose gap is 0 and is drawn as dashes -- and the car's id.  The
 * list cycles what sits between the place and the time every kColumnSeconds of
 * the race's clock: the driver's name, then the car.  The car is its folder
 * (the car's own copy at +0x1e4), named from its fedata.eng -- its make's first
 * three letters and its model, as much of the two as kCarNameMost characters
 * hold ("Jag XK8", "Lam Diablo S"); a folder with no text, or none to read, is
 * shown as it is.
 *
 * Where.  Over the table's own element -- 13 of the 23 a player -- from the
 * layout's box: the box's left edge, as tall as its rows (kRowUnits each), and
 * its top or its bottom, whichever the table keeps to (its flags' bit 0x10),
 * so the HUD options move it as they move the game's list and the HUD editor
 * grabs the rectangle the panel is drawn in.  The rectangle is the port's from
 * there on (written beside the drawing), which is what the editor frames and
 * moves: the list cannot be drawn a drag behind the frame, as the game's was
 * (the game placed its rows from numbers worked out for the last frame), and
 * nothing shifts as it is positioned.
 *
 * The game's own panel (sub_481c50) and rows (sub_48ae70) are not drawn while
 * the port's list is: hudTable's dispatcher and standingsRows, the hook at
 * sub_48ae70's top (tools/apply_native_render.py), both leave them out.  The
 * panel is then the minimap's, the HUD's navy at the same opacity as its map's
 * (kPanelAlpha, native_minimap.cpp) with its faint grid and metal corners, so
 * the race shows through it as it does through the map.
 *
 * Split screen keeps the game's list, as the minimap keeps the game's map:
 * one layer draws one list. */
namespace nfs3hp
{

bool raceIsRunning();                                                    // nfs3hp_main.cpp
bool minimapSprites(OverlayArt& out);                                     // native_minimap.cpp
void panelFurniture(std::vector<OverlayQuad>& quads, float x0, float y0, float x1, float y1, float alpha, float grid);

namespace
{

namespace standings
{
// The game's.
constexpr x86::reg32 kRows = 0x7254f8;          // the table's rows in use
constexpr x86::reg32 kTable = 0x7254fc;         // a row each 24 bytes
constexpr x86::reg32 kRowStride = 24;
constexpr x86::reg32 kRowName = 0x00;           // the driver's name, a string
constexpr x86::reg32 kRowNameColour = 0x04;     // and its colour, 0xAARRGGBB
constexpr x86::reg32 kRowGap = 0x08;            // the time to the one ahead, 1/64 s ticks, signed
constexpr x86::reg32 kRowGapColour = 0x0c;
constexpr x86::reg32 kRowArrested = 0x10;
constexpr x86::reg32 kRowCar = 0x14;            // the car's id
constexpr x86::reg32 kRacerFlags = 0x7256c4;    // the table's flags, 92 bytes a racer: & 0x10 keeps to the bottom
constexpr int kFlagBottom = 0x10;
constexpr x86::reg32 kRacers = 0x5efc48, kRacerCount = 0x5efda0;
constexpr x86::reg32 kPlayerIndex = 0x6fd4f4;
constexpr x86::reg32 kPlayerCars = 0x5efa48;
constexpr x86::reg32 kCarRecord = 0x220, kRecordId = 0x14, kCarFolder = 0x1e4;
constexpr x86::reg32 kCarFlags = 0x200;
constexpr x86::reg8 kCopBit = 0x20;
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;
constexpr x86::reg32 kRaceClock = 0x7d3684;
constexpr x86::reg32 kHudRects = 0x749758;
constexpr x86::reg32 kHudRectsPerPlayer = 23;
constexpr x86::reg32 kTableElement = 13;
constexpr x86::reg32 kCarSlotName = 0x110;      // a loaded car's folder, in the game's own record
constexpr x86::reg32 kLayout = 0x6fbc74;         // the elements' place as fractions, 16 bytes each
constexpr x86::reg32 kLayoutStride = 0x348;      // a player's layouts, 0x1a4 each
constexpr x86::reg32 kHudWidth = 0x7cdac8;       // the HUD's own, in pixels
constexpr x86::reg32 kHudHeight = 0x7cdacc;
constexpr x86::reg32 kTableWidth = 0x7925c8;     // the table's, as sub_47ee80 worked it out
constexpr int kMostRows = 16;
constexpr float kTicksPerSecond = 64.0f;
// The look.
constexpr float kPanelAlpha = 0.62f;     // the minimap's own (native_minimap.cpp's kPanelAlpha)
constexpr float kGrid = 12.0f;           // HUD units between its grid's lines, 0 for none
constexpr float kPad = 5.0f;             // HUD units inside it
constexpr float kRowUnits = 13.0f;       // a row's height, HUD units at most
constexpr float kCapMost = 8.0f;         // a capital's height, HUD units
constexpr float kRowFill = 0.55f;        // of its height a capital takes
constexpr float kLeastWidth = 84.0f;     // HUD units, the panel's at least
constexpr float kColumnSeconds = 3.2f;   // how long each of the two columns is shown
constexpr int kCarNameMost = 12;         // characters of a car's make and model
constexpr float kTintLeast = 0.55f;   // a dark name's brightest channel lifted to this
constexpr float kWhite[3] = { 1.0f, 1.0f, 1.0f };
constexpr float kDashes[3] = { 0.62f, 0.62f, 0.72f };
const Uint64 kPausedNs = 150 * SDL_NS_PER_MS;   // the clock standing this long: a pause
}  // namespace standings

struct Art
{
    std::shared_ptr<OverlayArt> pictures;   // the minimap's sprites and the HUD's font
    HudFont font;
};

std::unique_ptr<Art> s_art;
bool s_artTried = false;
x86::sreg32 s_lastClock = 0;
Uint64 s_clockMovedNs = 0;
std::map<std::string, std::string> s_carNames;   // a folder's make and model, as read

bool enabled()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_OPPONENT_LIST");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return on;
}

const Art* art()
{
    if (s_art || s_artTried)
        return s_art.get();
    s_artTried = true;
    auto made = std::make_unique<Art>();
    made->pictures = std::make_shared<OverlayArt>();
    if (!readHudFont(*made->pictures, made->font) || !minimapSprites(*made->pictures))
    {
        SDL_Log("[LIST] the HUD's font or the minimap's sprites could not be read; the game's list is kept");
        return nullptr;
    }
    s_art = std::move(made);
    return s_art.get();
}

std::string guestText(win32::WinApplication* app, x86::reg32 at, int most = 32)
{
    std::string text;
    for (int i = 0; at && i < most; ++i)
    {
        const char c = app->getMemory<char>(at + x86::reg32(i));
        if (!c)
            break;
        text.push_back(c);
    }
    return text;
}

std::string lowerCase(std::string text)
{
    for (char& c : text)
        c = char(SDL_tolower(static_cast<unsigned char>(c)));
    return text;
}

/* A car's make and model, from its folder's fedata.eng: its code, make, model
 * and full name, then its figures, as runs of letters between the header's
 * bytes -- tools/import_car.py's words.  Three letters of the make and the
 * model, as much of the two as kCarNameMost holds; the folder itself where
 * there is no text to read. */
const std::string& carName(const std::string& folder)
{
    using namespace standings;
    const auto known = s_carNames.find(folder);
    if (known != s_carNames.end())
        return known->second;
    std::string made = lowerCase(folder);
    const std::string path = win32::File::dataDirectory() + "gamedata/carmodel/" + made + "/fedata.eng";
    std::size_t size = 0;
    if (void* data = SDL_LoadFile(path.c_str(), &size))
    {
        std::vector<std::string> words;
        std::string run;
        for (std::size_t i = 0; i <= size; ++i)
        {
            const char c = i < size ? static_cast<const char*>(data)[i] : '\0';
            const bool printed = c >= 0x20 && c <= 0x7e;
            if (printed)
            {
                run.push_back(c);
                continue;
            }
            while (!run.empty() && run.front() == ' ')
                run.erase(run.begin());
            while (!run.empty() && run.back() == ' ')
                run.pop_back();
            if (run.size() >= 3)
                words.push_back(run);
            run.clear();
        }
        SDL_free(data);
        if (words.size() >= 3)
        {
            made = words[1].substr(0, 3);
            while (!made.empty() && made.back() == ' ')
                made.pop_back();
            if (!made.empty())
                made += ' ';
            made += words[2];
            if (int(made.size()) > kCarNameMost)
                made.resize(std::size_t(kCarNameMost));
            while (!made.empty() && made.back() == ' ')
                made.pop_back();
        }
    }
    return s_carNames.emplace(folder, made).first->second;
}

/* The folder of the car with this id, among the race's (its own copy of the
 * name at +0x1e4), or the game's record of the car by id. */
std::string carFolder(win32::WinApplication* app, x86::reg32 id)
{
    using namespace standings;
    const x86::reg32 count = SDL_min(app->getMemory<x86::reg32>(kRacerCount), x86::reg32(kMostRows));
    for (x86::reg32 k = 0; k < count; ++k)
    {
        const x86::reg32 car = app->getMemory<x86::reg32>(kRacers + k * 4);
        if (!car)
            continue;
        if (app->getMemory<x86::reg32>(car + kCarRecord + kRecordId) == id)
        {
            const std::string folder = guestText(app, car + kCarFolder);
            if (!folder.empty())
                return folder;
        }
    }
    if (const x86::reg32 record = carSlotRecord(id))
        return guestText(app, record + kCarSlotName);
    return std::string();
}

std::string gapText(x86::sreg32 ticks)
{
    using namespace standings;
    if (ticks == 0)
        return "-----------";
    const char sign = ticks > 0 ? '-' : '+';
    const x86::sreg32 abs = ticks > 0 ? ticks : -ticks;
    char text[24];
    SDL_snprintf(text, sizeof text, "%c %d:%02d.%02d", sign, int(abs / (x86::sreg32(kTicksPerSecond) * 60)),
                 int((abs / x86::sreg32(kTicksPerSecond)) % 60), int((abs % x86::sreg32(kTicksPerSecond)) * 100
                                                                       / x86::sreg32(kTicksPerSecond)));
    return text;
}

void colour(const x86::reg32 packed, float rgb[3])
{
    using namespace standings;
    if ((packed & 0xffffff) == 0)
    {
        rgb[0] = rgb[1] = rgb[2] = 1.0f;
        return;
    }
    rgb[0] = float((packed >> 16) & 0xff) / 255.0f;
    rgb[1] = float((packed >> 8) & 0xff) / 255.0f;
    rgb[2] = float(packed & 0xff) / 255.0f;
    // a dark driver's colour is unreadable on the HUD's navy: lifted, its hue kept
    const float most = SDL_max(rgb[0], SDL_max(rgb[1], rgb[2]));
    if (most > 0.0f && most < kTintLeast)
    {
        const float lift = kTintLeast / most;
        for (int k = 0; k < 3; ++k)
            rgb[k] = SDL_min(1.0f, rgb[k] * lift);
    }
}

/* The table's row count, or 0 when the port's list is not the one to draw: a
 * race, one player, not a cop's speeders, a table of more than one. */
x86::reg32 rowsWanted(win32::WinApplication* app)
{
    using namespace standings;
    const x86::reg32 player = app->getMemory<x86::reg32>(kPlayerIndex);
    if (player >= 2 || app->getMemory<x86::reg32>(kSplitScreen) == 1)
        return 0;
    const x86::reg32 car = app->getMemory<x86::reg32>(kPlayerCars + player * 4);
    if (!car || (app->getMemory<x86::reg8>(car + kCarFlags) & kCopBit))
        return 0;   // the speeders' list is the game's
    return SDL_min(app->getMemory<x86::reg32>(kRows), x86::reg32(kMostRows));
}

/* The panel, in HUD units: the element's box, as wide as the game's own table
 * and as tall as its rows, kept to the box's top or bottom. */
bool panelOf(win32::WinApplication* app, float& x0, float& y0, float& x1, float& y1)
{
    using namespace standings;
    const x86::reg32 rows = rowsWanted(app);
    if (rows < 2)
        return false;   // the game draws none either
    const x86::sreg32 hudW = x86::sreg32(app->getMemory<x86::reg32>(kHudWidth));
    const x86::sreg32 hudH = x86::sreg32(app->getMemory<x86::reg32>(kHudHeight));
    if (hudW <= 0 || hudH <= 0)
        return false;
    const x86::reg32 player = app->getMemory<x86::reg32>(kPlayerIndex);
    const x86::reg32 at = kLayout + player * kLayoutStride + kTableElement * 16;
    const float top = app->getMemory<float>(at), left = app->getMemory<float>(at + 4);
    const float bottom = app->getMemory<float>(at + 8), right = app->getMemory<float>(at + 12);
    if (!(right > left) || !(bottom > top))
        return false;
    const float unit = kOverlayUnits / float(hudH);
    const float bx0 = left * float(hudW) * unit;
    const float by0 = top * float(hudH) * unit;
    const float bx1 = right * float(hudW) * unit;
    const float by1 = bottom * float(hudH) * unit;
    const float room = (by1 - by0 - 2.0f * kPad) / float(rows);
    const float rowH = SDL_clamp(room, 3.0f, kRowUnits);
    const float height = float(rows) * rowH + 2.0f * kPad;
    const float tableW = float(x86::sreg32(app->getMemory<x86::reg32>(kTableWidth))) * unit;
    float width = tableW > 0 ? tableW + 2.0f * kPad : bx1 - bx0;
    width = SDL_min(width, bx1 - bx0);
    width = SDL_max(width, SDL_min(kLeastWidth, bx1 - bx0));
    const x86::reg8 flags = app->getMemory<x86::reg8>(kRacerFlags + player * 92);
    x0 = bx0;
    x1 = bx0 + width;
    if (flags & kFlagBottom)
    {
        y1 = by1;
        y0 = by1 - height;
    }
    else
    {
        y0 = by0;
        y1 = by0 + height;
    }
    return x1 > x0 && y1 > y0;
}

/* The element's rectangle made the panel's, for the editor to frame and drag. */
void writeRect(win32::WinApplication* app, float x0, float y0, float x1, float y1)
{
    using namespace standings;
    const x86::sreg32 hudH = x86::sreg32(app->getMemory<x86::reg32>(kHudHeight));
    if (hudH <= 0)
        return;
    const float toPixels = float(hudH) / kOverlayUnits;
    const x86::reg32 player = app->getMemory<x86::reg32>(kPlayerIndex);
    const x86::reg32 rect = kHudRects + player * kHudRectsPerPlayer * 16 + kTableElement * 16;
    app->getMemory<x86::reg32>(rect) = x86::reg32(SDL_lround(x0 * toPixels));
    app->getMemory<x86::reg32>(rect + 4) = x86::reg32(SDL_lround(y0 * toPixels));
    app->getMemory<x86::reg32>(rect + 8) = x86::reg32(SDL_lround(x1 * toPixels));
    app->getMemory<x86::reg32>(rect + 12) = x86::reg32(SDL_lround(y1 * toPixels));
}

void draw(win32::WinApplication* app, float x0, float y0, float x1, float y1)
{
    using namespace standings;
    const x86::reg32 rows = rowsWanted(app);
    const Art* a = art();
    if (!a)
        return;
    auto overlay = std::make_shared<Overlay>();
    overlay->art = a->pictures;
    panelFurniture(overlay->quads, x0, y0, x1, y1, kPanelAlpha, kGrid);
    const float rowH = (y1 - y0 - 2.0f * kPad) / float(rows);
    const float cap = SDL_min(kCapMost, rowH * kRowFill);
    const float scale = cap / a->font.capTexels;
    const float left = x0 + kPad;
    const float right = x1 - kPad;
    const float turns = hudTextAdvance(a->font, "88") * scale;   // room for two places
    const float whoX = left + turns + kPad * 0.5f;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    const int column = int(float(clock) / kTicksPerSecond / kColumnSeconds) % 2;
    const x86::reg32 count = SDL_min(app->getMemory<x86::reg32>(kRacerCount), x86::reg32(kMostRows));
    char place[8];
    for (x86::reg32 r = 0; r < rows; ++r)
    {
        const x86::reg32 rec = kTable + r * kRowStride;
        const float lineTop = y0 + kPad + rowH * float(r) + (rowH - cap) * 0.5f - a->font.capTop * scale;
        float name[3], gapColour[3];
        colour(app->getMemory<x86::reg32>(rec + kRowNameColour), name);
        colour(app->getMemory<x86::reg32>(rec + kRowGapColour), gapColour);
        const x86::reg32 id = app->getMemory<x86::reg32>(rec + kRowCar);
        std::string folder;
        for (x86::reg32 k = 0; k < count; ++k)
        {
            const x86::reg32 car = app->getMemory<x86::reg32>(kRacers + k * 4);
            if (!car || app->getMemory<x86::reg32>(car + kCarRecord + kRecordId) != id)
                continue;
            folder = guestText(app, car + kCarFolder);
            break;
        }
        if (folder.empty())
            folder = carFolder(app, id);
        // the place, against the left edge
        SDL_snprintf(place, sizeof place, "%d", int(r) + 1);
        const float placeX = left + turns - hudTextAdvance(a->font, place) * scale;
        addHudText(overlay->quads, *a->pictures, a->font, placeX, lineTop, scale, place, name, 0.85f);
        // and the time to the one ahead, against the right
        const std::string gap = gapText(x86::sreg32(app->getMemory<x86::reg32>(rec + kRowGap)));
        const float gapX = right - hudTextAdvance(a->font, gap.c_str()) * scale;
        addHudText(overlay->quads, *a->pictures, a->font, gapX, lineTop, scale, gap.c_str(), gapColour, 1.0f);
        // who: the driver, then the car
        std::string shown = guestText(app, app->getMemory<x86::reg32>(rec + kRowName));
        if (column == 1 && !folder.empty())
            shown = carName(folder);
        // as much of it as the room between the place and the time holds
        while (!shown.empty() && hudTextAdvance(a->font, shown.c_str()) * scale > gapX - kPad - whoX)
            shown.pop_back();
        addHudText(overlay->quads, *a->pictures, a->font, whoX, lineTop, scale, shown.c_str(), name, 1.0f);
    }
    win32::glide2x::overlayFrame(OverlayStandings, std::move(overlay));
}

}  // namespace

/* Once a frame (onSwap): the list in a race, on its own rectangle. */
void standingsTick(win32::WinApplication* app)
{
    using namespace standings;
    float x0, y0, x1, y1;
    if (!enabled() || !raceIsRunning() || !panelOf(app, x0, y0, x1, y1))
        return;
    const x86::sreg32 clock = x86::sreg32(app->getMemory<x86::reg32>(kRaceClock));
    if (clock != s_lastClock)
        s_clockMovedNs = SDL_GetTicksNS();
    s_lastClock = clock;
    if (SDL_GetTicksNS() - s_clockMovedNs > kPausedNs)
        return;   // a pause: the overlay would sit over its menu, and the editor moves the rectangle
    writeRect(app, x0, y0, x1, y1);
    draw(app, x0, y0, x1, y1);
}

/* Whether the game's own table is the port's to take the place of (hudTable's
 * dispatcher and the rows' hook ask, native_render.cpp). */
bool standingsDraws(win32::WinApplication* app)
{
    float x0, y0, x1, y1;
    return enabled() && raceIsRunning() && art() && panelOf(app, x0, y0, x1, y1);
}

/* The racers' rows, sub_48ae70: not drawn when the port's list is. */
bool standingsRows(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!standingsDraws(app))
        return false;
    cpu.esp += 4 + 0x10;   // the return address and its four arguments
    return true;
}

}  // namespace nfs3hp
