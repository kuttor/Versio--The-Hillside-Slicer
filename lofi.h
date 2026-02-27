#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

/**
 * LofiProcessor - Bit depth and sample rate reduction.
 * 
 * KNOB_5 sweeps from pristine 96kHz/24-bit down to gritty lo-fi:
 * 
 *   0.0       : Clean. Full 96kHz / 24-bit. Untouched.
 *   ~0.3      : Subtle warmth. 16-bit character.
 *   ~0.5      : SP-1200 zone. 12-bit / 26kHz. The sweet spot.
 *   ~0.7      : Crunchy. 10-bit / 16kHz. Sampler from hell.
 *   1.0       : 8-bit / 8kHz. Toy keyboard in a blender.
 * 
 * Both bit depth AND sample rate degrade together on a single curve.
 * No wavefold, no distortion — just honest digital degradation
 * that gets warmer and juicier as you turn it up.
 */
class LofiProcessor
{
  public:
    LofiProcessor() {}

    void Init(float sr)
    {
        sample_rate_ = sr;
        amount_      = 0.0f;
        hold_l_      = 0.0f;
        hold_r_      = 0.0f;
        counter_     = 0;
    }

    /** Set the lo-fi amount (0.0 = pristine, 1.0 = destroyed) */
    void SetAmount(float amt) { amount_ = std::max(0.0f, std::min(1.0f, amt)); }

    /** Process a stereo sample pair in-place */
    void Process(float& left, float& right)
    {
        if(amount_ < 0.01f)
            return; // Bypass when clean

        // ── Bit depth reduction ────────────────────────────
        // Map amount to bit depth: 24 → 16 → 12 → 10 → 8
        // Using a curve that spends more time in the sweet spot (12-16 bit)
        float t = amount_ * amount_; // Quadratic: more resolution in subtle range
        float bit_depth = 24.0f - (t * 16.0f); // 24 down to 8
        bit_depth = std::max(8.0f, bit_depth);

        float quant_levels = powf(2.0f, bit_depth) * 0.5f;
        left  = floorf(left * quant_levels + 0.5f) / quant_levels;
        right = floorf(right * quant_levels + 0.5f) / quant_levels;

        // ── Sample rate reduction ──────────────────────────
        // Map amount to effective sample rate: 96kHz → 48kHz → 26kHz → 16kHz → 8kHz
        // Decimation factor: 1 (clean) up to 12 (96k/12 = 8kHz)
        if(amount_ > 0.05f)
        {
            float sr_t = (amount_ - 0.05f) / 0.95f; // Normalize
            sr_t = sr_t * sr_t; // Quadratic curve
            uint32_t decimate = 1 + static_cast<uint32_t>(sr_t * 11.0f);

            counter_++;
            if(counter_ >= decimate)
            {
                hold_l_ = left;
                hold_r_ = right;
                counter_ = 0;
            }
            left  = hold_l_;
            right = hold_r_;
        }
    }

  private:
    float    sample_rate_ = 96000.0f;
    float    amount_      = 0.0f;
    float    hold_l_      = 0.0f;
    float    hold_r_      = 0.0f;
    uint32_t counter_     = 0;
};
