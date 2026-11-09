#ifndef LIB_LOUDNESS_H_
#define LIB_LOUDNESS_H_

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>

namespace win32
{

/* The finished mix's level on its way to the device.
 *
 * The game's mix is not quiet in itself -- a race peaks at full scale and
 * averages about -12 dBFS, the menu music about -17 dBFS, with the Audio
 * screen at its default 50% -- but beside music and video, mastered louder and
 * compressed, it sounds it.  A plain multiply cannot make it louder: a race
 * already reaches full scale, so a lift alone only clips, harshly, where it is
 * loud.  So the lift (NFS_VOLUME, 2.5 -- +8 dB -- by default, 0.1 to 8; 1
 * leaves the mix as the game made it) goes through a limiter: wherever the
 * lifted sound would pass -1 dBFS the gain is pulled down at once, and it eases
 * back over about 150 ms, the same for every channel so the stereo picture
 * holds.  Quiet passages come up by the whole lift; loud ones are held under
 * full scale instead of clipped.
 *
 * One per stream, used from that stream's thread only: it keeps its gain from
 * one block to the next. */
class Loudness
{
public:
    Loudness()
    {
        const char* value = SDL_getenv("NFS_VOLUME");
        const float parsed = value && *value ? float(SDL_atof(value)) : 2.5f;
        m_lift = parsed >= 0.1f && parsed <= 8.0f ? parsed : 2.5f;
        setRate(22050);
    }

    void setRate(int rate)
    {
        m_release = 1.0f - SDL_expf(-1.0f / (0.150f * float(rate > 0 ? rate : 22050)));
    }

    /* 16-bit samples, `channels` to a frame, levelled in place. */
    void apply(std::int16_t* samples, std::size_t frames, std::size_t channels)
    {
        if (m_lift == 1.0f || channels == 0)
            return;
        const float ceiling = 32767.0f * 0.891f;  // -1 dBFS
        for (std::size_t f = 0; f < frames; ++f)
        {
            std::int16_t* frame = samples + f * channels;
            float peak = 0.0f;
            for (std::size_t c = 0; c < channels; ++c)
            {
                const float level = SDL_fabsf(float(frame[c])) * m_lift;
                peak = level > peak ? level : peak;
            }
            const float wanted = peak > ceiling ? ceiling / peak : 1.0f;
            m_gain = wanted < m_gain ? wanted : m_gain + (wanted - m_gain) * m_release;
            const float scale = m_lift * m_gain;
            for (std::size_t c = 0; c < channels; ++c)
            {
                const float out = float(frame[c]) * scale;
                frame[c] = std::int16_t(out > 32767.0f ? 32767 : out < -32768.0f ? -32768 : int(out));
            }
        }
    }

private:
    float m_lift = 2.5f;
    float m_release = 0.0f;  // a frame's share of the way back
    float m_gain = 1.0f;     // the limiter's own, 1 when idle
};

}

#endif /* !LIB_LOUDNESS_H_ */
