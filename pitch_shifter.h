#pragma once
#include <cstring>
#include <cmath>
#include "shy_fft.h"

/**
 * Phase Vocoder Pitch Shifter — v2
 *
 * 2048-point FFT, 4x overlap (hop=512), 48kHz
 * Laroche-Dolson identity phase locking (eliminates metallic/reverberant buzz)
 * Delay-matched warm bypass at unity (vocoder keeps running, output swapped)
 * Crossfade on bypass transitions (no pop on pitch direction change)
 *
 * Latency: 1536 samples = 32ms at 48kHz
 * Memory: ~92KB per instance, ~184KB stereo
 * CPU: ~5-8% per channel at 48kHz/480MHz (half the frames vs 96kHz)
 */
class PitchShifter
{
  public:
    static constexpr int N       = 2048;
    static constexpr int HALF    = N / 2;
    static constexpr int HOP     = N / 4;
    static constexpr int LATENCY = N - HOP;

    // Bypass crossfade: 512 samples = ~10ms at 48kHz
    static constexpr int BYPASS_FADE = 512;

    PitchShifter() {}

    void Init(float sr)
    {
        sr_ = sr;
        fft_.Init();

        // Periodic Hann window: w[n] = 0.5*(1 - cos(2*pi*n/N))
        for(int i = 0; i < N; i++)
        {
            float t = static_cast<float>(i) / static_cast<float>(N);
            hann_[i] = 0.5f * (1.0f - cosf(6.28318530f * t));
        }
        Reset();
    }

    void Reset()
    {
        rover_  = LATENCY;
        factor_ = 1.0f;
        target_ = 1.0f;
        bypass_active_ = true;
        bypass_fade_counter_ = 0;
        memset(in_fifo_, 0, sizeof(in_fifo_));
        memset(out_fifo_, 0, sizeof(out_fifo_));
        memset(output_accum_, 0, sizeof(output_accum_));
        memset(prev_phase_, 0, sizeof(prev_phase_));
        memset(synth_phase_, 0, sizeof(synth_phase_));
        memset(delay_buf_, 0, sizeof(delay_buf_));
        delay_wr_ = 0;
    }

    /** Phase reset for buffer discontinuities (loop wrap, slice jump).
     *  Clears synthesis phase so stale spectral history doesn't corrupt
     *  new audio. Keeps in_fifo_ (no silence gap) and pitch factor. */
    void ResetForDiscontinuity()
    {
        factor_ = target_;
        memset(output_accum_, 0, sizeof(output_accum_));
        memset(out_fifo_, 0, sizeof(out_fifo_));
        memset(prev_phase_, 0, sizeof(prev_phase_));
        memset(synth_phase_, 0, sizeof(synth_phase_));
        rover_ = LATENCY;
    }

    void SetFactor(float f)
    {
        if(f != f) f = 1.0f;
        if(f < 0.5f)  f = 0.5f;
        if(f > 2.0f)  f = 2.0f;
        target_ = f;
    }

    float GetFactor() const { return factor_; }

    float Process(float in_sample)
    {
        // Smooth factor toward target
        float diff = target_ - factor_;
        if(diff > 0.00005f || diff < -0.00005f)
            factor_ += diff * 0.001f;  // Slower smoothing for 2048-point
        else
            factor_ = target_;

        // ── Delay buffer: raw input delayed by LATENCY ──
        // Both bypass and vocoder output are LATENCY samples behind input.
        // This means switching between them is phase-aligned.
        delay_buf_[delay_wr_] = in_sample;
        int delay_rd = delay_wr_ - LATENCY;
        if(delay_rd < 0) delay_rd += DELAY_BUF_SIZE;
        float delayed_raw = delay_buf_[delay_rd];
        delay_wr_ = (delay_wr_ + 1) & DELAY_BUF_MASK;

        // ── Always run vocoder (keeps state warm for transitions) ──
        in_fifo_[rover_] = in_sample;
        float vocoder_out = out_fifo_[rover_ - LATENCY];

        rover_++;
        if(rover_ >= N)
        {
            rover_ = LATENCY;
            ProcessFrame();
        }

        // Gain compensation for pitch shift
        float eff = factor_;
        if(eff < 0.97f)
        {
            float comp = 1.0f / sqrtf(eff);
            if(comp > 1.5f) comp = 1.5f;
            vocoder_out *= comp;
        }
        else if(eff > 1.03f)
        {
            vocoder_out *= 1.0f + (eff - 1.0f) * 0.1f;
        }

        // Soft limit
        if(vocoder_out >  1.5f) vocoder_out =  1.5f;
        if(vocoder_out < -1.5f) vocoder_out = -1.5f;

        // ── Bypass logic with hysteresis + crossfade ──
        // At unity: output delayed raw (clean, no PV artifacts).
        // Vocoder runs either way so state is always warm.
        bool want_bypass = bypass_active_;
        float deviation = fabsf(factor_ - 1.0f);
        if(bypass_active_)
        {
            if(deviation > 0.025f) want_bypass = false;
        }
        else
        {
            if(deviation < 0.008f) want_bypass = true;
        }

        // State change triggers crossfade
        if(want_bypass != bypass_active_)
        {
            bypass_active_ = want_bypass;
            bypass_fade_counter_ = BYPASS_FADE;
        }

        // Output selection with crossfade
        float out;
        if(bypass_fade_counter_ > 0)
        {
            float t = static_cast<float>(bypass_fade_counter_)
                      / static_cast<float>(BYPASS_FADE);
            if(bypass_active_)
            {
                // Fading TO bypass: vocoder→raw
                out = delayed_raw * (1.0f - t) + vocoder_out * t;
            }
            else
            {
                // Fading FROM bypass: raw→vocoder
                out = vocoder_out * (1.0f - t) + delayed_raw * t;
            }
            bypass_fade_counter_--;
        }
        else if(bypass_active_)
        {
            out = delayed_raw;
        }
        else
        {
            out = vocoder_out;
        }

        return out;
    }

  private:
    static constexpr float TWO_PI = 6.28318530f;
    static constexpr float PI     = 3.14159265f;

    static float FastAtan2(float y, float x)
    {
        float abs_y = fabsf(y) + 1e-10f;
        float r, angle;
        if(x >= 0.0f)
        {
            r = (x - abs_y) / (x + abs_y);
            angle = 0.7854f - 0.7854f * r;
        }
        else
        {
            r = (x + abs_y) / (abs_y - x);
            angle = 2.3562f - 0.7854f * r;
        }
        return (y < 0.0f) ? -angle : angle;
    }

    void ProcessFrame()
    {
        // ── Analysis: window + FFT ──
        for(int i = 0; i < N; i++)
            work_[i] = in_fifo_[i] * hann_[i];

        fft_.Direct(work_, fft_out_);

        // Extract magnitude + instantaneous frequency + analysis phase
        float hop_f   = static_cast<float>(HOP);
        float expect  = TWO_PI * hop_f / static_cast<float>(N);
        float inv_hop = 1.0f / hop_f;
        float bin_w   = TWO_PI / static_cast<float>(N);

        for(int k = 0; k < HALF; k++)
        {
            float re = fft_out_[k];
            float im = -fft_out_[k + HALF];  // ShyFFT negates imaginary

            float mag   = sqrtf(re * re + im * im);
            float phase = FastAtan2(im, re);

            // Store analysis phase BEFORE overwriting prev_phase_
            analysis_phase_[k] = phase;

            float delta    = phase - prev_phase_[k];
            prev_phase_[k] = phase;

            float expected = static_cast<float>(k) * expect;
            float dev      = delta - expected;

            // Wrap to [-PI, PI]
            dev = dev + PI;
            dev = dev - TWO_PI * floorf(dev / TWO_PI);
            dev = dev - PI;

            float freq = static_cast<float>(k) * bin_w + dev * inv_hop;

            magnitude_[k] = mag;
            inst_freq_[k] = freq;
        }

        // ── Pitch shift: remap bins ──
        memset(synth_mag_, 0, HALF * sizeof(float));
        memset(synth_freq_, 0, HALF * sizeof(float));

        float eff_factor = factor_;
        if(eff_factor < 0.5f) eff_factor = 0.5f;
        if(eff_factor > 2.0f) eff_factor = 2.0f;

        for(int k = 0; k < HALF; k++)
        {
            int dest = static_cast<int>(static_cast<float>(k) * eff_factor + 0.5f);
            if(dest >= 0 && dest < HALF)
            {
                synth_mag_[dest]  += magnitude_[k];
                synth_freq_[dest]  = inst_freq_[k] * eff_factor;
            }
        }

        // ── Standard phase propagation ──
        for(int k = 0; k < HALF; k++)
            prop_phase_[k] = synth_phase_[k] + synth_freq_[k] * hop_f;

        // ── Laroche-Dolson identity phase locking ──
        // 1. Find peaks in synthesis magnitude
        // 2. Compute rotation at each peak
        // 3. Apply peak's rotation to its region of influence
        // This preserves inter-bin phase relationships from analysis,
        // eliminating the metallic/reverberant quality of basic PV.

        // Find peaks and assign regions of influence
        memset(peak_region_, 0, HALF * sizeof(int));
        int last_peak = 0;

        for(int k = 1; k < HALF - 1; k++)
        {
            if(synth_mag_[k] > synth_mag_[k - 1]
               && synth_mag_[k] >= synth_mag_[k + 1]
               && synth_mag_[k] > 1e-10f)
            {
                // k is a peak. Assign region from midpoint of last peak
                // to midpoint toward next peak (next peak found later).
                int region_start = (last_peak + k) / 2;
                if(last_peak == 0) region_start = 0;

                // Mark all bins from region_start to k as belonging to peak k
                for(int j = region_start; j <= k; j++)
                    peak_region_[j] = k;

                last_peak = k;
            }
        }
        // Fill remaining bins to last peak
        for(int k = last_peak; k < HALF; k++)
            peak_region_[k] = last_peak;

        // Apply phase locking: non-peak bins inherit the rotation
        // from their peak, preserving the analysis phase relationship.
        for(int k = 0; k < HALF; k++)
        {
            int p = peak_region_[k];
            if(p == k || synth_mag_[k] < 1e-10f)
            {
                // Peak bin or silent: use propagated phase
                synth_phase_[k] = prop_phase_[k];
            }
            else
            {
                // Non-peak: lock to peak's rotation
                // φ_locked = φ_prop[peak] + (φ_analysis[k] - φ_analysis[peak])
                float rotation = prop_phase_[p] - analysis_phase_[p];
                synth_phase_[k] = analysis_phase_[k] + rotation;
            }
        }

        // ── Synthesis: rebuild spectrum from magnitude + locked phase ──
        for(int k = 0; k < HALF; k++)
        {
            float mag = synth_mag_[k];
            float ph  = synth_phase_[k];

            fft_out_[k]        = mag * cosf(ph);
            fft_out_[k + HALF] = -(mag * sinf(ph));  // Negate for ShyFFT
        }

        // Inverse FFT
        fft_.Inverse(fft_out_, work_);

        // Scale (ShyFFT unnormalized) + synthesis window
        float scale = 1.0f / static_cast<float>(N);
        for(int i = 0; i < N; i++)
            work_[i] *= scale * hann_[i];

        // Shift output accumulator left by HOP
        memmove(output_accum_, output_accum_ + HOP,
                N * sizeof(float));
        memset(output_accum_ + N, 0, HOP * sizeof(float));

        // Overlap-add: WOLA Hann^2 at 75% overlap sums to 1.5
        // Normalization: 2/3 = 0.6667
        for(int i = 0; i < N; i++)
            output_accum_[i] += work_[i] * 0.6667f;

        // Copy to output FIFO
        memcpy(out_fifo_, output_accum_, HOP * sizeof(float));

        // Shift input FIFO
        memmove(in_fifo_, in_fifo_ + HOP, LATENCY * sizeof(float));
    }

    // ── FFT engine ──
    ShyFFT<float, N, RotationPhasor> fft_;

    // ── Windows and work buffers ──
    float hann_[N];
    float work_[N];
    float fft_out_[N];

    // ── Analysis state ──
    float prev_phase_[HALF];
    float analysis_phase_[HALF];   // Current frame's analysis phases
    float magnitude_[HALF];
    float inst_freq_[HALF];

    // ── Synthesis state ──
    float synth_phase_[HALF];
    float prop_phase_[HALF];       // Propagated phase (before locking)
    float synth_mag_[HALF];
    float synth_freq_[HALF];
    int   peak_region_[HALF];      // Maps each bin to its nearest peak

    // ── FIFO buffers ──
    float in_fifo_[N];
    float out_fifo_[N];
    float output_accum_[N + HOP];

    // ── Delay buffer for bypass ──
    static constexpr int DELAY_BUF_SIZE = 2048;
    static constexpr int DELAY_BUF_MASK = DELAY_BUF_SIZE - 1;
    float delay_buf_[DELAY_BUF_SIZE];
    int   delay_wr_ = 0;

    // ── State ──
    float sr_     = 48000.0f;
    float factor_ = 1.0f;
    float target_ = 1.0f;
    int   rover_  = LATENCY;

    // ── Bypass ──
    bool bypass_active_       = true;
    int  bypass_fade_counter_ = 0;
};
