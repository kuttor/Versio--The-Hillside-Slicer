#pragma once
#include <cstring>
#include <cmath>

// 4-Head Granular Pitch Shifter - Anti-Tremolo
// 4 grains with 25% phase offset eliminate amplitude modulation.
// 4 Hann windows at 25% offset sum to exactly 2.0. Output *= 0.5.
// Buffer=16384, FULL_GRAIN=4096. At factor 2.0 each grain reads
// 8192 samples over its lifetime. Safe within buffer.

class PitchShifter
{
  public:
    static constexpr int NUM_GRAINS = 4;
    static constexpr int BUF_SIZE   = 16384;
    static constexpr int BUF_MASK   = BUF_SIZE - 1;
    static constexpr int FULL_GRAIN = 4096;
    static constexpr int QUARTER    = FULL_GRAIN / 4;

    PitchShifter() {}

    void Init(float sr)
    {
        sr_     = sr;
        factor_ = 1.0f;
        Reset();
    }

    void Reset()
    {
        write_pos_ = 0;
        memset(buf_, 0, sizeof(buf_));
        for(int g = 0; g < NUM_GRAINS; g++)
        {
            age_[g]  = g * QUARTER;
            read_[g] = 0.0f;
        }
    }

    void SetFactor(float f)
    {
        if(f != f) f = 1.0f;
        if(f < 0.25f) f = 0.25f;
        if(f > 2.0f)  f = 2.0f;
        factor_ = f;
    }

    float GetFactor() const { return factor_; }

    float Process(float in_sample)
    {
        if(factor_ > 0.99f && factor_ < 1.01f)
            return in_sample;

        buf_[write_pos_] = in_sample;
        write_pos_ = (write_pos_ + 1) & BUF_MASK;

        float sum = 0.0f;
        for(int g = 0; g < NUM_GRAINS; g++)
        {
            float sample = ReadInterp(read_[g]);
            float window = Hann(age_[g]);
            sum += sample * window;

            read_[g] += factor_;
            if(read_[g] >= static_cast<float>(BUF_SIZE))
                read_[g] -= static_cast<float>(BUF_SIZE);

            age_[g]++;

            if(age_[g] >= FULL_GRAIN)
            {
                read_[g] = static_cast<float>(
                    (write_pos_ - (FULL_GRAIN / 2) + BUF_SIZE) & BUF_MASK);
                age_[g] = 0;
            }
        }

        float out = sum * 0.5f;
        if(out > 1.0f)  out = 1.0f;
        if(out < -1.0f) out = -1.0f;
        return out;
    }

  private:
    static constexpr float PS_TWO_PI = 6.28318530f;

    float Hann(int age) const
    {
        if(age >= FULL_GRAIN) return 0.0f;
        float t = static_cast<float>(age) / static_cast<float>(FULL_GRAIN);
        return 0.5f * (1.0f - cosf(PS_TWO_PI * t));
    }

    float ReadInterp(float pos) const
    {
        int i0 = static_cast<int>(pos) & BUF_MASK;
        int i1 = (i0 + 1) & BUF_MASK;
        float frac = pos - floorf(pos);
        return buf_[i0] * (1.0f - frac) + buf_[i1] * frac;
    }

    float buf_[BUF_SIZE];
    float sr_     = 96000.0f;
    float factor_ = 1.0f;
    float read_[NUM_GRAINS];
    int   age_[NUM_GRAINS];
    int   write_pos_ = 0;
};
