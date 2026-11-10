#ifndef LIB_LOUDNESS_H_
#define LIB_LOUDNESS_H_

#include <SDL3/SDL.h>
#include <cmath>
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
 * loud.
 *
 * The first version lifted it (+8 dB) into a plain peak limiter, and that
 * gave back most of the lift: a race's mix is wall-to-wall peaks (the
 * engine's buzz alone is one every few milliseconds), so the limiter sat
 * pulled down all the time, and a race came out about 4 dB louder for an
 * 8 dB lift -- and lifting further did nothing at all (measured: +6 dB more
 * lift, +0.3 dB out).  No gain riding can make a signal like that louder;
 * rounding its peaks off can.  So now:
 *
 * 1. The lift, NFS_VOLUME: 1.8 (+5.1 dB) by default, picked by ear, 0.1 to 8; 1 leaves the
 *    mix as the game made it (and none of the rest is done).
 * 2. A soft saturator: up to -2.5 dBFS the lifted sound is left as it is;
 *    above it each sample is bent smoothly (tanh) towards -0.45 dBFS instead
 *    of rising further -- only the peaks rounded, some 3% of a race's samples
 *    and by 2 dB on average.  (At first it bent from -6 dBFS after a lift of
 *    3: a fifth of the samples, and it sounded it -- saturated.)
 * 3. A limiter that looks 3 ms ahead, as the safety under it: the gain is
 *    brought down over those 3 ms before a peak arrives, to hold it under
 *    -1 dBFS, and eases back over 80 ms.  The output is 3 ms later.
 *
 * The saturator then touches only the loudest peaks of a race.
 * The same gain for every channel, so the stereo picture holds.  One per
 * stream, used from that stream's thread only: it keeps its state from one
 * block to the next. */
class Loudness
{
public:
    Loudness()
    {
        const char* value = SDL_getenv("NFS_VOLUME");
        const float parsed = value && *value ? float(SDL_atof(value)) : 1.8f;
        m_lift = parsed >= 0.1f && parsed <= 8.0f ? parsed : 1.8f;
        setRate(22050);
    }

    void setRate(int rate)
    {
        const float r = float(rate > 0 ? rate : 22050);
        if (r == m_rate)
            return;
        m_rate = r;
        m_release = 1.0f - SDL_expf(-1.0f / (0.080f * r));
        m_ahead = std::size_t(0.003f * r) + 1;
        if (m_ahead > kMaxAhead)
            m_ahead = kMaxAhead;
        m_attack = 1.0f - SDL_expf(-3.0f / float(m_ahead));
        m_at = 0;
        m_filled = 0;
    }

    /* 16-bit samples, `channels` to a frame (at most 8), levelled in place. */
    void apply(std::int16_t* samples, std::size_t frames, std::size_t channels)
    {
        if (m_lift == 1.0f || channels == 0 || channels > kMaxChannels)
            return;
        const float ceiling = 32767.0f * 0.891f;  // -1 dBFS
        const float knee = 32767.0f * 0.75f;        // -2.5 dBFS
        const float top = 32767.0f * 0.95f;         // -0.45 dBFS
        for (std::size_t f = 0; f < frames; ++f)
        {
            std::int16_t* frame = samples + f * channels;
            // 1 and 2: lifted, and its peaks rounded off.
            float in[kMaxChannels];
            float peak = 0.0f;
            for (std::size_t c = 0; c < channels; ++c)
            {
                float v = float(frame[c]) * m_lift;
                const float a = SDL_fabsf(v);
                if (a > knee)
                {
                    const float bent = knee + (top - knee) * std::tanh((a - knee) / (top - knee));
                    v = v < 0.0f ? -bent : bent;
                }
                in[c] = v;
                peak = SDL_fabsf(v) > peak ? SDL_fabsf(v) : peak;
            }
            // 3: what this frame will need, and the frame from 3 ms ago out.
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;
            Delayed& slot = m_line[m_at];
            float out[kMaxChannels];
            const bool full = m_filled >= m_ahead;
            if (full)
                for (std::size_t c = 0; c < channels; ++c)
                    out[c] = slot.sample[c];
            for (std::size_t c = 0; c < channels; ++c)
                slot.sample[c] = in[c];
            slot.needed = needed;
            m_at = (m_at + 1) % m_ahead;
            if (!full)
                ++m_filled;
            float lowest = 1.0f;
            for (std::size_t k = 0; k < m_filled; ++k)
                lowest = m_line[k].needed < lowest ? m_line[k].needed : lowest;
            m_gain = lowest < m_gain ? m_gain + (lowest - m_gain) * m_attack : m_gain + (lowest - m_gain) * m_release;
            for (std::size_t c = 0; c < channels; ++c)
            {
                float v = full ? out[c] * m_gain : 0.0f;
                // The soft knee: the last decibel to full scale bent, not cut.
                const float a = SDL_fabsf(v);
                if (a > ceiling)
                {
                    const float over = (a - ceiling) / (32767.0f - ceiling);
                    const float bent = ceiling + (32767.0f - ceiling) * (over / (1.0f + over));
                    v = v < 0.0f ? -bent : bent;
                }
                frame[c] = std::int16_t(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : int(v));
            }
        }
    }

private:
    static constexpr std::size_t kMaxChannels = 8;
    static constexpr std::size_t kMaxAhead = 512;
    struct Delayed
    {
        float sample[kMaxChannels];
        float needed;
    };
    float m_lift = 1.8f;
    float m_rate = 0.0f;
    float m_release = 0.0f;  // a frame's share of the way back
    float m_attack = 0.0f;   // ... and down, over the look-ahead
    float m_gain = 1.0f;     // the limiter's own, 1 when idle
    std::size_t m_ahead = 1, m_at = 0, m_filled = 0;
    Delayed m_line[kMaxAhead] = {};
};

}

#endif /* !LIB_LOUDNESS_H_ */
