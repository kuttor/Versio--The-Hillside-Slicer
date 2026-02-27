/**
 * ╔══════════════════════════════════════════════════════════════╗
 * ║  THE HILLSIDE SLICER - Custom Versio Firmware               ║
 * ║  A real-time sample slicer with decoupled pitch & time.     ║
 * ║  Granular timestretch, auto-slicing, choke/open triggers.   ║
 * ║                                                             ║
 * ║  96kHz / 32-bit float / Stereo                              ║
 * ║                                                             ║
 * ║  Controls:                                                  ║
 * ║    KNOB_0 (Blend) : Start slice position                    ║
 * ║    KNOB_0 CV jack : CHOKE trigger input                     ║
 * ║    KNOB_1 (Tone)  : Play length (number of slices)          ║
 * ║    KNOB_1 CV jack : OPEN trigger input                      ║
 * ║    KNOB_2 (Speed) : Timestretch speed (/4 to x4)            ║
 * ║    KNOB_3 (Index) : Pitch shift (-12 to +12 semitones)      ║
 * ║    KNOB_4 (Regen) : Grain overlap (2x-8x)                   ║
 * ║    KNOB_5 (Size)  : Lo-fi (pristine → SP-1200 → 8-bit)     ║
 * ║    KNOB_6 (Dense) : Slot select (1-8)                       ║
 * ║    SW_0           : Rec mode: Immediate/ClockSync/Threshold ║
 * ║    SW_1           : Slice count: 16 / 32 / 64               ║
 * ║    TAP            : Record / Stop / Hold=Clear              ║
 * ║    Gate In        : Master clock                             ║
 * ║    LEDs 0-3       : State display                            ║
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include "daisy_versio.h"
#include "daisysp.h"

#include "gate_detect.h"
#include "slice_engine.h"
#include "grain_engine.h"
#include "lofi.h"
#include "led_manager.h"

using namespace daisy;
using namespace daisysp;

// ─── Hardware ──────────────────────────────────────────────────
DaisyVersio hw;

// ─── SDRAM Buffers ─────────────────────────────────────────────
// These MUST be global and use DSY_SDRAM_BSS.
// Total: 8 slots * 6.2M samples * 4 bytes = ~198MB... that's too much.
// We need to be smarter. Let's use 8 slots with smaller max size.
// At 96kHz, 16 beats @ 120BPM = ~384K samples per channel.
// 64 beats @ 60BPM worst case = ~6.1M samples per channel.
// With 64MB SDRAM, we can fit about 8M float samples total per channel.
// So: 8 slots * 1M samples each = 8M per channel = 64MB total. Perfect.

// SDRAM buffer sizing:
// MAX_SLOT_SAMPLES (1M) defined in slice_engine.h
// 8 slots * 1M samples * 4 bytes * 2 channels = 64MB = full SDRAM
// Each slot holds ~10.4 seconds at 96kHz - plenty for performance use.

static constexpr uint32_t TOTAL_BUF_SIZE = MAX_SLOT_SAMPLES * NUM_SLOTS;

// SDRAM buffers - stereo
static float DSY_SDRAM_BSS sdram_buf_l[TOTAL_BUF_SIZE];
static float DSY_SDRAM_BSS sdram_buf_r[TOTAL_BUF_SIZE];

// Pre-roll buffers (small, can live in SDRAM too)
static float DSY_SDRAM_BSS preroll_l[PREROLL_SAMPLES];
static float DSY_SDRAM_BSS preroll_r[PREROLL_SAMPLES];

// ─── DSP Modules ───────────────────────────────────────────────
SliceEngine     sliceEngine;
PlaybackEngine  playbackEngine;
LofiProcessor   lofi;
LedManager      ledManager;

// ─── Gate Detectors ────────────────────────────────────────────
GateDetector  clockGate;       // FSU/Gate input - master clock
GateDetector  chokeGate;       // KNOB_0 CV jack - choke trigger
GateDetector  openGate;        // KNOB_1 CV jack - open trigger
ClockTracker  clockTracker;

// ─── State ─────────────────────────────────────────────────────
static bool     tap_held          = false;
static uint32_t tap_hold_counter  = 0;
static uint32_t prev_slot         = 0;
static bool     choke_patched     = false;
static bool     open_patched      = false;
static uint32_t no_activity_choke = 0;
static uint32_t no_activity_open  = 0;

static constexpr uint32_t HOLD_CLEAR_THRESHOLD = 192000; // 2 sec at 96kHz
static constexpr uint32_t PATCH_DETECT_TIMEOUT = 288000; // 3 sec at 96kHz

// ─── Helpers ───────────────────────────────────────────────────

/** Map SW_0 3-way switch to RecordMode */
RecordMode GetRecordMode()
{
    // Switch3 returns: 0, 1, or 2 for position
    // We read the switch position from the Versio's sw array
    switch(hw.sw[DaisyVersio::SW_0].Read())
    {
        case Switch3::POS_UP:   return RecordMode::IMMEDIATE;
        case Switch3::POS_CENTER: return RecordMode::CLOCK_SYNC;
        case Switch3::POS_DOWN: return RecordMode::THRESHOLD;
        default: return RecordMode::IMMEDIATE;
    }
}

/** Map SW_1 3-way switch to slice count */
uint32_t GetSliceCount()
{
    switch(hw.sw[DaisyVersio::SW_1].Read())
    {
        case Switch3::POS_UP:     return 16;
        case Switch3::POS_CENTER: return 32;
        case Switch3::POS_DOWN:   return 64;
        default: return 16;
    }
}

/** Map KNOB_6 to slot index (0-7) with dead zones */
uint32_t GetSlotFromKnob(float value)
{
    // 8 slots across 0.0 - 1.0 with dead zones between
    // Each slot occupies ~0.11 of the range, with ~0.015 dead zone
    if(value < 0.110f) return 0;
    if(value < 0.235f) return 1;
    if(value < 0.360f) return 2;
    if(value < 0.485f) return 3;
    if(value < 0.610f) return 4;
    if(value < 0.735f) return 5;
    if(value < 0.860f) return 6;
    return 7;
}

/** Detect if a CV jack has something patched (activity detection) */
void UpdatePatchDetection(float choke_cv, float open_cv)
{
    // If CV is near zero for a long time, assume unpatched
    if(fabsf(choke_cv) > 0.05f)
        no_activity_choke = 0;
    else
        no_activity_choke++;

    if(fabsf(open_cv) > 0.05f)
        no_activity_open = 0;
    else
        no_activity_open++;

    choke_patched = (no_activity_choke < PATCH_DETECT_TIMEOUT);
    open_patched  = (no_activity_open < PATCH_DETECT_TIMEOUT);
}

// ═══════════════════════════════════════════════════════════════
//  AUDIO CALLBACK - runs at 96kHz, block size configured
// ═══════════════════════════════════════════════════════════════

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    // Process all hardware controls once per block
    hw.ProcessAllControls();

    // ── Read controls ──────────────────────────────────────
    float knob_start   = hw.GetKnobValue(DaisyVersio::KNOB_0);
    float knob_length  = hw.GetKnobValue(DaisyVersio::KNOB_1);
    float knob_speed   = hw.GetKnobValue(DaisyVersio::KNOB_2);
    float knob_pitch   = hw.GetKnobValue(DaisyVersio::KNOB_3);
    float knob_decay   = hw.GetKnobValue(DaisyVersio::KNOB_4);
    float knob_lofi    = hw.GetKnobValue(DaisyVersio::KNOB_5);
    float knob_slot    = hw.GetKnobValue(DaisyVersio::KNOB_6);

    // Note: KNOB_0 and KNOB_1 CV jacks are repurposed as gate inputs
    // The GetKnobValue already sums pot + CV, so we need the raw CV.
    // Since the pot and CV sum, we use the knob value directly as our
    // gate detector input when reading CV. The pot position adds an
    // offset, but gates are >3V so they'll still trigger above threshold
    // even with pot offset. However, for clean gate detection, we ideally
    // want just the CV. On Versio, the knobs array gives the SUMMED value.
    // For gate detection we use the summed value - it works because a 5V
    // gate into the jack will push the reading high regardless of pot position.

    float choke_cv = hw.GetKnobValue(DaisyVersio::KNOB_0);
    float open_cv  = hw.GetKnobValue(DaisyVersio::KNOB_1);

    // ── Update playback parameters ─────────────────────────
    playbackEngine.SetStartSlice(knob_start);
    playbackEngine.SetPlayLength(knob_length);
    playbackEngine.SetSpeed(knob_speed);
    playbackEngine.SetPitch(knob_pitch);
    playbackEngine.SetOverlap(knob_decay);
    playbackEngine.SetGrainSize(0.5f); // Fixed internal default: ~50ms, good all-rounder
    lofi.SetAmount(knob_lofi);

    // ── Slot selection ─────────────────────────────────────
    uint32_t new_slot = GetSlotFromKnob(knob_slot);
    if(new_slot != prev_slot)
    {
        sliceEngine.SetCurrentSlot(new_slot);
        ledManager.ShowSlotDisplay();
        prev_slot = new_slot;
    }

    // ── TAP button state machine ───────────────────────────
    bool tap_pressed = hw.SwitchPressed();

    if(tap_pressed)
    {
        if(!tap_held)
        {
            tap_held = true;
            tap_hold_counter = 0;
        }
        tap_hold_counter += size; // Accumulate hold time
    }
    else if(tap_held)
    {
        // Button released
        tap_held = false;

        if(tap_hold_counter >= HOLD_CLEAR_THRESHOLD)
        {
            // Long hold: CLEAR
            sliceEngine.ClearSlot();
        }
        else
        {
            // Short press: RECORD toggle
            RecordState current_state = sliceEngine.GetState();
            if(current_state == RecordState::RECORDING)
            {
                // Stop recording
                sliceEngine.StopRecording();
            }
            else
            {
                // Start/arm recording
                RecordMode mode   = GetRecordMode();
                uint32_t   beats  = GetSliceCount();
                sliceEngine.ArmRecording(mode, beats);
            }
        }
    }

    // ── Patch detection for choke/open jacks ───────────────
    UpdatePatchDetection(choke_cv, open_cv);
    bool auto_advance = (!choke_patched && !open_patched);
    playbackEngine.SetAutoAdvance(auto_advance);

    // ── Per-sample processing ──────────────────────────────
    for(size_t i = 0; i < size; i++)
    {
        float in_l = in[0][i];
        float in_r = in[1][i];

        // Feed pre-roll buffer (always running)
        sliceEngine.FeedPreroll(in_l, in_r);

        // ── Clock processing ───────────────────────────
        bool clock_tick = hw.Gate(); // Read gate input state
        // Simple edge detection for clock
        static bool prev_gate = false;
        bool clock_rising = (clock_tick && !prev_gate);
        prev_gate = clock_tick;

        clockTracker.Process(clock_rising);

        if(clockTracker.Tick())
        {
            // Clock tick received
            RecordState st = sliceEngine.GetState();

            if(st == RecordState::ARMED || st == RecordState::RECORDING)
            {
                sliceEngine.OnClockTick(clockTracker.GetPeriod());
            }

            if(st == RecordState::PLAYING && auto_advance)
            {
                // Auto-advance on clock when no trigger jacks patched
                playbackEngine.TriggerAuto();
            }
        }

        // ── Gate detection on CV jacks ─────────────────
        chokeGate.Process(choke_cv);
        openGate.Process(open_cv);

        if(chokeGate.RisingEdge() && choke_patched)
        {
            playbackEngine.TriggerChoke();
        }

        if(openGate.RisingEdge() && open_patched)
        {
            playbackEngine.TriggerOpen();
        }

        // ── Threshold detection ────────────────────────
        if(sliceEngine.GetState() == RecordState::ARMED &&
           GetRecordMode() == RecordMode::THRESHOLD)
        {
            if(sliceEngine.CheckThreshold(in_l, in_r))
            {
                sliceEngine.OnThresholdExceeded();
            }
        }

        // ── Recording ──────────────────────────────────
        if(sliceEngine.GetState() == RecordState::RECORDING)
        {
            sliceEngine.RecordSample(in_l, in_r);
        }

        // ── Playback ───────────────────────────────────
        float out_l = 0.0f;
        float out_r = 0.0f;

        playbackEngine.Process(out_l, out_r);

        // ── Lo-fi ──────────────────────────────────────
        lofi.Process(out_l, out_r);

        // ── Output ─────────────────────────────────────
        // During recording with no sample yet, pass through input
        RecordState current = sliceEngine.GetState();
        if(current == RecordState::EMPTY ||
           current == RecordState::ARMED ||
           current == RecordState::RECORDING)
        {
            // Pass-through input so you can hear what you're sampling
            out[0][i] = in_l;
            out[1][i] = in_r;
        }
        else
        {
            out[0][i] = out_l;
            out[1][i] = out_r;
        }
    }

    // ── LED Update (once per block, not per sample) ────────
    RecordState led_state = sliceEngine.GetState();
    float progress = 0.0f;

    if(led_state == RecordState::RECORDING)
    {
        progress = sliceEngine.GetRecordProgress();
    }
    else if(led_state == RecordState::PLAYING)
    {
        progress = playbackEngine.GetPlaybackProgress();
    }

    bool clearing = (tap_held && tap_hold_counter >= HOLD_CLEAR_THRESHOLD);

    // Build slot states array for LED manager
    RecordState slot_states[NUM_SLOTS];
    for(uint32_t s = 0; s < NUM_SLOTS; s++)
    {
        slot_states[s] = sliceEngine.GetSlotState(s);
    }

    ledManager.Update(led_state, progress, sliceEngine.GetCurrentSlot(),
                      slot_states, clearing);

    // Write LED colors to hardware
    for(uint32_t l = 0; l < 4; l++)
    {
        const LedManager::LedColor& c = ledManager.GetLed(l);
        hw.SetLed(l, c.r, c.g, c.b);
    }
    hw.UpdateLeds();
}

// ═══════════════════════════════════════════════════════════════
//  MAIN
// ═══════════════════════════════════════════════════════════════

int main(void)
{
    // Initialize Versio hardware
    hw.Init(true); // true = boost for 96kHz

    // Set sample rate to 96kHz for maximum fidelity
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);

    // Block size - 256 is good balance of latency vs efficiency
    hw.SetAudioBlockSize(256);

    float sample_rate = hw.AudioSampleRate();

    // Initialize DSP modules
    sliceEngine.Init(sdram_buf_l, sdram_buf_r,
                     preroll_l, preroll_r,
                     sample_rate);

    playbackEngine.Init(&sliceEngine, sample_rate);
    lofi.Init(sample_rate);
    ledManager.Init(sample_rate);

    // Start ADC for knob/CV reading
    hw.StartAdc();

    // Start audio processing
    hw.StartAudio(AudioCallback);

    // Main loop - nothing to do here, everything happens in the callback
    for(;;)
    {
        // Could add QSPI flash operations here for persistent storage
        // (save/load from flash is slow and should NOT happen in audio callback)
        hw.DelayMs(1);
    }
}
