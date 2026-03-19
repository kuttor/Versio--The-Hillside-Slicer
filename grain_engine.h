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
        bpm_ratio_      = 1.0f;
        current_window_len_ = 0.0;
        is_playing_     = false;
        read_pos_       = 0.0;
        slice_timer_    = 0.0;
        fade_counter_   = 0;
        fade_abs_pos_   = 0.0;
        current_slice_  = 0;
        prev_out_l_     = 0.0f;
        prev_out_r_     = 0.0f;
        xfade_counter_  = 0;
        xfade_prev_l_   = 0.0f;
        xfade_prev_r_   = 0.0f;
        prev_start_     = 0;
        start_flash_timer_ = 0;
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
        fade_counter_ = 0;
        is_playing_  = true;
        oneshot_done_ = false;
        prev_out_l_  = 0.0f;
        prev_out_r_  = 0.0f;
        xfade_counter_ = XFADE_MAX;
        xfade_total_   = XFADE_MAX;
        xfade_prev_l_ = 0.0f;
        xfade_prev_r_ = 0.0f;

        // Reset pitch shifter state for fresh audio content
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
            mapped = -1.0f * (1.0f - normalized / 0.45f);
        else
            mapped = 1.0f * (normalized - 0.55f) / 0.45f;
        speed_ratio_ = SafeRatio(powf(2.0f, mapped));
    }

    void SetPitch(float normalized)
    {
        normalized = Clampf(normalized, 0.0f, 1.0f);

        // Center to bipolar: -1.0 to +1.0
        float centered = (normalized - 0.5f) * 2.0f;

        // Dead zone at noon (6% each side = 12% total)
        if(centered > -0.12f && centered < 0.12f)
        {
            pitch_l_.SetFactor(1.0f);
            pitch_r_.SetFactor(1.0f);
            return;
        }

        // Symmetric: +/-12 semitones (1 octave each way)
        float semitones;
        if(centered > 0.0f)
        {
            float t = (centered - 0.12f) / 0.88f;
            semitones = t * 12.0f;
        }
        else
        {
            float t = (centered + 0.12f) / 0.88f;
            semitones = t * 12.0f;
        }

        // Quantize to nearest semitone
        semitones = roundf(semitones);
        if(semitones > 12.0f)  semitones = 12.0f;
        if(semitones < -12.0f) semitones = -12.0f;

        float factor = powf(2.0f, semitones / 12.0f);
        if(factor != factor) factor = 1.0f;
        factor = Clampf(factor, 0.5f, 2.0f);
        pitch_l_.SetFactor(factor);
        pitch_r_.SetFactor(factor);
    }

    /** Multiply the current speed by a BPM ratio.
     *  Call AFTER SetSpeed. 1.0 = no change. */
    void SetBpmRatio(float ratio)
    {
        if(ratio < 0.25f) ratio = 0.25f;
        if(ratio > 4.0f)  ratio = 4.0f;
        bpm_ratio_ = ratio;
    }

    void SetAutoAdvance(bool) {}
    void TriggerAuto() {}
    void TriggerChoke() {}
    void TriggerOpen() {}

    /** Set swing amount. 0.0 = straight, 1.0 = heavy triplet swing.
     *  8th-note swing: pairs of slices form 8th notes.
     *  Even pairs play longer, odd pairs play shorter. */
    void SetSwing(float normalized)
    {
        if(normalized < 0.0f) normalized = 0.0f;
        if(normalized > 1.0f) normalized = 1.0f;
        // Max 0.65: very audible groove, safe at all speed settings.
        swing_ = normalized * 0.65f;
    }

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

        double win_len = current_window_len_;
        if(win_len <= 0.0)
            win_len = static_cast<double>(hdr.slices[cs].length)
                      / static_cast<double>(speed_ratio_ * bpm_ratio_);
        float frac = (win_len > 0.0)
                     ? static_cast<float>(slice_timer_ / win_len) : 0.0f;
        frac = Clampf(frac, 0.0f, 1.0f);

        return Clampf((static_cast<float>(done) + frac)
                       / static_cast<float>(total), 0.0f, 1.0f);
    }

    uint32_t GetCurrentSlice() const { return current_slice_; }

    /** Progress within the current slice: 0.0 = start, 1.0 = end */
    float GetSliceProgress() const
    {
        if(!is_playing_ || !slices_) return 0.0f;
        const SlotHeader& hdr = slices_->GetHeader();
        uint32_t sidx = ClampU(current_slice_, hdr.num_slices);
        double win_len = current_window_len_;
        if(win_len <= 0.0)
            win_len = static_cast<double>(hdr.slices[sidx].length)
                      / static_cast<double>(speed_ratio_ * bpm_ratio_);
        if(win_len <= 0.0) return 0.0f;
        return Clampf(static_cast<float>(slice_timer_ / win_len), 0.0f, 1.0f);
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

        // Start knob changed: buffer-fade into new position
        if(current_slice_ < s0 || current_slice_ > s1)
        {
            // Start fade from current absolute position
            uint32_t ci = ClampU(current_slice_, hdr.num_slices);
            fade_abs_pos_ = static_cast<double>(hdr.slices[ci].start_sample)
                            + read_pos_;
            fade_total_ = FADE_LEN;
            fade_counter_ = FADE_LEN;
            current_slice_ = s0;
            read_pos_ = 0.0;
            slice_timer_ = 0.0;
        }

        uint32_t sidx = ClampU(current_slice_, hdr.num_slices);

        const SliceInfo& slice = hdr.slices[sidx];
        if(slice.length == 0) { AdvanceSlice(s0, s1, XFADE_MAX); return; }

        double slice_len = static_cast<double>(slice.length);
        double window_len = slice_len
                            / static_cast<double>(speed_ratio_ * bpm_ratio_);
        if(window_len < 1.0) window_len = 1.0;

        // Even groups: ratio > 1.0 = stretch (plays slower, takes longer)
        // Odd groups: ratio < 1.0 = compress (plays faster, takes shorter)
        // Window timing controls WHEN slices advance.
        double swing_window = window_len;

        if(swing_ > 0.001f)
        {
            uint32_t group = current_slice_ / 2;
            bool even_group = !(group & 1);
            if(even_group)
            {
                swing_window = window_len * static_cast<double>(1.0f + swing_);
            }
            else
            {
                swing_window = window_len * static_cast<double>(1.0f - swing_);
            }
            if(swing_window < 1024.0) swing_window = 1024.0;
        }


        current_window_len_ = swing_window;
        last_adj_start_ = slice.start_sample;

        // Read audio at constant speed — swing only changes WHEN slices advance
        const float* buf_l = slices_->GetSlotBufL() + slice.start_sample;
        const float* buf_r = slices_->GetSlotBufR() + slice.start_sample;

        double clamped = read_pos_;
        if(clamped < 0.0) clamped = 0.0;

        float samp_l, samp_r;
        if(read_pos_ >= slice_len)
        {
            // Past content end (even swing groups): output silence
            samp_l = 0.0f;
            samp_r = 0.0f;
        }
        else
        {
            samp_l = HermiteRead(buf_l, slice.length, clamped);
            samp_r = HermiteRead(buf_r, slice.length, clamped);
        }

        // Buffer-level crossfade for non-contiguous transitions
        if(fade_counter_ > 0)
        {
            const float* slot_l = slices_->GetSlotBufL();
            const float* slot_r = slices_->GetSlotBufR();
            uint32_t total = hdr.total_samples;
            if(fade_abs_pos_ >= 0.0
               && fade_abs_pos_ < static_cast<double>(total))
            {
                float f_l = HermiteRead(slot_l, total, fade_abs_pos_);
                float f_r = HermiteRead(slot_r, total, fade_abs_pos_);
                float t = static_cast<float>(fade_counter_)
                          / static_cast<float>(fade_total_);
                samp_l = samp_l * (1.0f - t) + f_l * t;
                samp_r = samp_r * (1.0f - t) + f_r * t;
            }
            fade_abs_pos_ += static_cast<double>(speed_ratio_ * bpm_ratio_);
            fade_counter_--;
        }

        // Pitch shift: bypass at unity handled inside PitchShifter
        // with delay-matched raw path (no click on transitions).
        float raw_l = pitch_l_.Process(samp_l);
        float raw_r = pitch_r_.Process(samp_r);

        // Crossfade only at non-contiguous transitions
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

        // Advance at constant speed, swing_window controls WHEN next slice fires
        slice_timer_ += 1.0;
        read_pos_    += static_cast<double>(speed_ratio_ * bpm_ratio_);

        if(slice_timer_ >= swing_window)
            AdvanceSlice(s0, s1, swing_window);
    }

  private:
    void AdvanceSlice(uint32_t s0, uint32_t s1, double win_len)
    {
        uint32_t prev_slice = current_slice_;

        // Compute absolute position BEFORE reset (for fade head)
        double old_abs = static_cast<double>(last_adj_start_) + read_pos_;

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

        // Buffer fade + phase reset for non-contiguous transitions
        // When swing is active, always crossfade — even groups may have
        // held their last sample, creating a micro-discontinuity.
        bool contiguous = false;
        if(prev_slice != current_slice_ && slices_)
        {
            const SlotHeader& hdr = slices_->GetHeader();
            uint32_t pi = ClampU(prev_slice, hdr.num_slices);
            uint32_t ni = ClampU(current_slice_, hdr.num_slices);
            contiguous = (hdr.slices[pi].start_sample + hdr.slices[pi].length
                          == hdr.slices[ni].start_sample);
        }

        bool needs_fade = !contiguous || (swing_ > 0.001f);

        if(needs_fade)
        {
            StartFade(old_abs, win_len);
            if(fabsf(pitch_l_.GetFactor() - 1.0f) >= 0.01f)
            {
                pitch_l_.ResetForDiscontinuity();
                pitch_r_.ResetForDiscontinuity();
            }
        }
    }

    /** Start a buffer-level crossfade from old_abs position.
     *  Caps length to 25% of window so fast roll isn't all-fade. */
    void StartFade(double old_abs, double win_len)
    {
        fade_abs_pos_ = old_abs;
        uint32_t len = FADE_LEN;
        // Cap to 25% of window for fast roll
        uint32_t max_len = static_cast<uint32_t>(win_len * 0.25);
        if(max_len < 64) max_len = 64;
        if(len > max_len) len = max_len;
        fade_total_ = len;
        fade_counter_ = len;
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
    float    bpm_ratio_     = 1.0f;
    float    swing_         = 0.0f;
    double   current_window_len_ = 0.0;  // Actual swung window for progress
    uint32_t last_adj_start_ = 0;
    bool     is_playing_    = false;

    double   read_pos_      = 0.0;
    double   slice_timer_   = 0.0;

    // Buffer-level crossfade (softcut-style dual read head).
    // When read_pos_ jumps, the fade head continues from the old
    // absolute buffer position and fades out over FADE_LEN samples.
    // The blended stream feeds the pitch shifter — no discontinuity.
    static constexpr uint32_t FADE_LEN = 1024;  // = 1 FFT window
    double   fade_abs_pos_  = 0.0;   // Absolute position in slot buffer
    uint32_t fade_counter_  = 0;     // Samples remaining (0 = inactive)
    uint32_t fade_total_    = FADE_LEN;
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

    // Play mode
    PlaybackMode play_mode_  = PlaybackMode::LOOP;
    bool     oneshot_done_   = false;

    // Flash
    uint32_t prev_start_         = 0;
    uint32_t start_flash_timer_  = 0;

    // Pitch shifter pair
    PitchShifter pitch_l_;
    PitchShifter pitch_r_;
};
