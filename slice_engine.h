#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * SliceEngine - Core buffer management for The Hillside Slicer.
 *
 * Recording uses configurable settings:
 *   - Bars: 1, 2, 4, 8 (recording length)
 *   - Slices: 2, 4, 8, 16, 32, 64 (how material is divided)
 *   - BPM: 40-300 (internal clock speed)
 *
 * Clock ticks per bar = 16 (16th notes).
 * Total ticks = bars × 16.
 *
 * At 96kHz stereo float:
 *   1 second = 96000 samples * 2ch * 4 bytes = 768KB
 *   MAX_SLOT_SAMPLES = 900K = ~9.4 seconds per slot
 *   8 slots * 900K * 2ch * 4 bytes = ~54.9MB (fits 64MB SDRAM)
 */

static constexpr uint32_t MAX_SLOT_SAMPLES = 900000;
static constexpr uint32_t NUM_SLOTS        = 8;
static constexpr uint32_t NUM_CHANNELS     = 2;
static constexpr uint32_t MAX_SLICES       = 128;

// Pre-roll buffer for threshold recording (100ms at 96kHz)
static constexpr uint32_t PREROLL_SAMPLES = 9600;

// Ticks per bar (16th notes in 4/4)
static constexpr uint32_t TICKS_PER_BAR = 16;

enum class RecordMode
{
    IMMEDIATE,  // SW_0 left: record starts on TAP
    CLOCK_SYNC, // SW_0 center: record starts on next clock tick
    THRESHOLD   // SW_0 right: record starts on audio threshold
};

enum class RecordState
{
    EMPTY,
    ARMED,
    RECORDING,
    PLAYING,
    OVERDUBBING,
    STOPPED,
    CLEARING
};

struct SliceInfo
{
    uint32_t start_sample = 0;
    uint32_t length       = 0;
};

struct SlotHeader
{
    uint32_t total_samples   = 0;
    uint32_t num_slices      = 0;
    uint32_t clock_period    = 0;
    float    sample_rate     = 0;
    bool     has_content     = false;
    SliceInfo slices[MAX_SLICES];
};

/** Configurable recording settings (set from settings page) */
struct RecordSettings
{
    uint32_t bars   = 1;    // 1, 2, 4, 8
    uint32_t slices = 16;   // 2, 4, 8, 16, 32, 64
    float    bpm    = 120.0f; // 40-300
    float    overdub_feedback = 1.0f;  // 1.0 = pure overdub, 0.0 = replace

    /** Total clock ticks for a full recording */
    uint32_t TotalTicks() const { return bars * TICKS_PER_BAR; }

    /** Internal clock period in samples at given sample rate */
    uint32_t ClockPeriod(float sr) const
    {
        // period = samples_per_minute / (BPM * ticks_per_beat)
        // ticks_per_beat = 4 (16th notes)
        if(bpm < 1.0f) return 12000;
        return static_cast<uint32_t>(sr * 60.0f / (bpm * 4.0f));
    }
};

class SliceEngine
{
  public:
    SliceEngine() {}

    void Init(float* buf_l, float* buf_r,
              float* preroll_l, float* preroll_r,
              float sr)
    {
        buf_l_      = buf_l;
        buf_r_      = buf_r;
        preroll_l_  = preroll_l;
        preroll_r_  = preroll_r;
        sample_rate_ = sr;
        preroll_pos_ = 0;

        memset(buf_l_, 0, sizeof(float) * MAX_SLOT_SAMPLES * NUM_SLOTS);
        memset(buf_r_, 0, sizeof(float) * MAX_SLOT_SAMPLES * NUM_SLOTS);
        memset(preroll_l_, 0, sizeof(float) * PREROLL_SAMPLES);
        memset(preroll_r_, 0, sizeof(float) * PREROLL_SAMPLES);

        for(uint32_t i = 0; i < NUM_SLOTS; i++)
        {
            headers_[i]  = SlotHeader();
            states_[i]   = RecordState::EMPTY;
        }

        current_slot_  = 0;
        record_pos_    = 0;
        beat_count_    = 0;
        threshold_     = 0.015f;
    }

    // ── Settings ───────────────────────────────────────────

    void SetSettings(const RecordSettings& s) { settings_ = s; }
    const RecordSettings& GetSettings() const { return settings_; }

    /** Get the internal clock period based on current BPM setting */
    uint32_t GetInternalClockPeriod() const
    {
        return settings_.ClockPeriod(sample_rate_);
    }

    /** Re-slice an existing recording with a new slice count.
     *  Call this when changing slices in settings while a loop is playing.
     *  Preserves the audio, just recalculates boundaries. */
    void ResliceCurrentSlot()
    {
        SlotHeader& hdr = headers_[current_slot_];
        if(!hdr.has_content || hdr.total_samples == 0) return;

        uint32_t num = settings_.slices;
        if(num < 2) num = 2;
        if(num > MAX_SLICES) num = MAX_SLICES;
        hdr.num_slices = num;

        uint32_t slice_len = hdr.total_samples / num;
        for(uint32_t i = 0; i < num; i++)
        {
            hdr.slices[i].start_sample = i * slice_len;
            hdr.slices[i].length       = slice_len;
        }
        if(num > 0)
            hdr.slices[num - 1].length = hdr.total_samples
                                         - hdr.slices[num - 1].start_sample;
    }

    /** Extend or truncate the current slot to a new bar count.
     *  If bars increased (e.g. 1→2), the audio region is extended
     *  with silence (zeros). If bars decreased, total_samples shrinks.
     *  The buffer already exists at full MAX_SLOT_SAMPLES so extending
     *  just means writing zeros and updating the header.
     *  Always call ResliceCurrentSlot() after this. */
    void ExtendToBarCount(uint32_t new_bars, uint32_t old_bars)
    {
        SlotHeader& hdr = headers_[current_slot_];
        if(!hdr.has_content || hdr.total_samples == 0) return;
        if(new_bars == old_bars) return;

        // Calculate what the new total should be
        // Original recording was old_bars bars long.
        // Scale total_samples proportionally.
        uint32_t samples_per_bar = hdr.total_samples / old_bars;
        uint32_t new_total = samples_per_bar * new_bars;

        // Clamp to buffer size
        uint32_t slot_offset = current_slot_ * MAX_SLOT_SAMPLES;
        if(new_total > MAX_SLOT_SAMPLES)
            new_total = MAX_SLOT_SAMPLES;

        // If extending, zero-fill the new region
        if(new_total > hdr.total_samples)
        {
            uint32_t start = slot_offset + hdr.total_samples;
            uint32_t count = new_total - hdr.total_samples;
            memset(&buf_l_[start], 0, count * sizeof(float));
            memset(&buf_r_[start], 0, count * sizeof(float));
        }

        hdr.total_samples = new_total;

        // Re-slice with current settings
        ResliceCurrentSlot();
    }

    /** Merge (overdub) source slot into destination slot.
     *  Sums audio sample-by-sample. If source is longer than dest,
     *  extends dest to source length (copies tail). Capped at MAX_SLOT_SAMPLES.
     *  Then reslices the destination slot.
     *  Returns true on success. */
    bool MergeSlots(uint32_t dest_slot, uint32_t src_slot)
    {
        if(dest_slot >= NUM_SLOTS || src_slot >= NUM_SLOTS) return false;
        if(dest_slot == src_slot) return false;

        SlotHeader& dst_hdr = headers_[dest_slot];
        const SlotHeader& src_hdr = headers_[src_slot];

        // Source must have content
        if(!src_hdr.has_content || src_hdr.total_samples == 0) return false;

        uint32_t dst_off = dest_slot * MAX_SLOT_SAMPLES;
        uint32_t src_off = src_slot  * MAX_SLOT_SAMPLES;

        uint32_t dst_len = dst_hdr.has_content ? dst_hdr.total_samples : 0;
        uint32_t src_len = src_hdr.total_samples;

        // Overlap region: sum source into destination
        uint32_t overlap = (dst_len < src_len) ? dst_len : src_len;
        for(uint32_t i = 0; i < overlap; i++)
        {
            buf_l_[dst_off + i] += buf_l_[src_off + i];
            buf_r_[dst_off + i] += buf_r_[src_off + i];
        }

        // If source is longer, copy the tail (no existing audio there)
        if(src_len > dst_len)
        {
            uint32_t new_total = src_len;
            if(new_total > MAX_SLOT_SAMPLES) new_total = MAX_SLOT_SAMPLES;

            uint32_t tail_start = overlap;
            uint32_t tail_count = new_total - tail_start;

            memcpy(&buf_l_[dst_off + tail_start],
                   &buf_l_[src_off + tail_start],
                   tail_count * sizeof(float));
            memcpy(&buf_r_[dst_off + tail_start],
                   &buf_r_[src_off + tail_start],
                   tail_count * sizeof(float));

            dst_hdr.total_samples = new_total;
        }

        // Mark destination as having content
        dst_hdr.has_content  = true;
        dst_hdr.sample_rate  = sample_rate_;

        // Reslice destination
        uint32_t saved_slot = current_slot_;
        current_slot_ = dest_slot;
        ResliceCurrentSlot();
        current_slot_ = saved_slot;

        states_[dest_slot] = RecordState::PLAYING;
        return true;
    }

    // ── Recording ──────────────────────────────────────────

    void FeedPreroll(float left, float right)
    {
        preroll_l_[preroll_pos_] = left;
        preroll_r_[preroll_pos_] = right;
        preroll_pos_ = (preroll_pos_ + 1) % PREROLL_SAMPLES;
    }

    void ArmRecording(RecordMode mode)
    {
        record_mode_   = mode;
        record_pos_    = 0;
        beat_count_    = 0;

        if(mode == RecordMode::IMMEDIATE)
        {
            states_[current_slot_] = RecordState::RECORDING;
            headers_[current_slot_] = SlotHeader();
            headers_[current_slot_].sample_rate = sample_rate_;
        }
        else
        {
            states_[current_slot_] = RecordState::ARMED;
            headers_[current_slot_] = SlotHeader();
            headers_[current_slot_].sample_rate = sample_rate_;
        }
    }

    void OnClockTick(uint32_t clock_period)
    {
        RecordState& st = states_[current_slot_];

        if(st == RecordState::ARMED && record_mode_ == RecordMode::CLOCK_SYNC)
        {
            st = RecordState::RECORDING;
            headers_[current_slot_].clock_period = clock_period;
            record_pos_ = 0;
            beat_count_ = 0;
            CopyPrerollToBuffer();
        }

        if(st == RecordState::RECORDING)
        {
            beat_count_++;
            headers_[current_slot_].clock_period = clock_period;

            // Auto-stop after configured number of ticks
            uint32_t total_ticks = settings_.TotalTicks();
            if(beat_count_ >= total_ticks)
            {
                StopRecording();
            }
        }
    }

    void OnThresholdExceeded()
    {
        RecordState& st = states_[current_slot_];
        if(st == RecordState::ARMED && record_mode_ == RecordMode::THRESHOLD)
        {
            st = RecordState::RECORDING;
            record_pos_ = 0;
            beat_count_ = 0;
            CopyPrerollToBuffer();
        }
    }

    bool CheckThreshold(float left, float right) const
    {
        float level = std::max(fabsf(left), fabsf(right));
        return level > threshold_;
    }

    void RecordSample(float left, float right)
    {
        if(states_[current_slot_] != RecordState::RECORDING)
            return;

        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        if(record_pos_ < MAX_SLOT_SAMPLES)
        {
            buf_l_[offset + record_pos_] = left;
            buf_r_[offset + record_pos_] = right;
            record_pos_++;
        }
        else
        {
            FinalizeRecording();
        }
    }

    void StopRecording()
    {
        if(states_[current_slot_] == RecordState::RECORDING)
            FinalizeRecording();
    }

    // ── Overdub ────────────────────────────────────────────

    /** Start overdubbing into the current slot.
     *  Playback must be active (PLAYING state).
     *  Overdub writes at the playback position using feedback mix. */
    void StartOverdub()
    {
        if(states_[current_slot_] != RecordState::PLAYING) return;
        if(!headers_[current_slot_].has_content) return;
        states_[current_slot_] = RecordState::OVERDUBBING;
    }

    /** Stop overdubbing, return to normal playback. */
    void StopOverdub()
    {
        if(states_[current_slot_] == RecordState::OVERDUBBING)
            states_[current_slot_] = RecordState::PLAYING;
    }

    /** Write one sample during overdub at an absolute buffer position.
     *  feedback: 1.0 = pure sum (overdub), 0.0 = full replace.
     *  The abs_pos is relative to slot start (0-based within slot). */
    void OverdubSample(uint32_t abs_pos, float left, float right,
                       float feedback)
    {
        if(states_[current_slot_] != RecordState::OVERDUBBING) return;

        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        uint32_t total  = headers_[current_slot_].total_samples;
        if(abs_pos >= total) return;

        uint32_t idx = offset + abs_pos;
        buf_l_[idx] = buf_l_[idx] * feedback + left;
        buf_r_[idx] = buf_r_[idx] * feedback + right;
    }

    void ClearSlot()
    {
        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        memset(&buf_l_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
        memset(&buf_r_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
        headers_[current_slot_] = SlotHeader();
        states_[current_slot_]  = RecordState::EMPTY;
    }

    /** Clear ALL slots, reset everything. Full module wipe. */
    void ClearAllSlots()
    {
        for(uint32_t s = 0; s < NUM_SLOTS; s++)
        {
            uint32_t offset = s * MAX_SLOT_SAMPLES;
            memset(&buf_l_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
            memset(&buf_r_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
            headers_[s] = SlotHeader();
            states_[s]  = RecordState::EMPTY;
        }
        current_slot_ = 0;
        record_pos_   = 0;
        beat_count_   = 0;
    }

    // ── Playback ───────────────────────────────────────────

    void ReadSample(float position, float& out_l, float& out_r) const
    {
        const SlotHeader& hdr = headers_[current_slot_];
        if(!hdr.has_content || hdr.total_samples == 0)
        {
            out_l = 0.0f;
            out_r = 0.0f;
            return;
        }

        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        uint32_t total  = hdr.total_samples;

        uint32_t idx0 = static_cast<uint32_t>(position);
        uint32_t idx1 = idx0 + 1;
        float    frac = position - static_cast<float>(idx0);

        if(idx0 >= total) idx0 = total - 1;
        if(idx1 >= total) idx1 = total - 1;

        out_l = buf_l_[offset + idx0] * (1.0f - frac) + buf_l_[offset + idx1] * frac;
        out_r = buf_r_[offset + idx0] * (1.0f - frac) + buf_r_[offset + idx1] * frac;
    }

    void ReadSampleClamped(float position, uint32_t total,
                           float& out_l, float& out_r) const
    {
        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;

        if(total == 0 || position < 0.0f)
        {
            out_l = 0.0f;
            out_r = 0.0f;
            return;
        }

        uint32_t idx0 = static_cast<uint32_t>(position);
        float    frac = position - static_cast<float>(idx0);

        if(idx0 >= total) { idx0 = total - 1; frac = 0.0f; }
        uint32_t idx1 = idx0 + 1;
        if(idx1 >= total) idx1 = idx0;

        out_l = buf_l_[offset + idx0] * (1.0f - frac) + buf_l_[offset + idx1] * frac;
        out_r = buf_r_[offset + idx0] * (1.0f - frac) + buf_r_[offset + idx1] * frac;
    }

    const SliceInfo& GetSlice(uint32_t slice_idx) const
    {
        return headers_[current_slot_].slices[slice_idx % MAX_SLICES];
    }

    void SetStopped()
    {
        if(states_[current_slot_] == RecordState::PLAYING)
            states_[current_slot_] = RecordState::STOPPED;
    }

    void Resume()
    {
        if(states_[current_slot_] == RecordState::STOPPED)
            states_[current_slot_] = RecordState::PLAYING;
    }

    // ── Slot management ────────────────────────────────────

    void SetCurrentSlot(uint32_t slot)
    {
        if(slot >= NUM_SLOTS) slot = NUM_SLOTS - 1;
        if(states_[current_slot_] == RecordState::RECORDING)
            FinalizeRecording();
        current_slot_ = slot;
    }

    uint32_t       GetCurrentSlot() const { return current_slot_; }
    RecordState    GetState() const { return states_[current_slot_]; }
    RecordState    GetSlotState(uint32_t slot) const { return states_[slot]; }
    const SlotHeader& GetHeader() const { return headers_[current_slot_]; }
    const SlotHeader& GetSlot0Header() const { return headers_[0]; }

    /** Direct buffer access for pitch shifter (reads from SDRAM) */
    const float* GetSlotBufL() const
    {
        return buf_l_ + current_slot_ * MAX_SLOT_SAMPLES;
    }
    const float* GetSlotBufR() const
    {
        return buf_r_ + current_slot_ * MAX_SLOT_SAMPLES;
    }

    /** Restore slot 0 from persisted data (called at boot).
     *  Audio data is already in the SDRAM buffers by the time this runs.
     *  This just sets up the header and state so playback works. */
    void RestoreSlot0(uint32_t total_samples, uint32_t num_slices,
                      float sample_rate)
    {
        SlotHeader& hdr = headers_[0];
        hdr.total_samples = total_samples;
        hdr.sample_rate   = sample_rate;
        hdr.has_content   = true;
        hdr.num_slices    = num_slices;

        // Rebuild slice boundaries
        if(num_slices > 0 && num_slices <= MAX_SLICES)
        {
            uint32_t slice_len = total_samples / num_slices;
            for(uint32_t i = 0; i < num_slices; i++)
            {
                hdr.slices[i].start_sample = i * slice_len;
                hdr.slices[i].length       = slice_len;
            }
            hdr.slices[num_slices - 1].length =
                total_samples - hdr.slices[num_slices - 1].start_sample;
        }

        states_[0] = RecordState::PLAYING;
    }
    uint32_t       GetRecordPosition() const { return record_pos_; }

    float GetRecordProgress() const
    {
        if(states_[current_slot_] != RecordState::RECORDING)
            return 0.0f;

        uint32_t total_ticks = settings_.TotalTicks();

        if(beat_count_ > 0)
            return static_cast<float>(beat_count_)
                   / static_cast<float>(total_ticks);

        // Before first tick, estimate from clock period
        uint32_t period = headers_[current_slot_].clock_period;
        if(period == 0) period = settings_.ClockPeriod(sample_rate_);
        uint32_t expected_total = total_ticks * period;
        if(expected_total == 0) return 0.0f;

        return static_cast<float>(record_pos_)
               / static_cast<float>(expected_total);
    }

  private:
    void FinalizeRecording()
    {
        SlotHeader& hdr = headers_[current_slot_];
        hdr.total_samples = record_pos_;
        hdr.has_content   = (record_pos_ > 0);

        // Use configured slice count
        uint32_t num = settings_.slices;
        if(num < 2) num = 2;
        if(num > MAX_SLICES) num = MAX_SLICES;
        hdr.num_slices = num;

        if(beat_count_ > 0 && hdr.clock_period == 0)
            hdr.clock_period = record_pos_ / beat_count_;

        uint32_t slice_len = hdr.total_samples / num;
        for(uint32_t i = 0; i < num; i++)
        {
            hdr.slices[i].start_sample = i * slice_len;
            hdr.slices[i].length       = slice_len;
        }
        // Last slice absorbs remainder
        if(num > 0)
            hdr.slices[num - 1].length = hdr.total_samples
                                         - hdr.slices[num - 1].start_sample;

        states_[current_slot_] = RecordState::PLAYING;
    }

    void CopyPrerollToBuffer()
    {
        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        for(uint32_t i = 0; i < PREROLL_SAMPLES; i++)
        {
            uint32_t src = (preroll_pos_ + i) % PREROLL_SAMPLES;
            buf_l_[offset + i] = preroll_l_[src];
            buf_r_[offset + i] = preroll_r_[src];
        }
        record_pos_ = PREROLL_SAMPLES;
    }

    float*     buf_l_       = nullptr;
    float*     buf_r_       = nullptr;
    float*     preroll_l_   = nullptr;
    float*     preroll_r_   = nullptr;
    float      sample_rate_ = 96000.0f;
    float      threshold_   = 0.015f;

    SlotHeader     headers_[NUM_SLOTS];
    RecordState    states_[NUM_SLOTS];
    RecordSettings settings_;

    uint32_t   current_slot_   = 0;
    uint32_t   record_pos_     = 0;
    uint32_t   beat_count_     = 0;
    uint32_t   preroll_pos_    = 0;
    RecordMode record_mode_    = RecordMode::IMMEDIATE;
};
