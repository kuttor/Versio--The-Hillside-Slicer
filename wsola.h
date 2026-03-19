#pragma once
#include <cmath>
#include <cstring>

/**
 * WSOLA Time Stretcher — Waveform Similarity Overlap-Add
 *
 * Stretches or compresses audio in time WITHOUT changing pitch.
 * No FFT. No phase vocoder. No spectral processing.
 * Just overlapping chunks with cross-correlation splice matching.
 *
 * Used for swing: even groups stretch (slower), odd groups compress (faster).
 *
 * How it works:
 *   1. Read overlapping frames from source buffer
 *   2. For each new frame, search ±SEARCH samples around the expected
 *      position for the best waveform match (cross-correlation)
 *   3. Hanning-window the frame and overlap-add into output
 *   4. Output samples from the accumulator
 *
 * The cross-correlation search is what makes splices invisible —
 * it finds where the waveform naturally lines up.
 */
class WsolaStretcher
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        Reset();

        // Pre-compute Hanning window
        for(int i = 0; i < FRAME; i++)
        {
            float t = static_cast<float>(i) / static_cast<float>(FRAME - 1);
            hann_[i] = 0.5f * (1.0f - cosf(6.28318530f * t));
        }
    }

    void Reset()
    {
        input_pos_      = 0.0;
        output_counter_ = 0;
        ratio_          = 1.0f;
        smooth_ratio_   = 1.0f;
        memset(accum_, 0, sizeof(accum_));
        accum_rd_ = 0;
        accum_wr_ = 0;
        primed_   = false;
    }

    /** Set stretch ratio. 1.0 = normal. >1.0 = slower. <1.0 = faster.
     *  Smoothed internally to avoid clicks. */
    void SetRatio(float r)
    {
        if(r < 0.4f) r = 0.4f;
        if(r > 2.5f) r = 2.5f;
        ratio_ = r;
    }

    /** Process one output sample from the source buffer.
     *  source: pointer to the FULL slice buffer
     *  source_len: total samples in the slice
     *  base_read_pos: where the engine's read_pos currently is (fractional)
     *  Returns the time-stretched sample. */
    float Process(const float* source, uint32_t source_len,
                  double base_read_pos)
    {
        if(source_len < FRAME * 2 || !source) return 0.0f;

        // Smooth ratio per sample
        smooth_ratio_ += 0.003f * (ratio_ - smooth_ratio_);

        // Prime on first call: fill accumulator with first frame
        if(!primed_)
        {
            input_pos_ = base_read_pos;
            SynthFrame(source, source_len);
            primed_ = true;
        }

        // Read one sample from accumulator
        float out = accum_[accum_rd_];
        accum_[accum_rd_] = 0.0f;
        accum_rd_ = (accum_rd_ + 1) & ACCUM_MASK;

        output_counter_++;

        // Every HOP output samples, synthesize a new overlapping frame
        if(output_counter_ >= HOP)
        {
            output_counter_ = 0;

            // Advance input position by HOP * ratio
            // ratio > 1 = input advances slower = audio stretches
            // ratio < 1 = input advances faster = audio compresses
            input_pos_ += static_cast<double>(HOP) * static_cast<double>(smooth_ratio_);

            // Wrap within source
            if(input_pos_ >= static_cast<double>(source_len))
                input_pos_ -= static_cast<double>(source_len);
            if(input_pos_ < 0.0)
                input_pos_ += static_cast<double>(source_len);

            SynthFrame(source, source_len);
        }

        return out;
    }

    /** Check if the stretcher is active (has been primed). */
    bool IsActive() const { return primed_; }

    /** Reset for a new slice — call when slice changes. */
    void ResetForNewSlice()
    {
        primed_ = false;
        output_counter_ = 0;
        memset(accum_, 0, sizeof(accum_));
        accum_rd_ = 0;
        accum_wr_ = 0;
    }

  private:
    // Frame/overlap parameters tuned for 96kHz
    static constexpr int FRAME  = 512;    // ~5.3ms analysis frame
    static constexpr int HOP    = 256;    // 50% overlap = 256 samples
    static constexpr int SEARCH = 48;     // ±48 sample search window

    // Ring buffer for overlap-add output
    static constexpr int ACCUM_SIZE = 2048;
    static constexpr int ACCUM_MASK = ACCUM_SIZE - 1;

    float hann_[FRAME];
    float accum_[ACCUM_SIZE];
    int   accum_rd_ = 0;
    int   accum_wr_ = 0;

    double input_pos_    = 0.0;
    int    output_counter_ = 0;
    float  ratio_        = 1.0f;
    float  smooth_ratio_ = 1.0f;
    float  sr_           = 96000.0f;
    bool   primed_       = false;

    /** Find best splice offset using cross-correlation. */
    int FindBestOffset(const float* source, uint32_t len, int center)
    {
        // Reference: last HOP samples of accumulator (what we just output)
        // Candidate: source around center ± SEARCH
        // Find offset that maximizes dot product

        int best_offset = 0;
        float best_corr = -1e30f;

        for(int offset = -SEARCH; offset <= SEARCH; offset++)
        {
            float corr = 0.0f;
            int pos = center + offset;

            // Compute correlation over a short window (128 samples)
            for(int j = 0; j < 128; j++)
            {
                int src_idx = pos + j;
                // Wrap within source buffer
                while(src_idx < 0) src_idx += static_cast<int>(len);
                while(src_idx >= static_cast<int>(len))
                    src_idx -= static_cast<int>(len);

                // Compare with what's already in the accumulator
                int acc_idx = (accum_wr_ + j) & ACCUM_MASK;
                corr += source[src_idx] * accum_[acc_idx];
            }

            if(corr > best_corr)
            {
                best_corr = corr;
                best_offset = offset;
            }
        }

        return best_offset;
    }

    /** Synthesize one FRAME of audio into the overlap-add accumulator. */
    void SynthFrame(const float* source, uint32_t len)
    {
        int center = static_cast<int>(input_pos_);

        // Search for best splice point
        int offset = FindBestOffset(source, len, center);
        int read_start = center + offset;

        // Overlap-add: Hanning-windowed frame into accumulator
        for(int i = 0; i < FRAME; i++)
        {
            int src_idx = read_start + i;
            // Wrap within source
            while(src_idx < 0) src_idx += static_cast<int>(len);
            while(src_idx >= static_cast<int>(len))
                src_idx -= static_cast<int>(len);

            int acc_idx = (accum_wr_ + i) & ACCUM_MASK;
            accum_[acc_idx] += source[src_idx] * hann_[i];
        }

        // Advance write pointer by HOP
        accum_wr_ = (accum_wr_ + HOP) & ACCUM_MASK;
    }
};
