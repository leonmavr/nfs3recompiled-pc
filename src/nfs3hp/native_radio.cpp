#include <nfs3hp.h>
#include <lib/file.h>
#include <SDL3/SDL.h>
#include "native_radio.h"
#include "native_radio_tags.h"
#include <minimp3.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/* Port: the radio, the player's own MP3s (native_radio.h; NFS_RADIO).
 *
 * The game's ten race songs are streamed from gamedata/audio/pc, as EA's XA
 * ADPCM, and its mixer decodes them in the music channel.  With MP3s in
 * USER/MUSIC, beside the game's data, the port plays those instead -- the
 * folder shuffled into an order of its own, a song after another, the order
 * shuffled again whenever it comes round, and a fresh one at every race.  What
 * plays is the race's music: nothing of the game's ten is heard with them --
 * the music channel's decoder is not called for a frame -- and nothing else
 * changes:
 * the channel keeps the game's music volume and pan, its resampler, its place
 * in the race's reverb, and stops as the game stops it, which is what a pause
 * and the music being off both do (native_mixer.cpp's radioChannel).
 *
 * The track is decoded here, in the port, because the game's decoder knows
 * only what EA's SND library decodes: minimp3 (third_party/minimp3) is the
 * MPEG Layer III decoder it has none of.  It is decoded on the sound thread,
 * a channel block at a time, into the rate the channel asks for; the folder
 * is searched, the file read and its names read on the main thread, once a
 * frame, so no file is ever read while the mixer waits.
 *
 * A track that plays out says so (ended), and the main thread loads the next.
 * One that gives nothing at all -- a broken file -- is passed over, and a
 * folder of those leaves the game's own music to play, as if the radio were
 * never there. */
namespace nfs3hp
{
bool raceIsRunning();   // nfs3hp_main.cpp

namespace radio
{
namespace
{

constexpr const char kFolder[] = "USER/MUSIC/";   // beside the game's data
constexpr const char kSetting[] = "NFS_RADIO";
constexpr int kStereo = 2;
// A track that gave fewer frames than this was no track at all: passed over.
constexpr std::uint64_t kNothing = 4096;

struct Track
{
    std::string name;                  // its file's name, for the log
    std::string artist, title;         // as its tags or its name gave them
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;   // the file, whole
    std::size_t at = 0;                // where its frames begin
};

/* The sound thread's side of the track playing, and how far into it. */
struct Playing
{
    std::shared_ptr<const Track> track;
    mp3dec_t decoder{};
    std::size_t at = 0;                          // in the track's bytes
    std::int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int channels = kStereo;
    int hz = 44100;
    std::vector<std::int16_t> source;            // stereo frames decoded, not yet played
    double position = 0.0;                       // where the next output frame reads from
    std::uint64_t played = 0;                    // frames given, over all its blocks
    bool drained = false;                        // nothing more will be decoded
    bool told = false;                           // said so, to the main thread
};

std::mutex s_lock;                     // Playing, and the track changing under it
Playing s_playing;
std::atomic<bool> s_wanted{false};     // the music channel's sound is the radio's
std::atomic<bool> s_ended{false};      // a track played out: the next one, on the main thread
std::atomic<std::uint64_t> s_played{0};

std::vector<std::string> s_tracks;     // the folder's, sorted; the order below indexes it
std::vector<std::size_t> s_order;      // the order the folder plays in this time round
std::size_t s_place = 0;               // where in that order the next one is
std::size_t s_last = 0;                // the one played last: not the one that opens the next round
bool s_anyPlayed = false;              // one of them played yet, so s_last means something
std::string s_artist, s_title;         // the one loaded, for the panel
unsigned s_generation = 0;
bool s_have = false;                   // a track loaded
bool s_racing = false, s_scanned = false, s_givenUp = false;
std::size_t s_misses = 0;              // tracks in a row that gave nothing

/* NFS_RADIO: on; 0 or off, the game's own music and no radio. */
bool enabled()
{
    static const bool on = [] {
        const char* value = SDL_getenv(kSetting);
        if (!value || !*value)
            return true;
        if (SDL_strcasecmp(value, "off") == 0)
            return false;
        if (SDL_strcasecmp(value, "on") == 0)
            return true;
        return SDL_atof(value) != 0.0;
    }();
    return on;
}

/* The radio's own numbers, so that no other of the port's is disturbed: a
 * xorshift, seeded once from the clock. */
std::uint32_t random(std::uint32_t bound)
{
    static std::uint32_t seed = 0;
    if (seed == 0)
        seed = std::uint32_t(SDL_GetTicksNS()) | 1u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return bound ? seed % bound : 0;
}

/* The folder's, shuffled: the order this round plays in.  The song that played
 * last is kept off the front, so a round never opens with the one that just
 * ended. */
void shuffle()
{
    s_order.resize(s_tracks.size());
    for (std::size_t i = 0; i < s_order.size(); ++i)
        s_order[i] = i;
    for (std::size_t i = s_order.size(); i > 1; --i)
        std::swap(s_order[i - 1], s_order[std::size_t(random(std::uint32_t(i)))]);
    if (s_anyPlayed && s_order.size() > 1 && s_order.front() == s_last)
        std::swap(s_order.front(), s_order[1]);
    s_place = 0;
}

bool isMp3(const char* name)
{
    const std::size_t length = SDL_strlen(name);
    return length > 4 && SDL_strcasecmp(name + length - 4, ".mp3") == 0;
}

void scan()
{
    s_scanned = true;
    s_tracks.clear();
    /* Beside the game's data first; a desktop run started in the folder that
     * holds FEDATA and GAMEDATA, the way the game is usually started, then
     * has its USER/MUSIC there too, and that is where it is looked for. */
    const std::string folders[] = { win32::File::dataDirectory() + kFolder, kFolder };
    for (const std::string& folder : folders)
    {
        SDL_EnumerateDirectory(
            folder.c_str(),
            [](void*, const char* dir, const char* entry) {
                if (isMp3(entry))
                    s_tracks.push_back(std::string(dir) + entry);   // dir carries its separator
                return SDL_ENUM_CONTINUE;
            },
            nullptr);
        if (!s_tracks.empty())
        {
            std::sort(s_tracks.begin(), s_tracks.end());   // the order below is the shuffled one
            shuffle();
            SDL_Log("[RADIO] %d MP3s in %s, shuffled", int(s_tracks.size()), folder.c_str());
            return;
        }
    }
    shuffle();
    SDL_Log("[RADIO] no MP3s in %s", folders[0].c_str());
}

std::shared_ptr<const Track> load(const std::string& path)
{
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data)
    {
        SDL_Log("[RADIO] %s not read (%s)", path.c_str(), SDL_GetError());
        return nullptr;
    }
    auto track = std::make_shared<Track>();
    const auto bytes = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
    SDL_free(data);
    const std::string::size_type separator = path.find_last_of("/\\");
    track->name = separator == std::string::npos ? path : path.substr(separator + 1);
    readTags(bytes->data(), bytes->size(), track->name.c_str(), track->artist, track->title);
    track->at = firstFrame(bytes->data(), bytes->size());
    track->bytes = bytes;
    return track;
}

void start(const std::shared_ptr<const Track>& track)
{
    s_artist = track->artist;
    s_title = track->title;
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_playing = Playing{};
        mp3dec_init(&s_playing.decoder);
        s_playing.track = track;
        s_playing.at = track->at;
    }
    s_have = true;
    s_ended.store(false);
    s_played.store(0);
    ++s_generation;
    SDL_Log("[RADIO] %s - %s (%s)", track->artist.c_str(), track->title.c_str(), track->name.c_str());
}

/* The next of the order, shuffling it anew at the end of a round; false when
 * none of the folder's could be read. */
bool loadNext()
{
    if (s_tracks.empty())
        return false;
    for (std::size_t tried = 0; tried < s_tracks.size(); ++tried)
    {
        if (s_place >= s_order.size())
            shuffle();
        const std::size_t at = s_order[s_place++];
        if (std::shared_ptr<const Track> track = load(s_tracks[at]))
        {
            s_last = at;
            s_anyPlayed = true;
            start(track);
            return true;
        }
    }
    return false;
}

void stop()
{
    s_wanted.store(false);
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_playing = Playing{};
    }
    s_artist.clear();
    s_title.clear();
    s_have = false;
    s_ended.store(false);
    s_played.store(0);
    ++s_generation;
}

std::int16_t sample(double value)
{
    const double rounded = value >= 0.0 ? value + 0.5 : value - 0.5;
    if (rounded >= 32767.0)
        return 32767;
    if (rounded <= -32768.0)
        return -32768;
    return std::int16_t(rounded);
}

/* One MPEG frame into the source, as stereo wherever it is mono; false when
 * nothing more can be read from the bytes left. */
bool decodeMore(Playing& p, const std::vector<std::uint8_t>& bytes)
{
    while (p.at < bytes.size())
    {
        mp3dec_frame_info_t info = {};
        const int samples = mp3dec_decode_frame(&p.decoder, bytes.data() + p.at,
                                                int(bytes.size() - p.at), p.frame, &info);
        if (info.frame_bytes <= 0)
            return false;
        p.at += std::size_t(info.frame_bytes);
        if (samples <= 0)
            continue;   // a tag, or a frame with nothing in it: the next one
        p.channels = info.channels > 0 ? info.channels : 1;
        if (info.hz > 0)
            p.hz = info.hz;
        for (int i = 0; i < samples; ++i)
        {
            const std::int16_t left = p.frame[i * p.channels];
            p.source.push_back(left);
            p.source.push_back(p.channels > 1 ? p.frame[i * p.channels + 1] : left);
        }
        return true;
    }
    return false;
}

}  // namespace

void tick()
{
    if (!enabled())
        return;
    const bool racing = raceIsRunning();
    if (racing != s_racing)
    {
        s_racing = racing;
        if (racing)
        {
            // Every race looks again: the player may have added a track to it.
            s_scanned = false;
            s_givenUp = false;
            s_misses = 0;
        }
    }
    if (!racing)
    {
        if (s_wanted.load(std::memory_order_relaxed))
            stop();
        return;
    }
    if (!s_scanned)
    {
        scan();
        s_givenUp = !s_tracks.empty() ? !loadNext() : true;
    }
    if (ended())
    {
        s_ended.store(false);
        if (s_played.load(std::memory_order_relaxed) < kNothing)
            ++s_misses;
        else
            s_misses = 0;
        if (s_misses >= s_tracks.size())
        {
            SDL_Log("[RADIO] none of the folder's tracks plays; the game's music it is");
            s_givenUp = true;
            stop();
        }
        else if (!s_givenUp)
            loadNext();
    }
    s_wanted.store(s_have && !s_givenUp, std::memory_order_relaxed);
}

bool wanted()
{
    return s_wanted.load(std::memory_order_relaxed);
}

bool ended()
{
    return s_ended.load(std::memory_order_relaxed);
}

void skip()
{
    if (s_tracks.empty() || s_givenUp)
        return;
    s_ended.store(false);
    s_misses = 0;
    loadNext();
}

const char* artist()
{
    return s_artist.c_str();
}

const char* title()
{
    return s_title.c_str();
}

unsigned generation()
{
    return s_generation;
}

bool decode(std::int16_t* out, int frames, int rate)
{
    if (!out || frames <= 0 || rate <= 0)
        return false;
    std::fill(out, out + std::size_t(frames) * kStereo, std::int16_t(0));
    std::lock_guard<std::mutex> lock(s_lock);
    Playing& p = s_playing;
    if (!p.track)
        return false;

    const std::vector<std::uint8_t>& bytes = *p.track->bytes;
    if (p.source.empty() && !p.drained && !decodeMore(p, bytes))
        p.drained = true;   // one frame first, for the rate the ratio below is of
    const double ratio = double(p.hz) / double(rate);   // source frames an output frame
    const double until = p.position + double(frames) * ratio;
    while (!p.drained && double(p.source.size() / kStereo) < until + 2.0)
        if (!decodeMore(p, bytes))
            p.drained = true;

    // Interpolated as the game's own resampler does it, a frame at a time.
    const std::size_t have = p.source.size() / kStereo;
    const std::int16_t* source = p.source.data();
    for (int k = 0; k < frames; ++k)
    {
        const std::size_t at = std::size_t(p.position);
        if (at + 1 >= have)
            break;   // past its end: silence
        const double fraction = p.position - double(at);
        out[k * kStereo] = sample(double(source[at * kStereo]) * (1.0 - fraction)
                                  + double(source[at * kStereo + 2]) * fraction);
        out[k * kStereo + 1] = sample(double(source[at * kStereo + 1]) * (1.0 - fraction)
                                      + double(source[at * kStereo + 3]) * fraction);
        p.position += ratio;
        ++p.played;
    }

    const std::size_t drop = std::min(std::size_t(p.position), have);
    if (drop > 0)
    {
        const std::size_t keep = have - drop;
        if (keep == 0)
            p.source.clear();
        else
        {
            std::memmove(p.source.data(), p.source.data() + drop * kStereo,
                         keep * kStereo * sizeof(std::int16_t));
            p.source.resize(keep * kStereo);
        }
        p.position -= double(drop);
    }
    s_played.store(p.played, std::memory_order_relaxed);
    if (p.drained && !p.told && p.position + 1.0 >= double(p.source.size() / kStereo))
    {
        p.told = true;
        SDL_Log("[RADIO] %s played out: %llu frames at %d Hz, %.1f s", p.track->name.c_str(),
                (unsigned long long)p.played, rate, double(p.played) / double(rate));
        s_ended.store(true);
    }
    return true;
}

}  // namespace radio
}  // namespace nfs3hp
