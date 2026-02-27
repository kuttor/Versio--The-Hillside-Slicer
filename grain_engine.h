#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "slice_engine.h"

/**
 * GranularTimestretch - Proper decoupled pitch/time via overlap-add granular synthesis.
 * 
 * How it works:
 *   - A "transport" position advances through the slice at the SPEED rate (timestretch)
 *   - Grains are spawned at regular intervals from the transport position
 *   - Each grain reads from the buffer at the PITCH rate (independent of transport)
 *   - Grains are windowed with a raised cosine (Hanning) for smooth overlap
 *   - Multiple overlapping grains create continuous output
 * 
 * This decouples pitch from time:
 *   - Speed 0.5x, Pitch 1.0 = half speed, original pitch (timestretch)
 *   - Speed 1.0,  Pitch 2.0 = original speed, octave up (pitch shift)
 *   - Speed 2.0,  Pitch 0.5 = double speed, octave down (both independent)
 */

static constexpr uint32_t MAX_GRAINS     = 12;
static constexpr uint32_t MIN_GRAIN_SIZE = 256;   // ~2.7ms at 96kHz
static constexpr uint32_t MAX_GRAIN_SIZE = 19200;  // ~200ms at 96kHz

struct Grain
{
    float    read_pos     = 0.0f;
    float    pitch_rate   = 1.0f;
    float    phase        = 0.0f;  // 0-1 envelope phase
    float    phase_inc    = 0.0f;  // Per-sample phase increment
    uint32_t slice_start  = 0;
    uint32_t slice_len    = 0;
    bool     active       = false;
    bool     reverse      = false;

    /** Hanning window at current phase */
    float Window() const
    {
        return 0.5f * (1.0f - cosf(phase * 6.2831853f));
    }

    /** Advance grain one sample. Returns false when done. */
    bool Process()
    {
        if(!active)
            return false;

        phase += phase_inc;
        if(phase >= 1.0f)
        {
            active = false;
            return false;
        }

        // Advance read position at pitch rate
        if(reverse)
            read_pos -= pitch_rate;
        else
            read_pos += pitch_rate;

        // Wrap within slice
        if(read_pos >= static_cast<float>(slice_len))
            read_pos -= static_cast<float>(slice_len);
        else if(read_pos < 0.0f)
            read_pos += static_cast<float>(slice_len);

        return true;
    }

    float GetAbsolutePosition() const
    {
        return static_cast<float>(slice_start) + read_pos;
    }
};

class PlaybackEngine
{
  public:
    PlaybackEngine() {}

    void Init(SliceEngine* slices, float sr)
    {
        slices_       = slices;
        sample_rate_  = sr;
        current_slice_ = 0;
        start_slice_  = 0;
        play_length_  = 16;
        pitch_ratio_  = 1.0f;
        speed_ratio_  = 1.0f;
        grain_size_   = 4800;
        overlap_      = 4;
        transport_pos_ = 0.0f;
        samples_since_grain_ = 0;
        grain_interval_ = grain_size_ / overlap_;
        is_playing_   = false;
        reverse_      = false;
        auto_advance_ = true;

        for(uint32_t i = 0; i < MAX_GRAINS; i++)
            grains_[i] = Grain();
    }

    // ── Slice triggering ───────────────────────────────────────

    void TriggerChoke()
    {
        if(!slices_ || slices_->GetState() != RecordState::PLAYING)
            return;
        for(uint32_t i = 0; i < MAX_GRAINS; i++)
            grains_[i].active = false;
        AdvanceSlice();
        ResetTransport();
        is_playing_ = true;
    }

    void TriggerOpen()
    {
        if(!slices_ || slices_->GetState() != RecordState::PLAYING)
            return;
        // Existing grains keep playing, fade out naturally via window
        AdvanceSlice();
        ResetTransport();
        is_playing_ = true;
    }

    void TriggerAuto()
    {
        if(!slices_ || slices_->GetState() != RecordState::PLAYING)
            return;
        AdvanceSlice();
        ResetTransport();
        is_playing_ = true;
    }

    // ── Parameters ─────────────────────────────────────────────

    void SetStartSlice(float normalized)
    {
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return;
        start_slice_ = static_cast<uint32_t>(normalized * (hdr.num_slices - 1));
        start_slice_ = std::min(start_slice_, hdr.num_slices - 1);
    }

    void SetPlayLength(float normalized)
    {
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return;
        play_length_ = 1 + static_cast<uint32_t>(normalized * (hdr.num_slices - 1));
        play_length_ = std::min(play_length_, hdr.num_slices);
    }

    /** Speed / timestretch (KNOB_2) - transport rate, independent of pitch */
    void SetSpeed(float normalized)
    {
        if(normalized < 0.08f)       speed_ratio_ = 0.25f;
        else if(normalized < 0.25f)  speed_ratio_ = 0.333f;
        else if(normalized < 0.42f)  speed_ratio_ = 0.5f;
        else if(normalized < 0.58f)  speed_ratio_ = 1.0f;
        else if(normalized < 0.75f)  speed_ratio_ = 2.0f;
        else if(normalized < 0.92f)  speed_ratio_ = 3.0f;
        else                         speed_ratio_ = 4.0f;
        RecalcGrainInterval();
    }

    /** Pitch shift (KNOB_3) - grain read rate, independent of speed */
    void SetPitch(float normalized)
    {
        float semitones = (normalized - 0.5f) * 24.0f;
        pitch_ratio_ = powf(2.0f, semitones / 12.0f);
    }

    /** Overlap factor (KNOB_4) - 2x (rhythmic) to 8x (smooth) */
    void SetOverlap(float normalized)
    {
        overlap_ = 2 + static_cast<uint32_t>(normalized * 6.0f);
        overlap_ = (overlap_ < 2) ? 2 : (overlap_ > 8) ? 8 : overlap_;
        RecalcGrainInterval();
    }

    /** Grain size (KNOB_5) - small=glitchy, large=smooth. THE character knob. */
    void SetGrainSize(float normalized)
    {
        float t = normalized * normalized; // Exponential curve for more resolution at small sizes
        grain_size_ = MIN_GRAIN_SIZE + static_cast<uint32_t>(t * (MAX_GRAIN_SIZE - MIN_GRAIN_SIZE));
        RecalcGrainInterval();
    }

    void SetReverse(bool rev) { reverse_ = rev; }
    void SetAutoAdvance(bool aa) { auto_advance_ = aa; }

    // ── Audio processing ───────────────────────────────────────

    void Process(float& out_l, float& out_r)
    {
        out_l = 0.0f;
        out_r = 0.0f;

        if(!slices_ || !is_playing_ || slices_->GetState() != RecordState::PLAYING)
            return;

        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0 || hdr.total_samples == 0)
            return;

        // ── Advance transport at speed rate ────────────────
        if(reverse_)
            transport_pos_ -= speed_ratio_;
        else
            transport_pos_ += speed_ratio_;

        uint32_t sidx = current_slice_ % hdr.num_slices;
        const SliceInfo& slice = hdr.slices[sidx];

        // Wrap transport within slice
        if(transport_pos_ >= static_cast<float>(slice.length))
            transport_pos_ -= static_cast<float>(slice.length);
        else if(transport_pos_ < 0.0f)
            transport_pos_ += static_cast<float>(slice.length);

        // ── Spawn grains at regular intervals ──────────────
        samples_since_grain_++;
        if(samples_since_grain_ >= grain_interval_)
        {
            samples_since_grain_ = 0;
            SpawnGrain(slice.start_sample, slice.length);
        }

        // ── Mix all active grains ──────────────────────────
        float mix_l = 0.0f;
        float mix_r = 0.0f;

        for(uint32_t i = 0; i < MAX_GRAINS; i++)
        {
            if(!grains_[i].active)
                continue;

            if(grains_[i].Process())
            {
                float l, r;
                slices_->ReadSample(grains_[i].GetAbsolutePosition(), l, r);
                float amp = grains_[i].Window();
                mix_l += l * amp;
                mix_r += r * amp;
            }
        }

        // Normalize: N overlapping Hanning windows sum to ~N/2
        if(overlap_ > 0)
        {
            float norm = 2.0f / static_cast<float>(overlap_);
            mix_l *= norm;
            mix_r *= norm;
        }

        out_l = SoftClip(mix_l);
        out_r = SoftClip(mix_r);
    }

    // ── State queries ──────────────────────────────────────────

    uint32_t GetCurrentSlice() const { return current_slice_; }

    float GetPlaybackProgress() const
    {
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0 || play_length_ == 0)
            return 0.0f;
        uint32_t rel = (current_slice_ >= start_slice_)
                       ? (current_slice_ - start_slice_) : 0;
        return static_cast<float>(rel) / static_cast<float>(play_length_);
    }

    bool  IsPlaying() const { return is_playing_; }
    float GetSpeedRatio() const { return speed_ratio_; }
    float GetPitchRatio() const { return pitch_ratio_; }

  private:
    void AdvanceSlice()
    {
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return;
        current_slice_++;
        if(current_slice_ >= start_slice_ + play_length_ ||
           current_slice_ >= hdr.num_slices)
        {
            current_slice_ = start_slice_;
        }
    }

    void ResetTransport()
    {
        transport_pos_       = 0.0f;
        samples_since_grain_ = grain_interval_; // Spawn immediately
    }

    void RecalcGrainInterval()
    {
        grain_interval_ = grain_size_ / overlap_;
        if(grain_interval_ < 1) grain_interval_ = 1;
    }

    void SpawnGrain(uint32_t slice_start, uint32_t slice_len)
    {
        int32_t idx = -1;
        for(uint32_t i = 0; i < MAX_GRAINS; i++)
        {
            if(!grains_[i].active) { idx = i; break; }
        }

        // Steal oldest grain if pool full
        if(idx < 0)
        {
            float max_phase = -1.0f;
            for(uint32_t i = 0; i < MAX_GRAINS; i++)
            {
                if(grains_[i].phase > max_phase)
                {
                    max_phase = grains_[i].phase;
                    idx = i;
                }
            }
        }

        if(idx >= 0)
        {
            Grain& g      = grains_[idx];
            g.active       = true;
            g.phase        = 0.0f;
            g.phase_inc    = 1.0f / static_cast<float>(grain_size_);
            g.pitch_rate   = pitch_ratio_;
            g.slice_start  = slice_start;
            g.slice_len    = slice_len;
            g.reverse      = reverse_;
            g.read_pos     = transport_pos_;

            if(g.read_pos >= static_cast<float>(slice_len))
                g.read_pos = fmodf(g.read_pos, static_cast<float>(slice_len));
            if(g.read_pos < 0.0f)
                g.read_pos += static_cast<float>(slice_len);
        }
    }

    static float SoftClip(float x)
    {
        if(x > 1.5f) return 1.0f;
        if(x < -1.5f) return -1.0f;
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    SliceEngine* slices_       = nullptr;
    float        sample_rate_  = 96000.0f;
    Grain        grains_[MAX_GRAINS];

    uint32_t current_slice_ = 0;
    uint32_t start_slice_   = 0;
    uint32_t play_length_   = 16;

    float    pitch_ratio_   = 1.0f;
    float    speed_ratio_   = 1.0f;
    uint32_t grain_size_    = 4800;
    uint32_t overlap_       = 4;
    uint32_t grain_interval_ = 1200;

    float    transport_pos_       = 0.0f;
    uint32_t samples_since_grain_ = 0;

    bool     reverse_       = false;
    bool     auto_advance_  = true;
    bool     is_playing_    = false;
};
