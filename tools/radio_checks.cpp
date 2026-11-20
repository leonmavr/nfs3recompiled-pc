#include "native_radio_tags.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

/* The radio's track names, checked: the ID3v2 tags a file carries, ID3v1's
 * when it has only those, and the file name's "Artist - Song" when it has
 * neither (src/nfs3hp/native_radio_tags.cpp).  Build with
 * -DNFS_BUILD_RADIO_TESTS=ON and run the `radio_checks` target. */
namespace
{

int s_checks = 0;

void check(bool ok, const char* what)
{
    if (!ok)
        throw std::runtime_error(std::string("failed: ") + what);
    ++s_checks;
}

void called(const std::vector<std::uint8_t>& bytes, const char* name, const char* artist, const char* title)
{
    std::string gotArtist, gotTitle;
    nfs3hp::radio::readTags(bytes.data(), bytes.size(), name, gotArtist, gotTitle);
    if (gotArtist != artist || gotTitle != title)
        throw std::runtime_error(std::string("'") + name + "' gave '" + gotArtist + "' - '" + gotTitle + "'");
    ++s_checks;
}

void append(std::vector<std::uint8_t>& out, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

/* ID3v2 tag: text frames of `major`, `id`-sized, a size byte each frame's
 * first; the tag's own ten bytes in front. */
std::vector<std::uint8_t> tag(std::uint8_t major, std::vector<std::pair<std::string, std::string>> frames)
{
    std::vector<std::uint8_t> body;
    for (const auto& [id, text] : frames)
    {
        std::vector<std::uint8_t> frame;
        append(frame, id.data(), id.size());
        const std::size_t size = text.size();
        if (major <= 2)
        {
            frame.push_back(std::uint8_t(size >> 16 & 0xff));
            frame.push_back(std::uint8_t(size >> 8 & 0xff));
            frame.push_back(std::uint8_t(size & 0xff));
        }
        else
        {
            // v2.4's size is synchsafe, seven bits a byte; v2.3's is plain.
            std::uint8_t bytes[4];
            for (int k = 0; k < 4; ++k)
                bytes[k] = std::uint8_t((size >> (8 * (3 - k))) & (major >= 4 ? 0x7f : 0xff));
            append(frame, bytes, 4);
            const std::uint8_t flags[2] = { 0, 0 };
            append(frame, flags, 2);
        }
        append(frame, text.data(), text.size());
        append(body, frame.data(), frame.size());
    }
    std::vector<std::uint8_t> out;
    append(out, "ID3", 3);
    out.push_back(major);
    out.push_back(0);
    out.push_back(0);
    const std::size_t size = body.size();
    out.push_back(std::uint8_t(size >> 21 & 0x7f));
    out.push_back(std::uint8_t(size >> 14 & 0x7f));
    out.push_back(std::uint8_t(size >> 7 & 0x7f));
    out.push_back(std::uint8_t(size & 0x7f));
    append(out, body.data(), body.size());
    return out;
}

std::vector<std::uint8_t> id3v1(const char* artist, const char* title)
{
    std::vector<std::uint8_t> tag(128, 0);
    std::memcpy(tag.data(), "TAG", 3);
    std::memcpy(tag.data() + 3, title, std::strlen(title));
    std::memcpy(tag.data() + 33, artist, std::strlen(artist));
    return tag;
}

void names()
{
    std::string artist, title;

    nfs3hp::radio::fromFileName("Saki Kaskas - Little Sweaty Sow.mp3", artist, title);
    check(artist == "Saki Kaskas" && title == "Little Sweaty Sow", "a plain name");
    nfs3hp::radio::fromFileName("01 - Rom Di Prisco - Hydrus 606.mp3", artist, title);
    check(artist == "Rom Di Prisco" && title == "Hydrus 606", "a track number in front");
    nfs3hp::radio::fromFileName("02. Matt Ragan - Snow Bags.mp3", artist, title);
    check(artist == "Matt Ragan" && title == "Snow Bags", "a numbered name");
    nfs3hp::radio::fromFileName("99 Luftballons.mp3", artist, title);
    check(artist.empty() && title == "99 Luftballons", "a number that is a song's");
    nfs3hp::radio::fromFileName("AC-DC - Thunderstruck.mp3", artist, title);
    check(artist == "AC-DC" && title == "Thunderstruck", "a hyphen that is a name's");
    nfs3hp::radio::fromFileName("No Separator Here.mp3", artist, title);
    check(artist.empty() && title == "No Separator Here", "a name with no artist");
    nfs3hp::radio::fromFileName("C:\\music\\Artist \xe2\x80\x93 Song.mp3", artist, title);
    check(artist == "Artist" && title == "Song", "an en dash, and the folder");
}

void tags()
{
    // ID3v2.3, ISO-8859-1.
    std::vector<std::uint8_t> bytes = tag(3, { { "TPE1", std::string("\x00", 1) + "Rom Di Prisco" },
                                               { "TIT2", std::string("\x00", 1) + "Cetus 808" } });
    called(bytes, "01 - wrong - wrong.mp3", "Rom Di Prisco", "Cetus 808");

    // ID3v2.4, UTF-8.
    bytes = tag(4, { { "TIT2", std::string("\x03", 1) + "Knossos" },
                     { "TPE1", std::string("\x03", 1) + "Saki K\xc3\xa4skas" } });
    called(bytes, "wrong.mp3", "Saki K\xc3\xa4skas", "Knossos");

    // ID3v2.2's own three-letter names.
    bytes = tag(2, { { "TT2", std::string("\x00", 1) + "Flimsy" }, { "TP1", std::string("\x00", 1) + "Saki" } });
    called(bytes, "wrong.mp3", "Saki", "Flimsy");

    // ID3v2.3, UTF-16 with a byte order mark, little end first.
    std::vector<std::uint8_t> utf16 = { 0x01, 0xff, 0xfe };
    for (const char c : std::string("Warped"))
    {
        utf16.push_back(std::uint8_t(c));
        utf16.push_back(0);
    }
    bytes = tag(3, { { "TIT2", std::string(utf16.begin(), utf16.end()) } });
    called(bytes, "Alistair Hirst - wrong.mp3", "Alistair Hirst", "Warped");

    // A tag with the title alone: the artist is the name's.
    bytes = tag(3, { { "TIT2", std::string("\x00", 1) + "Snow Bags" } });
    called(bytes, "Matt Ragan - Wrong Title.mp3", "Matt Ragan", "Snow Bags");

    // ID3v1 at the end, and no ID3v2 at all.
    bytes = id3v1("Rom Di Prisco", "Aquilla 303");
    called(bytes, "wrong - wrong.mp3", "Rom Di Prisco", "Aquilla 303");

    // ID3v2 first, ID3v1 only filling what it left out.
    bytes = tag(3, { { "TIT2", std::string("\x00", 1) + "Rear Flutterblast #19" } });
    const std::vector<std::uint8_t> old = id3v1("Matt Ragan", "Wrong");
    append(bytes, old.data(), old.size());
    called(bytes, "wrong.mp3", "Matt Ragan", "Rear Flutterblast #19");

    // Nothing at all: the name.
    called({}, "Saki Kaskas - Little Sweaty Sow.mp3", "Saki Kaskas", "Little Sweaty Sow");

    // Where the frames start: past the tag, and nothing where there is none.
    const std::vector<std::uint8_t> framed = tag(3, { { "TIT2", std::string("\x00", 1) + "Hydrus 606" } });
    check(nfs3hp::radio::firstFrame(framed.data(), framed.size()) == framed.size(), "the first frame after a tag");
    check(nfs3hp::radio::firstFrame(nullptr, 0) == 0, "the first frame without one");
}

}  // namespace

int main()
{
    try
    {
        names();
        tags();
    }
    catch (const std::exception& error)
    {
        std::printf("radio_checks: %s\n", error.what());
        return 1;
    }
    std::printf("radio_checks: %d checks passed\n", s_checks);
    return 0;
}
