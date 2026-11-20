#ifndef NFS3HP_NATIVE_RADIO_TAGS_H_
#define NFS3HP_NATIVE_RADIO_TAGS_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace nfs3hp
{
namespace radio
{

/* Port: what a track in USER/MUSIC is called (native_radio_tags.cpp).
 *
 * `bytes` are the file's own, `size` of them, and `name` its file name.  The
 * artist and the song come from the file's ID3v2 tag where it has one -- TPE1
 * and TIT2, or v2.2's TP1 and TT2 -- else from ID3v1's 128 bytes at its end,
 * else from the name itself, "Artist - Song.mp3".  Either is empty when
 * nothing says it: a name with no separator is a song's, with no artist. */
void readTags(const std::uint8_t* bytes, std::size_t size, const char* name,
              std::string& artist, std::string& title);

/* Where the audio starts: past the ID3v2 tag when the file begins with one
 * (ten bytes of header and the size it gives, and its footer if it has one),
 * else at nothing. */
std::size_t firstFrame(const std::uint8_t* bytes, std::size_t size);

/* The name's own reading of a track, without the path or the extension: a
 * leading track number dropped, then artist and song about the first " - ". */
void fromFileName(const char* name, std::string& artist, std::string& title);

}  // namespace radio
}  // namespace nfs3hp

#endif  // NFS3HP_NATIVE_RADIO_TAGS_H_
