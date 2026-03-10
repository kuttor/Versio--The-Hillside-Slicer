#pragma once
#include <cstring>
#include <cmath>
#include "shy_fft.h"

/**
 * Phase Vocoder Pitch Shifter using ShyFFT (Mutable Instruments, MIT)
 *
 * Proven on Daisy Versio via MultiVersio. No CMSIS dependency.
 *
 * Architecture:
 *   1024-point FFT, 4x overlap (hop=256)
 *   Analysis: magnitude + instantaneous frequency per bin
 *   Shift: resample bins in frequency domain
 *   Synthesis: accumulate phase, IFFT, overlap-add
 *
 * ShyFFT output layout: {real[0..N/2-1], imag[0..N/2-1]}
 *
 * Latency: 768 samples = 8ms at 96kHz
 * Memory: ~32KB per instance, ~64KB stereo
 * CPU: ~6% per channel at 96kHz/480MHz
 */
class PitchShifter
{
  public:
    static constexpr int N       = 1024;
    static constexpr int HALF    = N / 2;
    static constexpr int HOP     = N / 4;
    static constexpr int LATENCY = N - HOP;

    PitchShifter() {}

    void Init(float sr)
    {
        sr_ = sr;
        fft_.Init();

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
        memset(in_fifo_, 0, sizeof(in_fifo_));
        memset(out_fifo_, 0, sizeof(out_fifo_));
        memset(output_accum_, 0, sizeof(output_accum_));
        memset(prev_phase_, 0, sizeof(prev_phase_));
        memset(synth_phase_, 0, sizeof(synth_phase_));
    }

    void SoftReset()
    {
        memset(output_accum_, 0, sizeof(output_accum_));
        memset(out_fifo_, 0, sizeof(out_fifo_));
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
        float diff = target_ - factor_;
        if(diff > 0.00005f || diff < -0.00005f)
            factor_ += diff * 0.002f;
        else
            factor_ = target_;

        in_fifo_[rover_] = in_sample;
        float out = out_fifo_[rover_ - LATENCY];

        rover_++;
        if(rover_ >= N)
        {
            rover_ = LATENCY;
            ProcessFrame();
        }

        // Gain compensation
        if(factor_ < 0.97f)
        {
            float comp = 1.0f / sqrtf(factor_);
            if(comp > 1.8f) comp = 1.8f;
            out *= comp;
        }
        else if(factor_ > 1.03f)
        {
            out *= 1.0f + (factor_ - 1.0f) * 0.15f;
        }

        if(out > 1.0f)  out = 1.0f;
        if(out < -1.0f) out = -1.0f;
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
        // Window input
        for(int i = 0; i < N; i++)
            work_[i] = in_fifo_[i] * hann_[i];

        // Forward FFT
        // ShyFFT: output = {real[0..N/2-1], imag[0..N/2-1]}
        fft_.Direct(work_, fft_out_);

        // Analysis: extract magnitude + instantaneous frequency
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

            float delta    = phase - prev_phase_[k];
            prev_phase_[k] = phase;

            float expected = static_cast<float>(k) * expect;
            float dev      = delta - expected;

            // Wrap deviation to [-PI, PI]
            dev = dev + PI;
            dev = dev - TWO_PI * floorf(dev / TWO_PI);
            dev = dev - PI;

            float freq = static_cast<float>(k) * bin_w + dev * inv_hop;

            magnitude_[k] = mag;
            inst_freq_[k] = freq;
        }

        // ── Phase lock: non-peak bins inherit nearest peak's freq ──
        // Eliminates random phase drift in partials (main phasiness source)
        bool is_peak[HALF];
        is_peak[0] = (magnitude_[0] > magnitude_[1]);
        is_peak[HALF-1] = (magnitude_[HALF-1] > magnitude_[HALF-2]);
        for(int k = 1; k < HALF - 1; k++)
            is_peak[k] = (magnitude_[k] >= magnitude_[k-1]
                       && magnitude_[k] >= magnitude_[k+1]);

        int last_peak = 0;
        for(int k = 0; k < HALF; k++)
        {
            if(is_peak[k]) { last_peak = k; continue; }
            int next_peak = last_peak;
            for(int j = k + 1; j < HALF; j++)
                if(is_peak[j]) { next_peak = j; break; }
            int nearest = (k - last_peak <= next_peak - k)
                          ? last_peak : next_peak;
            inst_freq_[k] = inst_freq_[nearest];
        }

        // ── Magnitude gate: zero bins below noise floor ──
        float max_mag = 0.0f;
        for(int k = 0; k < HALF; k++)
            if(magnitude_[k] > max_mag) max_mag = magnitude_[k];
        float gate = max_mag * 0.003f;
        for(int k = 0; k < HALF; k++)
            if(magnitude_[k] < gate) magnitude_[k] = 0.0f;

        // Pitch shift: resample bins
        memset(synth_mag_, 0, HALF * sizeof(float));
        memset(synth_freq_, 0, HALF * sizeof(float));

        for(int k = 0; k < HALF; k++)
        {
            int dest = static_cast<int>(static_cast<float>(k) * factor_ + 0.5f);
            if(dest >= 0 && dest < HALF)
            {
                synth_mag_[dest]  += magnitude_[k];
                synth_freq_[dest]  = inst_freq_[k] * factor_;
            }
        }

        // Synthesis: accumulate phase, rebuild spectrum
        for(int k = 0; k < HALF; k++)
        {
            synth_phase_[k] += synth_freq_[k] * hop_f;

            float mag = synth_mag_[k];
            float ph  = synth_phase_[k];

            fft_out_[k]        = mag * cosf(ph);
            fft_out_[k + HALF] = -(mag * sinf(ph));  // Negate back for ShyFFT
        }

        // Inverse FFT
        fft_.Inverse(fft_out_, work_);

        // Scale (shy_fft doesn't normalize) + synthesis window
        float scale = 1.0f / static_cast<float>(N);
        for(int i = 0; i < N; i++)
            work_[i] *= scale * hann_[i];

        // Shift output accumulator
        memmove(output_accum_, output_accum_ + HOP,
                N * sizeof(float));
        memset(output_accum_ + N, 0, HOP * sizeof(float));

        // Overlap-add (normalize for 4x Hann^2 overlap: 2/3)
        for(int i = 0; i < N; i++)
            output_accum_[i] += work_[i] * 0.6667f;

        // Output
        memcpy(out_fifo_, output_accum_, HOP * sizeof(float));

        // Shift input
        memmove(in_fifo_, in_fifo_ + HOP, LATENCY * sizeof(float));
    }

    ShyFFT<float, N, RotationPhasor> fft_;

    float hann_[N];
    float work_[N];
    float fft_out_[N];

    float prev_phase_[HALF];
    float magnitude_[HALF];
    float inst_freq_[HALF];

    float synth_phase_[HALF];
    float synth_mag_[HALF];
    float synth_freq_[HALF];

    float in_fifo_[N];
    float out_fifo_[N];
    float output_accum_[N + HOP];

    float sr_     = 96000.0f;
    float factor_ = 1.0f;
    float target_ = 1.0f;
    int   rover_  = LATENCY;
};
