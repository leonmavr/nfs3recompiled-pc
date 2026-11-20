#ifndef NFS3HP_NATIVE_RADIO_H_
#define NFS3HP_NATIVE_RADIO_H_

#include <cstdint>

/* Port: the radio, the player's own MP3s in USER/MUSIC (native_radio.cpp).
 *
 * A race's music is one of the game's ten tracks, streamed from its .mus.  With
 * MP3s in USER/MUSIC -- beside the game's data, or in the folder the game was
 * started from -- the port plays those instead, shuffled, through the very
 * channel the game's music streamed through, so the music volume, the pan, the
 * resampling, the race's reverb and the pause are all still the game's.  Nothing there is an .mp3:
 * the game's decoder knows only EA's XA ADPCM, so the port brings its own
 * (third_party/minimp3).  NFS_RADIO=0 plays the game's music as it was.
 *
 * Split three ways: the main thread (tick, once a frame from the swap's call,
 * native_now_playing.cpp) finds the folder, reads a track's names and loads
 * it; the sound thread (decode, from the game's mixer, native_mixer.cpp)
 * decodes and resamples it into the channel's frames; and the panel that names
 * the song (native_now_playing.cpp) asks who is playing.  A track the sound
 * thread has played out says so (ended), and the main thread loads the next. */
namespace nfs3hp
{
namespace radio
{

/* Once a frame, on the main thread: a race beginning loads a track, one
 * playing out loads the next, a race over lets it go. */
void tick();

/* Whether the music channel is playing the player's own music right now: what
 * native_mixer.cpp asks before it hands a channel's frames to decode, and what
 * the panel asks to know which song to name. */
bool wanted();

/* The sound thread's side: `frames` stereo samples at `rate` a second -- the
 * channel's own, which resamples them as it resampled the track's.  Silence
 * where the track has run out or could not be read.  False when there is no
 * track at all, and the channel is left to the game. */
bool decode(std::int16_t* out, int frames, int rate);

/* The track loaded now, for the panel: its names as the tags or the file name
 * gave them, and a number that changes with every track -- the panel shows
 * the song again when it does. */
const char* artist();
const char* title();
unsigned generation();

/* The next track, asked for by the key that skips one (native_now_playing.cpp);
 * and whether the track playing has run out, for the main thread. */
void skip();
bool ended();

}  // namespace radio
}  // namespace nfs3hp

#endif  // NFS3HP_NATIVE_RADIO_H_
