#include "native_frontend.h"
#include <cstdint>
#include <cstring>
#include <iterator>

/* The Controls screen's list of the keys a player drives with (a widget of
 * the type "controls", 0x14: control1.mnu and the race's pause screen
 * pscntl1.mnu), decompiled to take one row more than the game's thirteen --
 * Next Song, the port's (native_now_playing.cpp, NFS_NEXT_SONG_KEY) -- and to
 * scroll, the list already filling the screen:
 *
 *   sub_43c900   drawn: the heading, each row's bar, its key and its name, the
 *                row being bound blinking, the prompt while a key is waited for;
 *   sub_43cf80   a key: up and down move the pointer a row (the selection is
 *                the row under it), Enter waits for a key to bind, Escape
 *                gives the wait up.
 *
 * Thirteen rows show, as many as the game's; past them the list scrolls a row
 * at a time as up or down goes past its ends, and a yellow triangle of the
 * menus' own (mmnu.qfs: yup1, ydn1) just right of the first or last row
 * says there is more that way.  The game's thirteen are each player's control
 * set (sub_4904d0); Next Song is kept by the port.  With two players (split
 * screen, [0x6fd3b0] 1) the list has two columns and both stay the game's own:
 * the generated functions run. */
namespace nfs3hp
{

x86::reg32 nextSongKey();                    // native_now_playing.cpp
void setNextSongKey(x86::reg32 code);

namespace
{

using fe::Guest;
using fe::GuestString;

constexpr x86::reg32 kControlsType = 0x14;
constexpr x86::reg32 kSplitScreen = 0x6fd3b0;      // 1: two players' columns
constexpr x86::reg32 kInRace = 0x559234;           // the menus are a race's pause screens
// The widget's layout, set as it is made (sub_43d310).
constexpr x86::reg32 kRowHeight = 0x555608;
constexpr x86::reg32 kRowIndent = 0x55560c;        // a dword each row: the curve the rows follow
constexpr x86::reg32 kKeyColumn = 0x5f3d9c;        // where a row's key is, from its left
constexpr x86::reg32 kBarImage = 0x5f3daa;         // word: "con2", a row's bar
constexpr x86::reg32 kBlinkImage = 0x5f3da8;       // word: "con1", the row being bound, every other half second
// The widget's state.
constexpr x86::reg32 kFocusSeen = 0x555640;        // the pointer put on the list as it came to it
constexpr x86::reg32 kBlink = 0x555644;            // 0 to 30, frames
constexpr x86::reg32 kLastKey = 0x66464c;          // the key that brought the list its focus
constexpr x86::reg32 kKeptBinding = 0x5f3d90;      // the binding before the wait, for Escape
constexpr x86::reg32 kPadPolled = 0x5f3d94;        // byte: the controllers looked at during the wait
// An item's fields, words.
constexpr x86::reg32 kItemX = 6, kItemY = 8, kItemFlags = 5, kItemTop = 0x1a, kItemWidth = 0x1e, kBinding = 0x3c;
constexpr std::uint8_t kWaiting = 4;               // kItemFlags: a key is waited for
// The game's, called by address.
constexpr x86::reg32 kItemIs = 0x44a660;           // eax the item, edx a type
constexpr x86::reg32 kRowAt = 0x49c1c0;            // eax a top, edx a row's height: the pointer's row, -1 above
constexpr x86::reg32 kSetPointer = 0x49bfc0;       // eax x, edx y
constexpr x86::reg32 kControlsFor = 0x4904d0;      // eax the mode: the control sets, 13 dwords a player
constexpr x86::reg32 kText = 0x4d1850;             // eax a line of the language's
constexpr x86::reg32 kDrawText = 0x452190;         // (text, x, y, font, align, colour)
constexpr x86::reg32 kDrawImage = 0x4d7990;        // eax an image, edx x, ebx y
constexpr x86::reg32 kFindImage = 0x4d5db0;        // eax its name: the menus' image of it
constexpr x86::reg32 kDrawBinding = 0x43c110;      // (row's text, binding, x, y, selected): a key's name or icon
constexpr x86::reg32 kPollPads = 0x43be40;
constexpr x86::reg32 kQueueKey = 0x4cc8e0;         // eax, edx, ebx: a key into the menus' queue
constexpr x86::reg32 kSnapshotPads = 0x43d920;
constexpr x86::reg32 kBindingOf = 0x43cda0;        // eax the key: its binding, 0 for none
constexpr x86::reg32 kCheckClash = 0x43ce80;       // eax the row, edx the binding
constexpr x86::reg32 kClick = 0x4181d0;            // eax, edx 0x7f: the menus' sound
constexpr x86::reg32 kSprintf = 0x4df690;
constexpr std::uint32_t kFont = 0x12;
constexpr std::uint32_t kTextColour = 0xff649dfd, kPromptColour = 0xffe440;
constexpr std::uint32_t kHeadingText = 0xc8, kPromptText = 0x45;
constexpr std::uint16_t kKeyUp = 0x4800, kKeyDown = 0x5000, kKeyLeft = 0x4b00, kKeyRight = 0x4d00;
constexpr std::uint16_t kKeyEnter = 0xd, kKeyEscape = 0x1b;

constexpr GuestString kHeading{ 0x53776c, "%s 1" };
constexpr GuestString kStrings[] = { kHeading };

// The game's thirteen rows' names, in their order on screen (the jump table at 0x43c8c8).
constexpr std::uint32_t kRowText[] = { 0x47, 0x46, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4f, 0x4e, 0x51, 0x1bc, 0x50 };
constexpr int kGameRows = int(std::size(kRowText));
constexpr int kRows = kGameRows + 1;       // and Next Song
constexpr int kShown = kGameRows;          // as many as fill the screen
constexpr const char* kNextSongName = "Next song";
// The triangles: the menus' own, at the right of the first and last rows shown.
constexpr const char* kUpImage = "yup1";
constexpr const char* kDownImage = "ydn1";
constexpr int kTriangleSize = 12;
constexpr int kTriangleGap = 2;            // the triangle's left past the list's right edge (the pointer sits inside it)

int s_scroll = 0;            // the first row shown
x86::reg32 s_list = 0;       // the list it is of
bool s_nextWaiting = false;  // Next Song's key being waited for: shown as none

std::int32_t signedWord(const Guest& guest, x86::reg32 address)
{
    return std::int16_t(guest.word(address));
}

std::int32_t signedDword(const Guest& guest, x86::reg32 address)
{
    return std::int32_t(guest.dword(address));
}

bool single(const Guest& guest)
{
    return guest.dword(kSplitScreen) != 1;
}

void rememberList(x86::reg32 item)
{
    if (item != s_list)
    {
        s_list = item;
        s_scroll = 0;
    }
}

x86::reg32 bindingOf(const Guest& guest, x86::reg32 sets, int row)
{
    if (row < kGameRows)
        return guest.dword(sets + x86::reg32(row) * 4);
    return s_nextWaiting ? 0 : nextSongKey();
}

void setBinding(Guest& guest, x86::reg32 sets, int row, x86::reg32 binding)
{
    if (row < kGameRows)
        guest.setDword(sets + x86::reg32(row) * 4, binding);
}

/* The pointer to the middle of the row shown `shown`, at the list's right. */
void pointAt(Guest& guest, x86::reg32 item, int shown)
{
    const std::int32_t height = signedDword(guest, kRowHeight);
    const std::int32_t y = signedWord(guest, item + kItemY) + height * shown + height / 2;
    const std::int32_t x = signedWord(guest, item + kItemX) + signedWord(guest, item + kItemWidth) - 10;
    guest.call(kSetPointer, fe::args({ x86::reg32(x), x86::reg32(y) }));
}

/* A name of four letters into the frame, for kFindImage: its image, or -1. */
std::int32_t image(Guest& guest, const char* name)
{
    const x86::reg32 at = guest.frame();
    for (int k = 0; k < 5; ++k)
        guest.setByte(at + x86::reg32(k), std::uint8_t(name[k]));
    return std::int16_t(guest.call(kFindImage, fe::args({ at })));
}

/* sub_43c900: eax the list, edx whether it has the focus. */
x86::reg32 draw(Guest& guest, x86::reg32 item, x86::reg32 focused)
{
    if (guest.call(kItemIs, fe::args({ item, kControlsType })) == 0)
    {
        guest.leaveEdx(0);
        return 0;
    }
    rememberList(item);
    const bool inRace = guest.dword(kInRace) != 0;
    const std::int32_t height = signedDword(guest, kRowHeight);
    const std::int32_t left = signedWord(guest, item + kItemX), top = signedWord(guest, item + kItemY);
    if (focused != 0)
    {
        if (guest.dword(kFocusSeen) == 0)
        {
            // Come to from below: its last row, from above its first.
            const std::uint32_t key = guest.dword(kLastKey);
            if (key == kKeyUp || key == kKeyLeft)
            {
                s_scroll = kRows - kShown;
                pointAt(guest, item, kShown - 1);
            }
            if (key == kKeyDown || key == kKeyRight)
            {
                s_scroll = 0;
                pointAt(guest, item, 0);
            }
        }
        guest.setDword(kFocusSeen, 1);
    }
    else
    {
        guest.setDword(kFocusSeen, 0);
    }

    const std::int32_t waitingRow = signedWord(guest, item + kBinding);
    std::int32_t selected = -1;
    if (waitingRow != -1)
    {
        if (std::int8_t(guest.byte(kPadPolled)) == 0)
        {
            const x86::reg32 polled = guest.call(kPollPads);
            guest.setDword(kPadPolled, polled);
            if (std::int8_t(polled) != 0)
                guest.call(kQueueKey, fe::args({ 0xd, 0x1c, 0 }));
        }
        const x86::reg32 prompt = guest.call(kText, fe::args({ kPromptText }));
        guest.call(kDrawText, fe::args({ prompt, 0x140, inRace ? 0x41u : 0x55u, kFont }, { 2, kPromptColour }));
        selected = waitingRow;
    }
    else if (focused != 0)
    {
        const std::int32_t shown = std::int32_t(guest.call(kRowAt, fe::args({ x86::reg32(signedWord(guest, item + kItemTop)), x86::reg32(height) })));
        selected = shown >= 0 && shown < kShown ? shown + s_scroll : -1;
    }

    const x86::reg32 sets = guest.call(kControlsFor, fe::args({ guest.dword(kSplitScreen) }));
    const x86::reg32 heading = guest.call(kText, fe::args({ kHeadingText }));
    const x86::reg32 line = guest.frame();
    guest.call(kSprintf, fe::args({}, { line, kHeading.address, heading }));
    guest.call(kDrawText, fe::args({ line, x86::reg32(left + signedDword(guest, kKeyColumn)), x86::reg32(top - 0x1e), kFont },
                                   { 0, kTextColour }));

    std::int32_t y = top;
    for (int shown = 0; shown < kShown; ++shown, y += height)
    {
        const int row = shown + s_scroll;
        const std::int32_t indent = signedDword(guest, kRowIndent + x86::reg32(shown) * 4);
        if (!inRace)
        {
            const std::int32_t x = left + indent;
            guest.call(kDrawImage, fe::args({ x86::reg32(signedWord(guest, kBarImage)), x86::reg32(x), x86::reg32(y) }));
            if (waitingRow == row)
            {
                std::uint32_t blink = guest.dword(kBlink) + 1;
                guest.setDword(kBlink, blink);
                if (std::int32_t(blink) > 30)
                    guest.setDword(kBlink, 0);
                else if (std::int32_t(blink) > 15)
                    guest.call(kDrawImage, fe::args({ x86::reg32(signedWord(guest, kBlinkImage)), x86::reg32(x), x86::reg32(y) }));
            }
        }
        const x86::reg32 rowText = row < kGameRows ? kRowText[row] : 0;
        std::int32_t x = left;
        guest.call(kDrawBinding, fe::args({ rowText, bindingOf(guest, sets, row), x86::reg32(x), x86::reg32(y) },
                                          { row == selected ? 1u : 0u }));
        if (!inRace)
            x += indent + 0xe;
        x86::reg32 name;
        if (row < kGameRows)
        {
            name = guest.call(kText, fe::args({ rowText }));
        }
        else
        {
            // The port's row: its name in the frame (0x4c bytes, the heading done with).
            name = guest.frame();
            for (std::size_t k = 0; k <= std::strlen(kNextSongName); ++k)
                guest.setByte(name + x86::reg32(k), std::uint8_t(kNextSongName[k]));
        }
        guest.call(kDrawText, fe::args({ name, x86::reg32(x), x86::reg32(y), kFont }, { 0, kTextColour }));
    }

    // More above, more below.
    const std::int32_t triangleX = left + signedWord(guest, item + kItemWidth) + kTriangleGap;
    const std::int32_t inset = (height - kTriangleSize) / 2;
    if (s_scroll > 0)
    {
        const std::int32_t up = image(guest, kUpImage);
        if (up >= 0)
            guest.call(kDrawImage, fe::args({ x86::reg32(up), x86::reg32(triangleX), x86::reg32(top + inset) }));
    }
    if (s_scroll + kShown < kRows)
    {
        const std::int32_t down = image(guest, kDownImage);
        if (down >= 0)
            guest.call(kDrawImage, fe::args({ x86::reg32(down), x86::reg32(triangleX),
                                              x86::reg32(top + height * (kShown - 1) + inset) }));
    }
    guest.leaveEdx(0);
    return 0;
}

/* sub_43cf80: eax the list, ebx the key (a word).  2 when the list took it. */
x86::reg32 key(Guest& guest, x86::reg32 item, std::uint16_t key)
{
    const x86::reg32 sets = guest.call(kControlsFor, fe::args({ guest.dword(kSplitScreen), 0 }));
    x86::reg32 result = guest.edx();
    if (guest.call(kItemIs, fe::args({ item, kControlsType })) == 0)
        return 0;
    rememberList(item);
    const std::int32_t waitingRow = signedWord(guest, item + kBinding);
    if (waitingRow == -1)
    {
        guest.setByte(item + kItemFlags, guest.byte(item + kItemFlags) & ~kWaiting);
        const std::int32_t height = signedDword(guest, kRowHeight);
        std::int32_t shown = std::int32_t(guest.call(kRowAt, fe::args({ x86::reg32(signedWord(guest, item + kItemTop)), x86::reg32(height) })));
        if (key == kKeyDown)
        {
            guest.call(kClick, fe::args({ 0, 0x7f }));
            if (shown + 1 < kShown)
            {
                ++shown;
                result = 2;
                pointAt(guest, item, shown);
            }
            else if (shown == kShown - 1 && s_scroll + kShown < kRows)
            {
                ++s_scroll;   // the next row up into view, the pointer where it is
                result = 2;
                pointAt(guest, item, shown);
            }
        }
        if (key == kKeyUp)
        {
            guest.call(kClick, fe::args({ 0, 0x7f }));
            if (shown > 0)
            {
                --shown;
                result = 2;
                pointAt(guest, item, shown);
            }
            else if (shown == 0 && s_scroll > 0)
            {
                --s_scroll;
                result = 2;
                pointAt(guest, item, shown);
            }
        }
        if (key == kKeyEnter)
        {
            result = 2;
            const int row = shown + s_scroll;
            if (shown >= 0 && shown < kShown && row < kRows)
            {
                guest.call(kSnapshotPads);
                guest.setDword(kKeptBinding, bindingOf(guest, sets, row));
                setBinding(guest, sets, row, 0);
                s_nextWaiting = row >= kGameRows;
                guest.setWord(item + kBinding, std::uint16_t(row));
                guest.setByte(item + kItemFlags, guest.byte(item + kItemFlags) | kWaiting);
                guest.setDword(kPadPolled, 0);
            }
        }
        return result;
    }

    // A key waited for.
    guest.setByte(item + kItemFlags, guest.byte(item + kItemFlags) | kWaiting);
    if (key == kKeyEscape)
    {
        guest.setByte(item + kItemFlags, guest.byte(item + kItemFlags) & ~kWaiting);
        setBinding(guest, sets, waitingRow, guest.dword(kKeptBinding));
    }
    else
    {
        const x86::reg32 binding = guest.call(kBindingOf, fe::args({ x86::reg32(std::int32_t(std::int16_t(key))) }));
        if (binding == 0)
            return 2;
        if (waitingRow < kGameRows)
            guest.call(kCheckClash, fe::args({ x86::reg32(waitingRow), binding }));
        guest.setByte(item + kItemFlags, guest.byte(item + kItemFlags) & ~kWaiting);
        if (waitingRow < kGameRows)
            setBinding(guest, sets, waitingRow, binding);
        else
            setNextSongKey(binding);
    }
    s_nextWaiting = false;
    guest.setWord(item + kBinding, 0xffff);
    return 2;
}

}  // namespace

bool controlsDraw(win32::WinApplication* app, x86::CPU& cpu)
{
    static fe::Site site{ "sub_43c900", 0, 20 + 0x4c, kStrings, std::size(kStrings),
                          fe::kRestoreEbx | fe::kRestoreEcx | fe::kRestoreEsi | fe::kRestoreEdi };
    if (!single(Guest(app, cpu)))
        return false;
    const x86::reg32 item = cpu.eax, focused = cpu.edx;
    return fe::run(app, cpu, site, [&](Guest& guest) { return draw(guest, item, focused); });
}

bool controlsKey(win32::WinApplication* app, x86::CPU& cpu)
{
    static fe::Site site{ "sub_43cf80", 0, 16 + 0xc, nullptr, 0,
                          fe::kRestoreEcx | fe::kRestoreEsi | fe::kRestoreEdi };
    if (!single(Guest(app, cpu)))
        return false;
    const x86::reg32 item = cpu.eax;
    const std::uint16_t pressed = std::uint16_t(cpu.ebx);
    return fe::run(app, cpu, site, [&](Guest& guest) { return key(guest, item, pressed); });
}

}  // namespace nfs3hp
