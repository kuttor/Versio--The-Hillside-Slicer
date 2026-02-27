#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * SliceEngine - Core buffer management for The Hillside Slicer.
 * 
 * Manages recording into SDRAM, dividing recordings into slices,
 * and maintaining 8 sample slots (4 persistent concept, 8 in SDRAM).
 * 
 * At 96kHz stereo float:
 *   1 second = 96000 samples * 2 channels * 4 bytes = 768KB
 *   16 beats @ 120BPM = 8 seconds = ~6.1MB
 *   Max single buffer = ~30MB (64 beats @ 80BPM)
 * 
 * We allocate 8 slots of MAX_SLOT_SAMPLES each.
 * MAX_SLOT_SAMPLES sized for 64 beats at ~60BPM = generous headroom.
 */

// Maximum samples per channel per slot
// 1M samples at 96kHz = ~10.4 seconds per slot.
// 8 slots * 1M * 2 channels * 4 bytes = 64MB = SDRAM capacity.
// At 120BPM: 16 beats=8sec, 32 beats=16sec (clamped), 64 beats=32sec (clamped)
// Users must use faster tempos for higher beat counts. This is fine.
static constexpr uint32_t MAX_SLOT_SAMPLES = 1000000;
static constexpr uint32_t NUM_SLOTS        = 8;
static constexpr uint32_t NUM_CHANNELS     = 2;
static constexpr uint32_t MAX_SLICES       = 64;

// Pre-roll buffer for threshold recording (100ms at 96kHz)
static constexpr uint32_t PREROLL_SAMPLES = 9600;

enum class RecordMode
{
    IMMEDIATE,  // SW_0 left: record starts on button press
    CLOCK_SYNC, // SW_0 middle: record starts on next clock tick
    THRESHOLD   // SW_0 right: record starts when input exceeds threshold
};

enum class RecordState
{
    EMPTY,     // No sample in this slot
    ARMED,     // Waiting for trigger (clock, threshold)
    RECORDING, // Actively recording
    PLAYING,   // Sample loaded and ready
    CLEARING   // Hold-to-clear in progress
};

struct SliceInfo
{
    uint32_t start_sample = 0; // Start position in the slot buffer
    uint32_t length       = 0; // Length in samples
};

struct SlotHeader
{
    uint32_t total_samples   = 0; // Total recorded samples per channel
    uint32_t num_slices      = 0; // 16, 32, or 64
    uint32_t clock_period    = 0; // Samples per beat at time of recording
    float    sample_rate     = 0; // SR at time of recording
    bool     has_content     = false;
    SliceInfo slices[MAX_SLICES];
};

class SliceEngine
{
  public:
    SliceEngine() {}

    /**
     * Initialize with pointers to SDRAM buffers.
     * Buffers must be allocated in SDRAM with DSY_SDRAM_BSS.
     * 
     * @param buf_l  Pointer to left channel SDRAM buffer [NUM_SLOTS * MAX_SLOT_SAMPLES]
     * @param buf_r  Pointer to right channel SDRAM buffer [NUM_SLOTS * MAX_SLOT_SAMPLES]
     * @param preroll_l  Pre-roll circular buffer left
     * @param preroll_r  Pre-roll circular buffer right
     * @param sr  Sample rate
     */
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

        // Zero everything
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
        target_beats_  = 16;
        threshold_     = 0.05f; // -26dB threshold for auto-trigger
    }

    // ── Recording ──────────────────────────────────────────────

    /** Always feed the pre-roll buffer (runs continuously) */
    void FeedPreroll(float left, float right)
    {
        preroll_l_[preroll_pos_] = left;
        preroll_r_[preroll_pos_] = right;
        preroll_pos_ = (preroll_pos_ + 1) % PREROLL_SAMPLES;
    }

    /** Arm recording on the current slot */
    void ArmRecording(RecordMode mode, uint32_t num_beats)
    {
        target_beats_  = num_beats;
        record_mode_   = mode;
        record_pos_    = 0;
        beat_count_    = 0;

        if(mode == RecordMode::IMMEDIATE)
        {
            // Start recording right now
            states_[current_slot_] = RecordState::RECORDING;
            headers_[current_slot_] = SlotHeader();
            headers_[current_slot_].num_slices   = num_beats;
            headers_[current_slot_].sample_rate   = sample_rate_;
        }
        else
        {
            // Armed - waiting for clock or threshold
            states_[current_slot_] = RecordState::ARMED;
            headers_[current_slot_] = SlotHeader();
            headers_[current_slot_].num_slices   = num_beats;
            headers_[current_slot_].sample_rate   = sample_rate_;
        }
    }

    /** Call when clock tick arrives during armed/recording state */
    void OnClockTick(uint32_t clock_period)
    {
        RecordState& st = states_[current_slot_];

        if(st == RecordState::ARMED && record_mode_ == RecordMode::CLOCK_SYNC)
        {
            // First clock after arming: start recording
            st = RecordState::RECORDING;
            headers_[current_slot_].clock_period = clock_period;
            record_pos_ = 0;
            beat_count_ = 0;

            // Copy pre-roll into buffer start (captures the transient)
            CopyPrerollToBuffer();
        }

        if(st == RecordState::RECORDING)
        {
            beat_count_++;
            headers_[current_slot_].clock_period = clock_period;

            if(beat_count_ >= target_beats_)
            {
                // Done recording - finalize
                FinalizeRecording();
            }
        }
    }

    /** Call when threshold is exceeded during armed state */
    void OnThresholdExceeded()
    {
        RecordState& st = states_[current_slot_];
        if(st == RecordState::ARMED && record_mode_ == RecordMode::THRESHOLD)
        {
            st = RecordState::RECORDING;
            record_pos_ = 0;
            beat_count_ = 0;

            // Copy pre-roll so we don't lose the attack
            CopyPrerollToBuffer();
        }
    }

    /** Check if input exceeds threshold (call from audio callback) */
    bool CheckThreshold(float left, float right) const
    {
        float level = std::max(fabsf(left), fabsf(right));
        return level > threshold_;
    }

    /** Record a sample into the current slot's buffer */
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
            // Buffer full - auto stop
            FinalizeRecording();
        }
    }

    /** Stop recording manually (immediate mode) */
    void StopRecording()
    {
        if(states_[current_slot_] == RecordState::RECORDING)
        {
            FinalizeRecording();
        }
    }

    /** Clear the current slot */
    void ClearSlot()
    {
        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        memset(&buf_l_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
        memset(&buf_r_[offset], 0, sizeof(float) * MAX_SLOT_SAMPLES);
        headers_[current_slot_] = SlotHeader();
        states_[current_slot_]  = RecordState::EMPTY;
    }

    // ── Playback ───────────────────────────────────────────────

    /** Read a sample from a specific position in the current slot.
     *  Position is in samples, not normalized. 
     *  Returns interpolated sample for sub-sample accuracy. */
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

        // Linear interpolation for sub-sample positioning
        uint32_t idx0 = static_cast<uint32_t>(position);
        uint32_t idx1 = idx0 + 1;
        float    frac = position - static_cast<float>(idx0);

        // Wrap indices
        idx0 = idx0 % total;
        idx1 = idx1 % total;

        out_l = buf_l_[offset + idx0] * (1.0f - frac) + buf_l_[offset + idx1] * frac;
        out_r = buf_r_[offset + idx0] * (1.0f - frac) + buf_r_[offset + idx1] * frac;
    }

    /** Get slice boundaries for a given slice index */
    const SliceInfo& GetSlice(uint32_t slice_idx) const
    {
        return headers_[current_slot_].slices[slice_idx % MAX_SLICES];
    }

    // ── Slot management ────────────────────────────────────────

    void SetCurrentSlot(uint32_t slot)
    {
        if(slot >= NUM_SLOTS)
            slot = NUM_SLOTS - 1;
        if(states_[current_slot_] == RecordState::RECORDING)
            FinalizeRecording(); // stop recording if switching away
        current_slot_ = slot;
    }

    uint32_t       GetCurrentSlot() const { return current_slot_; }
    RecordState    GetState() const { return states_[current_slot_]; }
    RecordState    GetSlotState(uint32_t slot) const { return states_[slot]; }
    const SlotHeader& GetHeader() const { return headers_[current_slot_]; }
    uint32_t       GetRecordPosition() const { return record_pos_; }
    float          GetRecordProgress() const
    {
        if(target_beats_ == 0 || headers_[current_slot_].clock_period == 0)
            return 0.0f;
        uint32_t total_expected = target_beats_ * headers_[current_slot_].clock_period;
        if(total_expected == 0) return 0.0f;
        return static_cast<float>(record_pos_) / static_cast<float>(total_expected);
    }

  private:
    void FinalizeRecording()
    {
        SlotHeader& hdr = headers_[current_slot_];
        hdr.total_samples = record_pos_;
        hdr.has_content   = (record_pos_ > 0);

        // Calculate slice boundaries - evenly divide the buffer
        uint32_t num = hdr.num_slices;
        if(num == 0) num = 16;
        if(num > MAX_SLICES) num = MAX_SLICES;

        uint32_t slice_len = hdr.total_samples / num;
        for(uint32_t i = 0; i < num; i++)
        {
            hdr.slices[i].start_sample = i * slice_len;
            hdr.slices[i].length       = slice_len;
        }
        // Last slice absorbs any remainder
        if(num > 0)
        {
            hdr.slices[num - 1].length = hdr.total_samples - hdr.slices[num - 1].start_sample;
        }

        states_[current_slot_] = RecordState::PLAYING;
    }

    void CopyPrerollToBuffer()
    {
        uint32_t offset = current_slot_ * MAX_SLOT_SAMPLES;
        // Copy pre-roll ring buffer into the start of the slot buffer
        // Start from the oldest sample in the ring
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
    float      threshold_   = 0.05f;

    SlotHeader   headers_[NUM_SLOTS];
    RecordState  states_[NUM_SLOTS];

    uint32_t   current_slot_   = 0;
    uint32_t   record_pos_     = 0;
    uint32_t   beat_count_     = 0;
    uint32_t   target_beats_   = 16;
    uint32_t   preroll_pos_    = 0;
    RecordMode record_mode_    = RecordMode::IMMEDIATE;
};
