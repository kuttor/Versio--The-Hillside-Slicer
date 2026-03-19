#pragma once
#include <cmath>
#include <cstring>

/**
 * Tape Wobble — Modulated delay line simulating wow & flutter.
 *
 * Wow:     slow pitch drift (0.3-2Hz) from motor speed irregularity
 * Flutter: faster jitter (4-12Hz) from tape-head friction
 * Scrape:  very fast micro-jitter (20-40Hz) from tape surface
 *
 * Three layered LFOs with slight randomization create complex,
 * non-repeating modulation that sounds like real tape, not chorus.
 *
 * Implementation: short delay line (~20ms) with modulated read position.
 * No filtering. No saturation. Just time-domain pitch wobble.
 */

// Simple noise source for LFO randomization
static inline float HashNoise(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed & 0xFFFF) / 32768.0f - 1.0f;
}

class TapeWobble
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        memset(buf_l_, 0, sizeof(buf_l_));
        memset(buf_r_, 0, sizeof(buf_r_));
        wr_ = 0;
        amount_ = 0.0f;
        smooth_amount_ = 0.0f;

        // LFO phases — start at different points
        wow_phase_   = 0.0f;
        flut_phase_  = 0.37f;
        scrape_phase_ = 0.71f;
        noise_seed_  = 12345u;

        // Randomized rate targets
        wow_rate_    = 0.8f;
        flut_rate_   = 6.0f;
        scrape_rate_ = 28.0f;
        rate_timer_  = 0;
    }

    /** Set wobble amount. 0.0 = off, 1.0 = heavy VHS degradation. */
    void SetAmount(float a)
    {
        if(a < 0.0f) a = 0.0f;
        if(a > 1.0f) a = 1.0f;
        amount_ = a;
    }

    /** Process stereo pair in-place. */
    void Process(float& l, float& r)
    {
        // Smooth amount to avoid clicks
        smooth_amount_ += 0.0005f * (amount_ - smooth_amount_);

        if(smooth_amount_ < 0.0001f)
        {
            // Bypass: still write to buffer to keep it primed
            buf_l_[wr_] = l;
            buf_r_[wr_] = r;
            if(++wr_ >= BUF_SIZE) wr_ = 0;
            return;
        }

        // Write to circular buffer
        buf_l_[wr_] = l;
        buf_r_[wr_] = r;

        // Randomize LFO rates slowly (~4x per second)
        rate_timer_++;
        if(rate_timer_ >= static_cast<uint32_t>(sr_ * 0.25f))
        {
            rate_timer_ = 0;
            float n = HashNoise(noise_seed_);
            wow_rate_   = 0.5f + n * 0.4f;    // 0.1-0.9 Hz
            n = HashNoise(noise_seed_);
            flut_rate_  = 5.0f + n * 3.0f;    // 2-8 Hz
            n = HashNoise(noise_seed_);
            scrape_rate_ = 24.0f + n * 12.0f;  // 12-36 Hz
        }

        // Advance LFO phases
        wow_phase_   += wow_rate_ / sr_;
        flut_phase_  += flut_rate_ / sr_;
        scrape_phase_ += scrape_rate_ / sr_;
        if(wow_phase_ > 1.0f) wow_phase_ -= 1.0f;
        if(flut_phase_ > 1.0f) flut_phase_ -= 1.0f;
        if(scrape_phase_ > 1.0f) scrape_phase_ -= 1.0f;

        // LFO shapes: sine for wow, triangle for flutter, noise-ish for scrape
        float wow   = sinf(wow_phase_ * 6.28318530f);
        float flut  = 2.0f * fabsf(2.0f * flut_phase_ - 1.0f) - 1.0f;
        float scrape = sinf(scrape_phase_ * 6.28318530f)
                       * (0.7f + 0.3f * HashNoise(noise_seed_));

        // Composite modulation depth (in samples)
        // Wow: up to 12ms, Flutter: up to 1.5ms, Scrape: up to 0.3ms
        float depth = smooth_amount_;
        float mod_samples = depth * (wow * sr_ * 0.012f
                                     + flut * sr_ * 0.0015f * depth
                                     + scrape * sr_ * 0.0003f * depth * depth);

        // Base delay: 15ms (keeps modulation centered)
        float base_delay = sr_ * 0.015f;
        float total_delay = base_delay + mod_samples;

        // Clamp to buffer
        if(total_delay < 1.0f) total_delay = 1.0f;
        if(total_delay > BUF_SIZE - 2) total_delay = BUF_SIZE - 2;

        // Read with linear interpolation
        float rd = static_cast<float>(wr_) - total_delay;
        if(rd < 0.0f) rd += BUF_SIZE;
        int idx = static_cast<int>(rd);
        float frac = rd - static_cast<float>(idx);
        int next = idx + 1;
        if(next >= BUF_SIZE) next = 0;

        l = buf_l_[idx] * (1.0f - frac) + buf_l_[next] * frac;
        r = buf_r_[idx] * (1.0f - frac) + buf_r_[next] * frac;

        if(++wr_ >= BUF_SIZE) wr_ = 0;
    }

  private:
    static constexpr int BUF_SIZE = 2048;  // ~21ms at 96kHz
    float buf_l_[BUF_SIZE];
    float buf_r_[BUF_SIZE];
    int   wr_ = 0;
    float sr_ = 96000.0f;

    float amount_ = 0.0f;
    float smooth_amount_ = 0.0f;

    // LFO state
    float wow_phase_   = 0.0f;
    float flut_phase_  = 0.0f;
    float scrape_phase_ = 0.0f;
    float wow_rate_    = 0.8f;
    float flut_rate_   = 6.0f;
    float scrape_rate_ = 28.0f;
    uint32_t noise_seed_ = 12345u;
    uint32_t rate_timer_ = 0;
};

/**
 * Tape Lo-Fi — Sample rate reduction + bit crush + gentle rolloff.
 *
 * Emulates degraded tape/cassette playback:
 *   - Sample rate reduction (96kHz → down to ~4kHz)
 *   - Bit depth reduction (24-bit → down to ~6-bit)
 *   - Gentle 1-pole lowpass to smooth the staircase (tape head rolloff)
 *   - Subtle hiss that increases with amount
 *
 * No resonant filters. No saturation stages. Pure degradation math.
 */
class TapeLofi
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        amount_ = 0.0f;
        smooth_amount_ = 0.0f;
        hold_l_ = 0.0f;
        hold_r_ = 0.0f;
        hold_counter_ = 0;
        lp_l_ = 0.0f;
        lp_r_ = 0.0f;
        noise_seed_ = 54321u;
    }

    /** Set lo-fi amount. 0.0 = clean, 1.0 = heavily degraded. */
    void SetAmount(float a)
    {
        if(a < 0.0f) a = 0.0f;
        if(a > 1.0f) a = 1.0f;
        amount_ = a;
    }

    /** Process stereo pair in-place. */
    void Process(float& l, float& r)
    {
        smooth_amount_ += 0.0005f * (amount_ - smooth_amount_);

        if(smooth_amount_ < 0.0001f) return;  // Clean bypass

        float t = smooth_amount_;

        // ── Sample rate reduction ─────────────────────────
        // Decimation factor: 1 (clean) → 24 (4kHz effective at 96kHz)
        uint32_t decimate = 1 + static_cast<uint32_t>(t * 23.0f);

        hold_counter_++;
        if(hold_counter_ >= decimate)
        {
            hold_counter_ = 0;
            hold_l_ = l;
            hold_r_ = r;
        }
        l = hold_l_;
        r = hold_r_;

        // ── Bit depth reduction ───────────────────────────
        // Quantize to fewer levels: 24-bit (16M) → ~64 levels at max
        // Scale: 2^16 at t=0 → 2^6 at t=1
        float bits = 16.0f - t * 10.0f;  // 16 → 6
        float levels = powf(2.0f, bits);
        l = floorf(l * levels + 0.5f) / levels;
        r = floorf(r * levels + 0.5f) / levels;

        // ── Gentle 1-pole lowpass (tape head rolloff) ─────
        // Cutoff drops from 20kHz to ~3kHz with amount
        float cutoff = 20000.0f - t * 17000.0f;
        float coeff = 1.0f - expf(-6.28318530f * cutoff / sr_);
        lp_l_ += coeff * (l - lp_l_);
        lp_r_ += coeff * (r - lp_r_);
        l = lp_l_;
        r = lp_r_;

        // ── Subtle hiss ───────────────────────────────────
        float hiss_level = t * t * 0.008f;
        l += HashNoise(noise_seed_) * hiss_level;
        r += HashNoise(noise_seed_) * hiss_level;
    }

  private:
    float sr_ = 96000.0f;
    float amount_ = 0.0f;
    float smooth_amount_ = 0.0f;

    // Decimation
    float    hold_l_ = 0.0f;
    float    hold_r_ = 0.0f;
    uint32_t hold_counter_ = 0;

    // Lowpass
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;

    // Noise
    uint32_t noise_seed_ = 54321u;
};
