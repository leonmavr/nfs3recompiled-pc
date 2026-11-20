#include "native_radio_tags.h"
#include <cstring>
#include <vector>

/* Port: the names a track in USER/MUSIC carries (native_radio_tags.h).
 *
 * the ID3v2 tag at the file's start -- "ID3", its version, its twenty-eight
 * bits of synchsafe size and its frames -- where it has one, else ID3v1's
 * "TAG" and its hundred and twenty-eight bytes at the end, else the file's
 * own name.  The text is turned into UTF-8: the tag's frames say which way
 * they wrote theirs, and ID3v1's is ISO-8859-1. */
namespace nfs3hp
{
namespace radio
{
namespace
{

constexpr std::uint8_t kUnsynchronised = 0x80;   // an ID3v2 header's flags
constexpr std::uint8_t kExtended = 0x40;
constexpr std::uint8_t kFooter = 0x10;           // v2.4's

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

/* A tag's sizes: seven bits a byte, the top one always clear. */
std::uint32_t synchsafe32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0] & 0x7f) << 21) | (std::uint32_t(p[1] & 0x7f) << 14)
         | (std::uint32_t(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

void appendUtf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80)
        out += char(code);
    else if (code < 0x800)
    {
        out += char(0xc0 | (code >> 6));
        out += char(0x80 | (code & 0x3f));
    }
    else if (code < 0x10000)
    {
        out += char(0xe0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3f));
        out += char(0x80 | (code & 0x3f));
    }
    else
    {
        out += char(0xf0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3f));
        out += char(0x80 | ((code >> 6) & 0x3f));
        out += char(0x80 | (code & 0x3f));
    }
}

bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

void trim(std::string& text, bool both)
{
    std::size_t end = text.size();
    while (end > 0 && blank(text[end - 1]))
        --end;
    text.resize(end);
    if (!both)
        return;
    std::size_t start = 0;
    while (start < text.size() && blank(text[start]))
        ++start;
    text.erase(0, start);
}

/* ISO-8859-1, as ID3v1 writes it and a v2 frame may. */
std::string latin1(const std::uint8_t* p, std::size_t size)
{
    std::string out;
    for (std::size_t i = 0; i < size && p[i]; ++i)
        appendUtf8(out, p[i]);
    trim(out, true);
    return out;
}

/* A text frame's own bytes, its first saying how they are written: 0
 * ISO-8859-1, 1 UTF-16 (its byte order mark first), 2 UTF-16 big end first, 3
 * UTF-8.  Up to the first zero. */
std::string frameText(const std::uint8_t* p, std::size_t size)
{
    std::string out;
    if (size == 0)
        return out;
    const std::uint8_t encoding = p[0];
    ++p;
    --size;
    if (encoding == 0 || encoding == 3)
    {
        for (std::size_t i = 0; i < size && p[i]; ++i)
        {
            if (encoding == 0)
                appendUtf8(out, p[i]);
            else
                out += char(p[i]);
        }
    }
    else
    {
        bool little = false;
        std::size_t at = 0;
        if (encoding == 1 && size >= 2 && p[0] == 0xff && p[1] == 0xfe)
        {
            little = true;
            at = 2;
        }
        else if (encoding == 1 && size >= 2 && p[0] == 0xfe && p[1] == 0xff)
        {
            at = 2;   // big end, which is also the order without a mark
        }
        for (; at + 1 < size; at += 2)
        {
            const std::uint32_t unit = little ? (std::uint32_t(p[at]) | (std::uint32_t(p[at + 1]) << 8))
                                              : ((std::uint32_t(p[at]) << 8) | p[at + 1]);
            if (unit == 0)
                break;
            std::uint32_t code = unit;
            if (unit >= 0xd800 && unit <= 0xdbff && at + 3 < size)
            {
                const std::uint32_t low = little ? (std::uint32_t(p[at + 2]) | (std::uint32_t(p[at + 3]) << 8))
                                                 : ((std::uint32_t(p[at + 2]) << 8) | p[at + 3]);
                if (low >= 0xdc00 && low <= 0xdfff)
                {
                    code = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    at += 2;
                }
            }
            appendUtf8(out, code);
        }
    }
    trim(out, true);
    return out;
}

struct Tag
{
    bool present = false;
    std::uint8_t major = 0;
    std::uint8_t flags = 0;
    std::uint32_t size = 0;   // after its ten bytes
    std::size_t total = 0;    // the whole tag: its header, its body, its footer
};

Tag id3v2(const std::uint8_t* bytes, std::size_t size)
{
    Tag tag;
    if (size < 10 || std::memcmp(bytes, "ID3", 3) != 0)
        return tag;
    tag.present = true;
    tag.major = bytes[3];
    tag.flags = bytes[5];
    tag.size = synchsafe32(bytes + 6);
    if (tag.size > size - 10)
        tag.size = std::uint32_t(size - 10);
    tag.total = std::size_t(tag.size) + 10 + ((tag.flags & kFooter) && tag.major >= 4 ? 10 : 0);
    return tag;
}

/* ff 00 written for ff, so that a frame's bytes never look like a sync. */
std::vector<std::uint8_t> unsynchronised(const std::uint8_t* p, std::size_t size)
{
    std::vector<std::uint8_t> out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        out.push_back(p[i]);
        if (p[i] == 0xff && i + 1 < size && p[i + 1] == 0x00)
            ++i;
    }
    return out;
}

void readId3v2(const std::uint8_t* bytes, std::size_t size, std::string& artist, std::string& title)
{
    const Tag tag = id3v2(bytes, size);
    if (!tag.present)
        return;
    std::vector<std::uint8_t> plain;
    const std::uint8_t* body = bytes + 10;
    std::size_t bodySize = tag.size;
    if (tag.flags & kUnsynchronised)
    {
        plain = unsynchronised(body, bodySize);
        body = plain.data();
        bodySize = plain.size();
    }
    const std::size_t idSize = tag.major <= 2 ? 3 : 4;
    const std::size_t headerSize = idSize + (tag.major <= 2 ? 3 : 4) + (tag.major <= 2 ? 0 : 2);
    std::size_t at = 0;
    if (tag.flags & kExtended)
    {
        if (bodySize < 4)
            return;
        at = 4 + (tag.major >= 4 ? std::size_t(synchsafe32(body)) : be32(body));
    }
    while (at + headerSize <= bodySize)
    {
        const std::uint8_t* frame = body + at;
        if (frame[0] == 0)
            break;   // the padding behind the frames
        std::size_t length = 0;
        if (idSize == 3)
            length = (std::size_t(frame[3]) << 16) | (std::size_t(frame[4]) << 8) | frame[5];
        else if (tag.major >= 4)
            length = synchsafe32(frame + 4);
        else
            length = be32(frame + 4);
        if (length > bodySize - at - headerSize)
            length = bodySize - at - headerSize;
        const bool isTitle = idSize == 3 ? std::memcmp(frame, "TT2", 3) == 0 : std::memcmp(frame, "TIT2", 4) == 0;
        const bool isArtist = idSize == 3 ? std::memcmp(frame, "TP1", 3) == 0 : std::memcmp(frame, "TPE1", 4) == 0;
        if (isTitle && title.empty())
            title = frameText(frame + headerSize, length);
        else if (isArtist && artist.empty())
            artist = frameText(frame + headerSize, length);
        at += headerSize + length;
    }
}

void readId3v1(const std::uint8_t* bytes, std::size_t size, std::string& artist, std::string& title)
{
    if (size < 128)
        return;
    const std::uint8_t* tag = bytes + size - 128;
    if (std::memcmp(tag, "TAG", 3) != 0)
        return;
    if (title.empty())
        title = latin1(tag + 3, 30);
    if (artist.empty())
        artist = latin1(tag + 33, 30);
}

}  // namespace

void readTags(const std::uint8_t* bytes, std::size_t size, const char* name, std::string& artist,
              std::string& title)
{
    artist.clear();
    title.clear();
    readId3v2(bytes, size, artist, title);
    readId3v1(bytes, size, artist, title);
    if (artist.empty() || title.empty())
    {
        // What the tag left unsaid, the file's own name says.
        std::string namingArtist, namingTitle;
        fromFileName(name, namingArtist, namingTitle);
        if (artist.empty())
            artist = namingArtist;
        if (title.empty())
            title = namingTitle;
    }
}

std::size_t firstFrame(const std::uint8_t* bytes, std::size_t size)
{
    const Tag tag = id3v2(bytes, size);
    return tag.present ? tag.total : 0;
}

void fromFileName(const char* name, std::string& artist, std::string& title)
{
    std::string stem = name ? name : "";
    const std::string::size_type slash = stem.find_last_of("/\\:");
    if (slash != std::string::npos)
        stem.erase(0, slash + 1);
    const std::string::size_type dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        stem.erase(dot);

    /* A track number in front ("01 - ", "01. ", "01_"): digits, and then a
     * separator rather than a word -- "99 Luftballons" keeps its ninety-nine. */
    std::size_t digits = 0;
    while (digits < stem.size() && stem[digits] >= '0' && stem[digits] <= '9')
        ++digits;
    if (digits > 0)
    {
        std::size_t after = digits;
        while (after < stem.size() && (stem[after] == ' ' || stem[after] == '\t'))
            ++after;
        if (after < stem.size() && (stem[after] == '-' || stem[after] == '.' || stem[after] == '_'))
        {
            ++after;
            while (after < stem.size() && (stem[after] == ' ' || stem[after] == '\t'))
                ++after;
            stem.erase(0, after);
        }
    }

    /* Artist and song about the first dash standing on its own: " - ", " – "
     * (the en dash) or " — " (the em dash), the dashes the names are tagged
     * with.  A hyphen inside either ("AC-DC") is not one. */
    static const char* const separators[] = { " - ", " \xe2\x80\x93 ", " \xe2\x80\x94 " };
    std::string::size_type split = std::string::npos;
    std::size_t width = 0;
    for (const char* separator : separators)
    {
        const std::string::size_type at = stem.find(separator);
        if (at != std::string::npos && (split == std::string::npos || at < split))
        {
            split = at;
            width = std::strlen(separator);
        }
    }
    if (split == std::string::npos)
    {
        title = stem;
        artist.clear();
    }
    else
    {
        artist = stem.substr(0, split);
        title = stem.substr(split + width);
    }
    trim(artist, true);
    trim(title, true);
    if (title.empty() && !artist.empty())
    {
        title = artist;   // a name that is only a dash's left side
        artist.clear();
    }
}

}  // namespace radio
}  // namespace nfs3hp
