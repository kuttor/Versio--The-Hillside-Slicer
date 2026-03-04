#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include "slice_engine.h"
#include "pitch_shifter.h"

/**
 * PlaybackEngine - Tape-style slicer with phase vocoder pitch correction.
 *
 * SPEED (top center, KNOB_2):
 *   Tape. Dead zone 0.45–0.55 = exactly 1.0x.
 *
 * PITCH (bottom center, KNOB_3):
 *   Granular pitch shift. Cubic S-curve, ±12 semi (0.5x-2.0x).
 *   Tiny dead zone at noon. Always runs (no bypass discontinuity).
 *
 * Play modes:
 *   LOOP    (SW_1 up):     loops forever
 *   ONESHOT (SW_1 center): plays once, stops at end of range
 *   [future](SW_1 down):   reserved
 */

static constexpr uint32_t XFADE_MAX     = 1024;
static constexpr uint32_t GATE_FADE_SAMPLES = 384;
// Crossfade adapts to slice length — never more than 12% of a slice
// so short slices (high slice counts) don't get eaten by fades.

enum class PlaybackMode
{
    LOOP,
    ONESHOT,
    RESAMPLE // future
};

class PlaybackEngine
{
  public:
    PlaybackEngine() {}

    void Init(SliceEngine* slices, float sr)
    {
        slices_      = slices;
        sample_rate_ = sr;
        pitch_l_.Init(sr);
        pitch_r_.Init(sr);
        Reset();
    }

    void Reset()
    {
        start_slice_    = 0;
        end_slice_      = 15;
        speed_ratio_    = 1.0f;
        slice_gate_     = 1.0f;
        is_playing_     = false;
        read_pos_       = 0.0;
        slice_timer_    = 0.0;
        current_slice_  = 0;
        prev_out_l_     = 0.0f;
        prev_out_r_     = 0.0f;
        xfade_counter_  = 0;
        xfade_prev_l_   = 0.0f;
        xfade_prev_r_   = 0.0f;
        prev_start_     = 0;
        start_flash_timer_ = 0;
        gate_fade_counter_ = 0;
        gate_was_open_     = true;
        play_mode_      = PlaybackMode::LOOP;
        oneshot_done_   = false;
        pitch_l_.Reset();
        pitch_r_.Reset();
    }

    void StartPlayback()
    {
        current_slice_ = start_slice_;
        read_pos_    = 0.0;
        slice_timer_ = 0.0;
        is_playing_  = true;
        oneshot_done_ = false;
        prev_out_l_  = 0.0f;
        prev_out_r_  = 0.0f;
        xfade_counter_ = XFADE_MAX;
        xfade_total_   = XFADE_MAX;
        xfade_prev_l_ = 0.0f;
        xfade_prev_r_ = 0.0f;
        gate_fade_counter_ = 0;
        gate_was_open_     = true;

        // Reset pitch buffers: fresh prime with new audio
        // Without this, WSOLA correlates against stale data
        pitch_l_.Reset();
        pitch_r_.Reset();
    }

    void StopPlayback() { is_playing_ = false; oneshot_done_ = false; }

    /** Reset playhead to start without clearing pitch state.
     *  Gate trigger in loop mode for seamless restart. */
    void ResetPlayhead()
    {
        if(!is_playing_) return;
        xfade_prev_l_ = prev_out_l_;
        xfade_prev_r_ = prev_out_r_;
        current_slice_ = start_slice_;
        slice_timer_   = 0.0;
        read_pos_      = 0.0;
        xfade_total_   = XFADE_MAX;
        xfade_counter_ = XFADE_MAX;
        gate_was_open_ = true;
        gate_fade_counter_ = 0;
    }

    void ResetPitchShifters()
    {
        pitch_l_.Reset();
        pitch_r_.Reset();
    }

    /** Call when slices have been recalculated under an active loop.
     *  Resets read position within the current slice to avoid
     *  read_pos/slice_timer exceeding the new (possibly shorter) slice. */
    void ResyncSlices()
    {
        if(!is_playing_ || !slices_) return;
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return;

        // Clamp current_slice_ into valid range
        uint32_t s0 = ClampU(start_slice_, hdr.num_slices);
        uint32_t s1 = ClampU(end_slice_, hdr.num_slices);
        if(s1 < s0) s1 = s0;

        if(current_slice_ > s1 || current_slice_ < s0)
            current_slice_ = s0;

        // Crossfade from whatever was playing into clean restart
        xfade_prev_l_ = prev_out_l_;
        xfade_prev_r_ = prev_out_r_;
        uint32_t sidx = ClampU(current_slice_, hdr.num_slices);
        xfade_total_   = AdaptiveXfade(hdr.slices[sidx].length);
        xfade_counter_ = xfade_total_;

        // Reset positions to start of current slice
        read_pos_    = 0.0;
        slice_timer_ = 0.0;
        gate_was_open_ = true;
        gate_fade_counter_ = 0;
    }

    // ── Parameters ─────────────────────────────────────────

    void SetPlayMode(PlaybackMode mode) { play_mode_ = mode; }

    void SetStartSlice(uint32_t idx)
    {
        if(idx != prev_start_)
            start_flash_timer_ = 90;
        prev_start_ = idx;
        start_slice_ = idx;
    }

    void SetEndSlice(uint32_t idx) { end_slice_ = idx; }

    void SetSpeed(float normalized)
    {
        normalized = Clampf(normalized, 0.0f, 1.0f);
        if(normalized >= 0.45f && normalized <= 0.55f)
        {
            speed_ratio_ = 1.0f;
            return;
        }
        float mapped;
        if(normalized < 0.45f)
            mapped = -2.0f * (1.0f - normalized / 0.45f);
        else
            mapped = 2.0f * (normalized - 0.55f) / 0.45f;
        speed_ratio_ = SafeRatio(powf(2.0f, mapped));
    }

    void SetPitch(float normalized)
    {
        normalized = Clampf(normalized, 0.0f, 1.0f);

        // Center to bipolar: -1.0 to +1.0
        float centered = (normalized - 0.5f) * 2.0f;

        // Dead zone at noon (4% each side = 8% total)
        if(centered > -0.08f && centered < 0.08f)
        {
            pitch_l_.SetFactor(1.0f);
            pitch_r_.SetFactor(1.0f);
            return;
        }

        float semitones;
        if(centered > 0.0f)
        {
            // CW: +1 to +12 semitones (linear)
            float t = (centered - 0.08f) / 0.92f;  // 0.0 to 1.0
            semitones = t * 12.0f;
        }
        else
        {
            // CCW: -1 to -24 semitones (linear)
            float t = (centered + 0.08f) / 0.92f;  // -1.0 to 0.0
            semitones = t * 24.0f;
        }

        // Quantize to nearest semitone
        semitones = roundf(semitones);

        // Clamp
        if(semitones > 12.0f)  semitones = 12.0f;
        if(semitones < -24.0f) semitones = -24.0f;

        float factor = powf(2.0f, semitones / 12.0f);
        if(factor != factor) factor = 1.0f;
        factor = Clampf(factor, 0.25f, 2.0f);
        pitch_l_.SetFactor(factor);
        pitch_r_.SetFactor(factor);
    }

    void SetSliceGate(float normalized)
    {
        slice_gate_ = Clampf(normalized, 0.01f, 1.0f);
    }

    void SetAutoAdvance(bool) {}
    void TriggerAuto() {}
    void TriggerChoke() {}
    void TriggerOpen() {}

    // ── Queries ────────────────────────────────────────────

    uint32_t GetStartSlice() const { return start_slice_; }
    uint32_t GetEndSlice() const { return end_slice_ + 1; }
    uint32_t GetNumSlices() const
    {
        return slices_ ? slices_->GetHeader().num_slices : 0;
    }
    bool IsPlaying() const { return is_playing_; }
    bool IsOneshotDone() const { return oneshot_done_; }
    void SetOneshotDone() { oneshot_done_ = true; }

    /** Return the current absolute read position within the slot buffer.
     *  Used by overdub to know where to write into the buffer. */
    uint32_t GetAbsoluteReadPos() const
    {
        if(!slices_ || !is_playing_) return 0;
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return 0;
        uint32_t sidx = ClampU(current_slice_, hdr.num_slices);
        uint32_t abs = hdr.slices[sidx].start_sample
                       + static_cast<uint32_t>(read_pos_);
        if(abs >= hdr.total_samples) abs = hdr.total_samples - 1;
        return abs;
    }

    bool StartSliceChanged() const { return start_flash_timer_ > 0; }
    void TickFlashTimer()
    {
        if(start_flash_timer_ > 0) start_flash_timer_--;
    }

    float GetPlaybackProgress() const
    {
        if(!is_playing_ || !slices_) return 0.0f;
        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0) return 0.0f;

        uint32_t s0 = ClampU(start_slice_, hdr.num_slices);
        uint32_t s1 = ClampU(end_slice_, hdr.num_slices);
        if(s1 < s0) s1 = s0;
        uint32_t total = s1 - s0 + 1;
        if(total == 0) return 0.0f;

        uint32_t cs = ClampU(current_slice_, hdr.num_slices);
        uint32_t done = (cs >= s0) ? (cs - s0) : 0;

        double win_len = static_cast<double>(hdr.slices[cs].length)
                         / static_cast<double>(speed_ratio_);
        float frac = (win_len > 0.0)
                     ? static_cast<float>(slice_timer_ / win_len) : 0.0f;
        frac = Clampf(frac, 0.0f, 1.0f);

        return Clampf((static_cast<float>(done) + frac)
                       / static_cast<float>(total), 0.0f, 1.0f);
    }

    // ── Audio ──────────────────────────────────────────────

    void Process(float& out_l, float& out_r)
    {
        out_l = 0.0f;
        out_r = 0.0f;

        if(!slices_ || !is_playing_ || oneshot_done_) return;

        const SlotHeader& hdr = slices_->GetHeader();
        if(hdr.num_slices == 0 || hdr.total_samples == 0) return;

        uint32_t s0 = ClampU(start_slice_, hdr.num_slices);
        uint32_t s1 = ClampU(end_slice_, hdr.num_slices);
        if(s1 < s0) s1 = s0;

        // Start knob changed: crossfade into new position
        if(current_slice_ < s0 || current_slice_ > s1)
        {
            xfade_prev_l_ = prev_out_l_;
            xfade_prev_r_ = prev_out_r_;
            current_slice_ = s0;
            uint32_t slen = hdr.slices[ClampU(s0, hdr.num_slices)].length;
            xfade_total_   = AdaptiveXfade(slen);
            xfade_counter_ = xfade_total_;
            read_pos_ = 0.0;
            slice_timer_ = 0.0;
        }

        uint32_t sidx = ClampU(current_slice_, hdr.num_slices);
        const SliceInfo& slice = hdr.slices[sidx];
        if(slice.length == 0) { AdvanceSlice(s0, s1); return; }

        double slice_len = static_cast<double>(slice.length);
        double window_len = slice_len / static_cast<double>(speed_ratio_);
        if(window_len < 1.0) window_len = 1.0;

        double win_frac = slice_timer_ / window_len;

        // ── Gate with state-tracked fade ────────────────────
        bool gate_open = (win_frac <= static_cast<double>(slice_gate_));

        if(gate_was_open_ && !gate_open)
            gate_fade_counter_ = GATE_FADE_SAMPLES;
        gate_was_open_ = gate_open;

        float gate_gain = 0.0f;
        if(gate_open)
        {
            gate_gain = 1.0f;
        }
        else if(gate_fade_counter_ > 0)
        {
            gate_gain = static_cast<float>(gate_fade_counter_)
                        / static_cast<float>(GATE_FADE_SAMPLES);
            gate_fade_counter_--;
        }

        float raw_l = 0.0f, raw_r = 0.0f;

        if(gate_gain > 0.0f && read_pos_ >= 0.0 && read_pos_ < slice_len)
        {
            const float* buf_l = slices_->GetSlotBufL()
                                 + slice.start_sample;
            const float* buf_r = slices_->GetSlotBufR()
                                 + slice.start_sample;
            float samp_l = HermiteRead(buf_l, slice.length, read_pos_);
            float samp_r = HermiteRead(buf_r, slice.length, read_pos_);

            // Granular pitch shift (lightweight, no FIFO dependency)
            raw_l = pitch_l_.Process(samp_l);
            raw_r = pitch_r_.Process(samp_r);

            // Fade near end of slice data (adaptive to slice length)
            uint32_t end_fade = AdaptiveXfade(static_cast<uint32_t>(slice_len));
            float remaining = static_cast<float>(slice_len - read_pos_);
            if(remaining < static_cast<float>(end_fade)
               && remaining > 0.0f)
            {
                float fade = remaining / static_cast<float>(end_fade);
                raw_l *= fade;
                raw_r *= fade;
            }

            raw_l *= gate_gain;
            raw_r *= gate_gain;
        }

        // Crossfade at slice/knob transitions
        if(xfade_counter_ > 0)
        {
            float t = static_cast<float>(xfade_counter_)
                      / static_cast<float>(xfade_total_);
            raw_l = raw_l * (1.0f - t) + xfade_prev_l_ * t;
            raw_r = raw_r * (1.0f - t) + xfade_prev_r_ * t;
            xfade_counter_--;
        }

        out_l = raw_l;
        out_r = raw_r;
        prev_out_l_ = raw_l;
        prev_out_r_ = raw_r;

        // Advance
        slice_timer_ += 1.0;
        read_pos_    += static_cast<double>(speed_ratio_);

        if(slice_timer_ >= window_len)
            AdvanceSlice(s0, s1);
    }

  private:
    void AdvanceSlice(uint32_t s0, uint32_t s1)
    {
        xfade_prev_l_ = prev_out_l_;
        xfade_prev_r_ = prev_out_r_;

        slice_timer_ = 0.0;
        read_pos_    = 0.0;

        current_slice_++;
        if(current_slice_ > s1)
        {
            if(play_mode_ == PlaybackMode::ONESHOT)
            {
                oneshot_done_ = true;
                return;
            }
            current_slice_ = s0;
        }

        // Adaptive crossfade based on the slice we're entering
        if(slices_)
        {
            const SlotHeader& hdr = slices_->GetHeader();
            uint32_t sidx = ClampU(current_slice_, hdr.num_slices);
            xfade_total_   = AdaptiveXfade(hdr.slices[sidx].length);
            xfade_counter_ = xfade_total_;
        }
        else
        {
            xfade_total_   = XFADE_MAX;
            xfade_counter_ = XFADE_MAX;
        }

        gate_was_open_ = true;
        gate_fade_counter_ = 0;
    }

    static float Clampf(float v, float lo, float hi)
    {
        if(v < lo) return lo;
        if(v > hi) return hi;
        return v;
    }

    static uint32_t ClampU(uint32_t val, uint32_t n)
    {
        return (val < n) ? val : n - 1;
    }

    static float SafeRatio(float r)
    {
        if(r != r) return 1.0f;
        if(r < 0.25f) return 0.25f;
        if(r > 4.0f) return 4.0f;
        return r;
    }

    /** Hermite 4-point interpolation for fractional buffer positions.
     *  Clamps at buffer boundaries (no wrapping). */
    static float HermiteRead(const float* buf, uint32_t len, double pos)
    {
        if(len < 4) return 0.0f;
        if(pos < 0.0) pos = 0.0;
        double dlen = static_cast<double>(len);
        if(pos >= dlen - 1.0) pos = dlen - 1.001;

        int   idx = static_cast<int>(pos);
        float f   = static_cast<float>(pos - static_cast<double>(idx));

        int last = static_cast<int>(len) - 1;
        int im1 = (idx > 0) ? idx - 1 : 0;
        int ip1 = (idx < last) ? idx + 1 : last;
        int ip2 = (idx < last - 1) ? idx + 2 : last;

        float xm1 = buf[im1];
        float x0  = buf[idx];
        float x1  = buf[ip1];
        float x2  = buf[ip2];

        float c0 = x0;
        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

        return ((c3 * f + c2) * f + c1) * f + c0;
    }

    SliceEngine* slices_       = nullptr;
    float        sample_rate_  = 96000.0f;

    uint32_t start_slice_   = 0;
    uint32_t end_slice_     = 15;
    uint32_t current_slice_ = 0;

    float    speed_ratio_   = 1.0f;
    float    slice_gate_    = 1.0f;
    bool     is_playing_    = false;

    double   read_pos_      = 0.0;
    double   slice_timer_   = 0.0;

    // Crossfade
    float    prev_out_l_    = 0.0f;
    float    prev_out_r_    = 0.0f;
    float    xfade_prev_l_  = 0.0f;
    float    xfade_prev_r_  = 0.0f;
    uint32_t xfade_counter_ = 0;
    uint32_t xfade_total_   = XFADE_MAX;

    /** Crossfade length adapts to slice size — max 12% of slice,
     *  capped at XFADE_MAX, minimum 16 for click-free transitions. */
    uint32_t AdaptiveXfade(uint32_t slice_length) const
    {
        uint32_t xf = slice_length / 8;
        if(xf > XFADE_MAX) xf = XFADE_MAX;
        if(xf < 16) xf = 16;
        return xf;
    }

    // Gate fade
    uint32_t gate_fade_counter_ = 0;
    bool     gate_was_open_     = true;

    // Play mode
    PlaybackMode play_mode_  = PlaybackMode::LOOP;
    bool     oneshot_done_   = false;

    // Flash
    uint32_t prev_start_         = 0;
    uint32_t start_flash_timer_  = 0;

    // WSOLA pitch shifter
    PitchShifter pitch_l_;
    PitchShifter pitch_r_;
};
