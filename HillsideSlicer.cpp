/**
 * THE HILLSIDE SLICER - Custom Versio Firmware
 *
 * Panel:
 *   1 / KNOB_0 (top-left)      : Start slice / [Settings: Bars 1-4]
 *   2 / KNOB_1 (bottom-left)   : Input Volume / [Settings: Merge DEST slot 1-8]
 *   3 / KNOB_2 (top-center)    : Speed / [Settings: BPM 40-200 in 5s]
 *   4 / KNOB_3 (bottom-center) : Pitch (-24/+12 semi) / [Settings: Merge SRC slot 1-8]
 *   5 / KNOB_4 (top-right)     : End slice / [Settings: Slices 4-64]
 *   6 / KNOB_5 (right-middle)  : Slice Gate
 *   7 / KNOB_6 (bottom-right)  : Slot select (1-8)
 *
 *   ABC / SW_0 : Immediate / ClockSync / Threshold
 *   XYZ / SW_1 : Loop / OneShot / SETTINGS
 *   FSU / TAP  : Record / Stop / Hold=Clear
 *   GATE IN    : 24PPQN Clock In (bar-sync playback)
 *
 *   Volume knob (KNOB_1):
 *     Normal mode: Controls INCOMING signal level (monitoring + recording).
 *     Settings mode: Controls OUTPUT volume (end-of-chain, real-time).
 *     On config exit, input vol uses catch-up to prevent jumps.
 *
 *   Settings mode (SW_1 down):
 *     STAGED — changes shown on LEDs but only applied when you
 *     flip back to Loop or OneShot. Audio continues unchanged.
 *     Hold TAP 1.5s = LEDs flash red (warning).
 *     Hold TAP 2.0s = full module reset (all slots, default config).
 *
 *   Buffer Merge (settings mode):
 *     KNOB_1 = destination slot, KNOB_3 = source slot.
 *     Triple-tap TAP to execute:
 *       Tap 1: dim solid red (2s timeout)
 *       Tap 2: bright solid red (2s timeout)
 *       Tap 3: MERGE — source overdubbed into destination
 *     Result: audio summed sample-by-sample. If source is longer,
 *     destination extends to match. Resliced automatically.
 *     White LED flash on completion.
 *
 *   Persistence:
 *     Slot 1 (index 0) auto-saves to 8MB QSPI flash after recording.
 *     Restored on power-up. Also saves after merge into slot 1.
 *     LED 0: dim white during save, bright green when done.
 *
 *   Bar extension:
 *     If you increase bars in settings (e.g. 1→2), the existing
 *     recording is extended with silence and re-sliced on exit.
 *
 *   Catch-up knobs on mode transitions.
 *   Record starts with default params (speed=1x, pitch=0, etc).
 */

#include "daisy_versio.h"
#include "daisysp.h"

#include "gate_detect.h"
#include "slice_engine.h"
#include "grain_engine.h"
#include "led_manager.h"
#include "persistence_manager.h"

using namespace daisy;

DaisyVersio hw;

// SDRAM
static constexpr uint32_t TOTAL_BUF_SIZE = MAX_SLOT_SAMPLES * NUM_SLOTS;
static float DSY_SDRAM_BSS sdram_buf_l[TOTAL_BUF_SIZE];
static float DSY_SDRAM_BSS sdram_buf_r[TOTAL_BUF_SIZE];
static float DSY_SDRAM_BSS preroll_l[PREROLL_SAMPLES];
static float DSY_SDRAM_BSS preroll_r[PREROLL_SAMPLES];

// DSP
SliceEngine        sliceEngine;
PlaybackEngine     playbackEngine;
LedManager         ledManager;
PersistenceManager persistManager;
ClockTracker    clockTracker;
PPQNClockDetector ppqnClock;

// Active settings (what the engine uses)
static RecordSettings active_settings;

// Staged settings (what the config page edits, applied on exit)
static RecordSettings staged_settings;

// Persistence: flag set in audio callback, handled in main loop
static volatile bool save_slot0_pending = false;

// ── Buffer Merge (config mode) ────────────────────────────────
// Triple-tap confirmation to overdub one slot into another.
// Knob 5 = destination slot, Knob 3 = source slot (in config mode).
enum class MergeState
{
    IDLE,
    CONFIRM_1,   // Dim solid red — tap again within 2s
    CONFIRM_2,   // Bright solid red — tap again within 2s to execute
};
static MergeState  merge_state       = MergeState::IDLE;
static uint32_t    merge_timeout     = 0;
static uint32_t    merge_dest_slot   = 0;
static uint32_t    merge_src_slot    = 0;
static uint32_t    merge_done_timer  = 0;  // White flash after merge

static constexpr uint32_t MERGE_TIMEOUT_TICKS = 750;  // ~2s at 375 ticks/s

// ── Catch-up knob system ───────────────────────────────────────
static constexpr float CATCH_THRESHOLD = 0.03f;

struct CatchKnob
{
    float target = 0.5f;
    bool  caught = true;

    float Update(float raw)
    {
        if(caught) { target = raw; return raw; }
        if(fabsf(raw - target) < CATCH_THRESHOLD)
        {
            caught = true;
            target = raw;
            return raw;
        }
        return target;
    }

    void Set(float t) { target = t; caught = false; }
    void Catch(float raw) { target = raw; caught = true; }
};

static CatchKnob ck_start;
static CatchKnob ck_speed;
static CatchKnob ck_pitch;
static CatchKnob ck_end;
static CatchKnob ck_gate;
static CatchKnob ck_input_vol;

// Output volume (set in config mode, applied at end of chain)
static float output_volume = 1.0f;
static uint32_t frozen_slot = 0;

// First-move-activates for speed/pitch during recording
static bool  speed_frozen     = false;
static bool  pitch_frozen     = false;
static float speed_freeze_raw = 0.5f;
static float pitch_freeze_raw = 0.5f;

// State
static bool     tap_held          = false;
static uint32_t tap_hold_counter  = 0;
static uint32_t prev_slot         = 0;
static bool     was_recording     = false;
static bool     in_settings_mode  = false;
static PlaybackMode prev_play_mode = PlaybackMode::LOOP;
static bool     prev_settings_mode = false;

// Internal clock
static uint32_t internal_clock_counter = 0;
static bool     external_clock_active  = false;
static uint32_t ext_clock_timeout      = 0;

// Gate
static bool     prev_gate_state   = false;
static uint32_t gate_flash_timer  = 0;

// Knob vis
static float    prev_knob_start  = -1.0f;
static float    prev_knob_end    = -1.0f;
static uint32_t knob_vis_timer   = 0;

// Settings flash
// (settings flash removed — solid colors only)
static uint32_t prev_staged_bars    = 1;
static uint32_t prev_staged_slices  = 16;
static float    prev_staged_bpm     = 120.0f;
static uint32_t config_param_flash  = 0;    // countdown for magenta flash
static uint32_t config_flash_led    = 0;    // which LED to flash (0-2)

// Config-mode hold reset
static bool     config_tap_held       = false;
static uint32_t config_hold_counter   = 0;
static bool     config_reset_done     = false;

static constexpr uint32_t HOLD_CLEAR_THRESHOLD   = 115200;  // 1.2s normal clear
static constexpr uint32_t CONFIG_WARN_THRESHOLD  = 144000;  // 1.5s red flash
static constexpr uint32_t CONFIG_RESET_THRESHOLD = 192000;  // 2.0s full reset
static constexpr uint32_t EXT_CLOCK_TIMEOUT      = 384000;
static constexpr float    KNOB_CHANGE_THRESH     = 0.02f;
static constexpr uint32_t KNOB_VIS_DURATION      = 300;
static constexpr uint32_t GATE_FLASH_DURATION    = 4800;


// ─── Helpers ───────────────────────────────────────────────────

RecordMode GetRecordMode()
{
    switch(hw.sw[DaisyVersio::SW_0].Read())
    {
        case Switch3::POS_UP:     return RecordMode::IMMEDIATE;
        case Switch3::POS_CENTER: return RecordMode::CLOCK_SYNC;
        case Switch3::POS_DOWN:   return RecordMode::THRESHOLD;
        default: return RecordMode::IMMEDIATE;
    }
}

PlaybackMode ReadSW1()
{
    switch(hw.sw[DaisyVersio::SW_1].Read())
    {
        case Switch3::POS_UP:
            in_settings_mode = false;
            return PlaybackMode::LOOP;
        case Switch3::POS_CENTER:
            in_settings_mode = false;
            return PlaybackMode::ONESHOT;
        case Switch3::POS_DOWN:
            in_settings_mode = true;
            return PlaybackMode::LOOP;
        default:
            in_settings_mode = false;
            return PlaybackMode::LOOP;
    }
}

uint32_t KnobToSliceIndex(float value, uint32_t num_slices)
{
    if(num_slices == 0) return 0;
    uint32_t idx = static_cast<uint32_t>(value * static_cast<float>(num_slices));
    if(idx >= num_slices) idx = num_slices - 1;
    return idx;
}

uint32_t GetSlotFromKnob(float value)
{
    if(value < 0.110f) return 0;
    if(value < 0.235f) return 1;
    if(value < 0.360f) return 2;
    if(value < 0.485f) return 3;
    if(value < 0.610f) return 4;
    if(value < 0.735f) return 5;
    if(value < 0.860f) return 6;
    return 7;
}

uint32_t KnobToBars(float value)
{
    if(value < 0.25f) return 1;
    if(value < 0.50f) return 2;
    if(value < 0.75f) return 3;
    return 4;
}

float KnobToBPM(float value)
{
    float raw = 40.0f + value * 160.0f;
    float snapped = 5.0f * floorf(raw / 5.0f + 0.5f);
    if(snapped < 40.0f) snapped = 40.0f;
    if(snapped > 200.0f) snapped = 200.0f;
    return snapped;
}

uint32_t KnobToSlices(float value)
{
    if(value < 0.20f) return 4;
    if(value < 0.40f) return 8;
    if(value < 0.60f) return 16;
    if(value < 0.80f) return 32;
    return 64;
}

inline float HardLimit(float x)
{
    if(x > 1.0f) return 1.0f;
    if(x < -1.0f) return -1.0f;
    return x;
}

void ApplyRecordDefaults()
{
    // Speed/pitch: freeze at noon until user moves knob
    speed_frozen = true;
    speed_freeze_raw = hw.GetKnobValue(DaisyVersio::KNOB_2);
    ck_speed.Catch(0.50f);
    playbackEngine.SetSpeed(0.50f);
    pitch_frozen = true;
    pitch_freeze_raw = hw.GetKnobValue(DaisyVersio::KNOB_3);
    ck_pitch.Catch(0.50f);
    playbackEngine.SetPitch(0.50f);
    // Start/end: use current physical knob position, no catch-up
    float raw_s = hw.GetKnobValue(DaisyVersio::KNOB_0);
    float raw_e = hw.GetKnobValue(DaisyVersio::KNOB_4);
    ck_start.Catch(raw_s);
    ck_end.Catch(raw_e);
    uint32_t ns = playbackEngine.GetNumSlices();
    if(ns == 0) ns = active_settings.slices;
    uint32_t si = static_cast<uint32_t>(raw_s * (ns > 0 ? ns - 1 : 0));
    uint32_t ei = static_cast<uint32_t>(raw_e * (ns > 0 ? ns - 1 : 0));
    if(si >= ns) si = ns - 1;
    if(ei >= ns) ei = ns - 1;
    if(ei < si) ei = si;
    playbackEngine.SetStartSlice(si);
    playbackEngine.SetEndSlice(ei);
    ck_gate.Set(1.0f);
    playbackEngine.SetSliceGate(1.0f);
}

/** Apply staged settings. If bars increased, extend the current
 *  slot's recording with silence. Then re-slice. */
void CommitStagedSettings()
{
    RecordSettings old = active_settings;
    active_settings = staged_settings;
    sliceEngine.SetSettings(active_settings);

    // If bars changed and there's an active recording, extend/truncate
    if(active_settings.bars != old.bars)
    {
        sliceEngine.ExtendToBarCount(active_settings.bars, old.bars);
        playbackEngine.ResyncSlices();
    }

    // Re-slice with new slice count
    if(active_settings.slices != old.slices)
    {
        sliceEngine.ResliceCurrentSlot();
        playbackEngine.ResyncSlices();
    }
}

/** Full module reset: clear all slots, reset config to defaults */
void FullModuleReset()
{
    for(uint32_t s = 0; s < NUM_SLOTS; s++)
    {
        sliceEngine.SetCurrentSlot(s);
        sliceEngine.ClearSlot();
    }
    sliceEngine.SetCurrentSlot(0);
    prev_slot = 0;

    playbackEngine.StopPlayback();

    active_settings.bars   = 1;
    active_settings.slices = 16;
    active_settings.bpm    = 120.0f;
    active_settings.overdub_feedback = 1.0f;
    staged_settings = active_settings;
    sliceEngine.SetSettings(active_settings);

    prev_staged_bars   = 1;
    prev_staged_slices = 16;
    prev_staged_bpm    = 120.0f;

    ApplyRecordDefaults();
}

// ═══════════════════════════════════════════════════════════════

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.ProcessAllControls();
    hw.tap.Debounce();

    // ── Raw knobs ──────────────────────────────────────────
    float knob_0_raw = hw.GetKnobValue(DaisyVersio::KNOB_0);
    float knob_1_raw = hw.GetKnobValue(DaisyVersio::KNOB_1);
    float knob_2_raw = hw.GetKnobValue(DaisyVersio::KNOB_2);
    float knob_3_raw = hw.GetKnobValue(DaisyVersio::KNOB_3);
    float knob_4_raw = hw.GetKnobValue(DaisyVersio::KNOB_4);
    float knob_5_raw = hw.GetKnobValue(DaisyVersio::KNOB_5);
    float knob_6_raw = hw.GetKnobValue(DaisyVersio::KNOB_6);

    // Input volume: squared taper, hard floor at 2%
    // This is an INPUT VCA — controls incoming signal level.
    // Uses catch-up knob so it doesn't jump when exiting config
    // (where KNOB_1 was controlling output volume).
    float input_vol;
    if(!in_settings_mode)
    {
        float iv = ck_input_vol.Update(knob_1_raw);
        input_vol = (iv < 0.02f) ? 0.0f : iv * iv;
    }
    else
    {
        // Frozen in config mode
        input_vol = (ck_input_vol.target < 0.02f) ? 0.0f
                    : ck_input_vol.target * ck_input_vol.target;

        // KNOB_1 controls output volume in real time (squared taper)
        float ov = (knob_1_raw < 0.02f) ? 0.0f
                   : knob_1_raw * knob_1_raw;
        output_volume = ov;
    }

    // ── Mode switch ────────────────────────────────────────
    PlaybackMode play_mode = ReadSW1();
    playbackEngine.SetPlayMode(play_mode);
    if(play_mode == PlaybackMode::ONESHOT && prev_play_mode == PlaybackMode::LOOP)
    {
        if(playbackEngine.IsPlaying() && !playbackEngine.IsOneshotDone())
            playbackEngine.SetOneshotDone();
    }
    if(play_mode == PlaybackMode::LOOP && prev_play_mode == PlaybackMode::ONESHOT)
    {
        if(playbackEngine.IsPlaying() && playbackEngine.IsOneshotDone())
            playbackEngine.StartPlayback();  // Resume looping
    }
    prev_play_mode = play_mode;

    // ── Mode transitions ───────────────────────────────────
    if(in_settings_mode != prev_settings_mode)
    {
        if(in_settings_mode)
        {
            // Entering settings: snapshot current active → staged
            staged_settings = active_settings;
            prev_staged_bars   = staged_settings.bars;
            prev_staged_bpm    = staged_settings.bpm;
            prev_staged_slices = staged_settings.slices;
            config_tap_held     = false;
            config_hold_counter = 0;
            config_reset_done   = false;
            // Freeze slot so it can't change from knob jitter
            frozen_slot = sliceEngine.GetCurrentSlot();
        }
        else
        {
            // Leaving settings: commit staged changes
            CommitStagedSettings();

            // Restore slot — knob_6 may have drifted during config
            sliceEngine.SetCurrentSlot(frozen_slot);
            prev_slot = frozen_slot;

            // Shared knobs need catch-up (were repurposed in settings)
            ck_start.Set(ck_start.target);
            ck_speed.Set(ck_speed.target);
            ck_end.Set(ck_end.target);
            ck_pitch.Set(ck_pitch.target);  // Was merge source selector
            ck_gate.Set(ck_gate.target);    // Was overdub feedback selector
            ck_input_vol.Set(ck_input_vol.target);  // Was output vol
        }
    }
    prev_settings_mode = in_settings_mode;

    // ── Catch-up knobs ─────────────────────────────────────
    float eff_start, eff_speed, eff_end;
    if(!in_settings_mode)
    {
        eff_start = ck_start.Update(knob_0_raw);
        if(speed_frozen) {
            if(knob_2_raw-speed_freeze_raw>0.03f||speed_freeze_raw-knob_2_raw>0.03f){speed_frozen=false;ck_speed.Catch(knob_2_raw);}
            eff_speed=speed_frozen?0.50f:knob_2_raw;
        } else { eff_speed=ck_speed.Update(knob_2_raw); }
        eff_end   = ck_end.Update(knob_4_raw);
    }
    else
    {
        eff_start = ck_start.target;
        eff_speed = ck_speed.target;
        eff_end   = ck_end.target;
    }
    float eff_pitch, eff_gate;
    if(!in_settings_mode)
    {
        if(pitch_frozen) {
            if(knob_3_raw-pitch_freeze_raw>0.03f||pitch_freeze_raw-knob_3_raw>0.03f){pitch_frozen=false;ck_pitch.Catch(knob_3_raw);}
            eff_pitch=pitch_frozen?0.50f:knob_3_raw;
        } else { eff_pitch=ck_pitch.Update(knob_3_raw); }
        eff_gate = ck_gate.Update(knob_5_raw);
    }
    else
    {
        // Settings mode: freeze pitch/gate at caught values
        eff_pitch = ck_pitch.target;
        eff_gate  = ck_gate.target;

        // Knob 1 (bottom-left) = merge destination slot (0-7)
        // Knob 3 (bottom-center) = merge source slot (0-7)
        merge_dest_slot = static_cast<uint32_t>(knob_1_raw * 7.99f);
        if(merge_dest_slot > 7) merge_dest_slot = 7;
        merge_src_slot  = static_cast<uint32_t>(knob_3_raw * 7.99f);
        if(merge_src_slot > 7) merge_src_slot = 7;

        // Knob 5 (right-middle) = overdub feedback
        // Left = 1.0 (pure overdub/sum), Right = 0.0 (full replace)
        float overdub_fb = 1.0f - knob_5_raw;
        staged_settings.overdub_feedback = overdub_fb;
    }

    // ── Settings mode: edit staged values (NOT live) ───────
    if(in_settings_mode)
    {
        uint32_t new_bars   = KnobToBars(knob_0_raw);
        float new_bpm;
        if(ppqnClock.has_clock)
        {
            uint32_t ds = static_cast<uint32_t>(knob_2_raw * 3.99f);
            static constexpr uint32_t dtbl[] = {1, 2, 4, 8};
            ppqnClock.divider = dtbl[ds > 3 ? 3 : ds];
            new_bpm = ppqnClock.GetEffBPM();
        }
        else
            new_bpm = KnobToBPM(knob_2_raw);
        uint32_t new_slices = KnobToSlices(knob_4_raw);

        // Detect parameter changes → trigger magenta flash
        if(new_bars != prev_staged_bars
           || new_slices != prev_staged_slices
           || fabsf(new_bpm - prev_staged_bpm) > 1.0f)
        {
            config_param_flash = 200;  // ~200 callbacks ≈ 0.5s

            // Which LED changed?
            if(new_bars != prev_staged_bars)         config_flash_led = 0;
            else if(fabsf(new_bpm - prev_staged_bpm) > 1.0f) config_flash_led = 1;
            else                                     config_flash_led = 2;
        }

        prev_staged_bars   = new_bars;
        prev_staged_bpm    = new_bpm;
        prev_staged_slices = new_slices;

        staged_settings.bars   = new_bars;
        staged_settings.bpm    = new_bpm;
        staged_settings.slices = new_slices;
        // NOT applied to engine — staged only!
    }

    // ── Playback params ────────────────────────────────────
    uint32_t num_slices = playbackEngine.GetNumSlices();
    if(num_slices == 0) num_slices = active_settings.slices;

    uint32_t start_idx = 0;
    uint32_t end_idx   = num_slices > 0 ? num_slices - 1 : 0;

    if(!in_settings_mode)
    {
        start_idx = KnobToSliceIndex(eff_start, num_slices);
        end_idx   = KnobToSliceIndex(eff_end, num_slices);
        if(end_idx < start_idx) end_idx = start_idx;

        playbackEngine.SetStartSlice(start_idx);
        playbackEngine.SetEndSlice(end_idx);
        playbackEngine.SetSpeed(eff_speed);
    }

    playbackEngine.SetPitch(eff_pitch);
    playbackEngine.SetSliceGate(eff_gate);

    // ── Knob vis ───────────────────────────────────────────
    if(!in_settings_mode)
    {
        RecordState vis_state = sliceEngine.GetState();
        if(vis_state == RecordState::PLAYING ||
           vis_state == RecordState::OVERDUBBING ||
           vis_state == RecordState::STOPPED)
        {
            if(fabsf(knob_0_raw - prev_knob_start) > KNOB_CHANGE_THRESH ||
               fabsf(knob_4_raw - prev_knob_end) > KNOB_CHANGE_THRESH)
                knob_vis_timer = KNOB_VIS_DURATION;
        }
        prev_knob_start = knob_0_raw;
        prev_knob_end   = knob_4_raw;
        if(knob_vis_timer > 0) knob_vis_timer--;
    }

    // ── Slot ───────────────────────────────────────────────
    if(!in_settings_mode)
    {
        uint32_t new_slot = GetSlotFromKnob(knob_6_raw);
        if(new_slot != prev_slot)
        {
            sliceEngine.SetCurrentSlot(new_slot);
            ledManager.ShowSlotDisplay();
            prev_slot = new_slot;
        }
    }

    // ── TAP ────────────────────────────────────────────────
    bool tap_pressed = hw.tap.Pressed();

    if(in_settings_mode)
    {
        // Config mode TAP:
        //   Short tap = advance merge confirmation
        //   Long hold = full module reset (existing)
        if(tap_pressed)
        {
            if(!config_tap_held)
            {
                config_tap_held = true;
                config_hold_counter = 0;
                config_reset_done = false;
            }
            config_hold_counter += size;

            if(!config_reset_done &&
               config_hold_counter >= CONFIG_RESET_THRESHOLD)
            {
                FullModuleReset();
                merge_state = MergeState::IDLE;
                config_reset_done = true;
            }
        }
        else if(config_tap_held)
        {
            // Button released in config mode
            config_tap_held = false;

            // Short tap (not a hold)?
            if(config_hold_counter < CONFIG_WARN_THRESHOLD && !config_reset_done)
            {
                // Advance merge confirmation state
                switch(merge_state)
                {
                    case MergeState::IDLE:
                        // Validate: source must have content, can't merge to self
                        if(merge_dest_slot != merge_src_slot &&
                           sliceEngine.GetSlotState(merge_src_slot) == RecordState::PLAYING)
                        {
                            merge_state   = MergeState::CONFIRM_1;
                            merge_timeout = MERGE_TIMEOUT_TICKS;
                        }
                        break;

                    case MergeState::CONFIRM_1:
                        merge_state   = MergeState::CONFIRM_2;
                        merge_timeout = MERGE_TIMEOUT_TICKS;
                        break;

                    case MergeState::CONFIRM_2:
                        // EXECUTE MERGE
                        sliceEngine.MergeSlots(merge_dest_slot, merge_src_slot);
                        // If we merged into the current slot, resync playback
                        if(merge_dest_slot == sliceEngine.GetCurrentSlot())
                            playbackEngine.ResyncSlices();
                        merge_done_timer = 300;  // White flash ~0.8s
                        merge_state = MergeState::IDLE;
                        // Trigger save if destination is slot 0
                        if(merge_dest_slot == 0)
                            save_slot0_pending = true;
                        break;
                }
            }
            config_hold_counter = 0;
        }
        else
        {
            config_hold_counter = 0;
        }

        // Merge confirmation timeout
        if(merge_state != MergeState::IDLE)
        {
            if(merge_timeout > 0)
                merge_timeout--;
            else
                merge_state = MergeState::IDLE;  // Timed out, cancel
        }
        if(merge_done_timer > 0) merge_done_timer--;
    }
    else
    {
        // Leaving settings mode cancels any merge confirmation
        if(merge_state != MergeState::IDLE)
            merge_state = MergeState::IDLE;
        merge_done_timer = 0;
        // Normal mode TAP: record/overdub/stop/hold-clear
        if(tap_pressed)
        {
            if(!tap_held) { tap_held = true; tap_hold_counter = 0; }
            tap_hold_counter += size;
        }
        else if(tap_held)
        {
            tap_held = false;
            if(tap_hold_counter >= HOLD_CLEAR_THRESHOLD)
            {
                // Long hold = clear slot (from any state)
                sliceEngine.StopOverdub();
                sliceEngine.ClearSlot();
                playbackEngine.StopPlayback();
                playbackEngine.ResetPitchShifters();
            }
            else
            {
                RecordState st = sliceEngine.GetState();
                if(st == RecordState::RECORDING)
                {
                    // Stop recording → finalize → play
                    sliceEngine.StopRecording();
                    playbackEngine.StartPlayback();

                    // Release all catch-up knobs so they respond
                    // immediately. Defaults were applied on arm;
                    // now let the user move knobs freely.
                    ck_start.Catch(ck_start.target);
                    ck_end.Catch(ck_end.target);
                    ck_speed.Catch(ck_speed.target);
                    ck_pitch.Catch(ck_pitch.target);
                    ck_gate.Catch(ck_gate.target);
                    speed_frozen = false;
                    pitch_frozen = false;
                }
                else if(st == RecordState::PLAYING)
                {
                    if(play_mode == PlaybackMode::ONESHOT
                       && playbackEngine.IsOneshotDone())
                    {
                        playbackEngine.StartPlayback();
                    }
                    else
                    {
                        sliceEngine.StartOverdub();
                    }
                }
                else if(st == RecordState::OVERDUBBING)
                {
                    // Stop overdubbing → back to normal playback
                    sliceEngine.StopOverdub();
                    // Trigger save if slot 0
                    if(sliceEngine.GetCurrentSlot() == 0)
                        save_slot0_pending = true;
                }
                else if(st == RecordState::STOPPED ||
                        st == RecordState::EMPTY)
                {
                    // Empty/stopped → arm new recording
                    playbackEngine.StopPlayback();
                    sliceEngine.ArmRecording(GetRecordMode());
                    ApplyRecordDefaults();
                }
                else
                {
                    // ARMED → just re-arm (already armed)
                    sliceEngine.ArmRecording(GetRecordMode());
                    ApplyRecordDefaults();
                }
            }
        }
        else
        {
            tap_hold_counter = 0;
        }
    }

    // ── Auto-finalize ──────────────────────────────────────
    RecordState current_state = sliceEngine.GetState();
    if(was_recording && current_state == RecordState::PLAYING)
    {
        playbackEngine.StartPlayback();

        // Release ALL catch-up knobs so they respond immediately.
        // This path fires when recording auto-stops via clock tick,
        // which bypasses the tap handler entirely.
        ck_start.Catch(ck_start.target);
        ck_end.Catch(ck_end.target);
        ck_speed.Catch(ck_speed.target);
        ck_pitch.Catch(ck_pitch.target);
        ck_gate.Catch(ck_gate.target);
        speed_frozen = false;
        pitch_frozen = false;

        // Trigger save if slot 0 just finished recording
        if(sliceEngine.GetCurrentSlot() == 0)
            save_slot0_pending = true;
    }
    was_recording = (current_state == RecordState::RECORDING);

    // ── Internal clock (uses ACTIVE settings, not staged) ──
    uint32_t int_clock_period = sliceEngine.GetInternalClockPeriod();

    // ── Per-sample ─────────────────────────────────────────
    for(size_t i = 0; i < size; i++)
    {
        float raw_l = in[0][i];
        float raw_r = in[1][i];

        // Input VCA: volume knob scales the incoming signal.
        // This is what gets monitored AND recorded.
        float in_l = raw_l * input_vol;
        float in_r = raw_r * input_vol;

        sliceEngine.FeedPreroll(in_l, in_r);

        // ── Gate ───────────────────────────────────────
        bool gate_now = hw.Gate();
        bool gate_rising = (gate_now && !prev_gate_state);
        prev_gate_state = gate_now;

        if(gate_rising)
            gate_flash_timer = GATE_FLASH_DURATION;
        if(gate_flash_timer > 0)
            gate_flash_timer--;

        // == Route gate (24PPQN clock on Gate jack) ====
        ppqnClock.bar_length = active_settings.bars * 4;
        ppqnClock.Process(gate_rising);

        RecordState st_now = sliceEngine.GetState();

        if(st_now == RecordState::ARMED || st_now == RecordState::RECORDING)
        {
            bool got_tick = false;
            uint32_t tick_period = int_clock_period;

            if(ppqnClock.beat_tick)
            {
                got_tick = true;
                tick_period = ppqnClock.GetBeatPeriod();
                if(tick_period == 0) tick_period = int_clock_period;
                external_clock_active = true;
                ext_clock_timeout = 0;
            }
            else
            {
                ext_clock_timeout++;
                if(ext_clock_timeout >= EXT_CLOCK_TIMEOUT)
                    external_clock_active = false;
                if(!external_clock_active)
                {
                    internal_clock_counter++;
                    if(internal_clock_counter >= int_clock_period)
                    {
                        internal_clock_counter = 0;
                        got_tick = true;
                    }
                }
            }

            if(got_tick)
                sliceEngine.OnClockTick(tick_period);
        }
        else if(st_now == RecordState::PLAYING ||
                st_now == RecordState::OVERDUBBING)
        {
            // Bar tick resets playhead in loop mode (keeps slicer synced)
            if(ppqnClock.has_clock && ppqnClock.bar_tick
               && play_mode == PlaybackMode::LOOP)
            {
                playbackEngine.ResetPlayhead();
                external_clock_active = true;
                ext_clock_timeout = 0;
            }

            if(!ppqnClock.has_clock)
            {
                internal_clock_counter++;
                if(internal_clock_counter >= int_clock_period)
                    internal_clock_counter = 0;
            }
        }
        else if(st_now == RecordState::STOPPED)
        {
            // Clock starts while stopped with content: auto-start loop
            if(ppqnClock.beat_tick && play_mode == PlaybackMode::LOOP)
            {
                const SlotHeader& hdr = sliceEngine.GetHeader();
                if(hdr.num_slices > 0 && hdr.total_samples > 0)
                {
                    playbackEngine.StartPlayback();
                    ppqnClock.ResetBar();
                    external_clock_active = true;
                    ext_clock_timeout = 0;
                }
            }
        }
        else
        {
            if(ppqnClock.has_clock)
            { external_clock_active = true; ext_clock_timeout = 0; }
            else
            {
                ext_clock_timeout++;
                if(ext_clock_timeout >= EXT_CLOCK_TIMEOUT)
                    external_clock_active = false;
            }
        }
        // ── Threshold ──────────────────────────────────
        if(sliceEngine.GetState() == RecordState::ARMED &&
           GetRecordMode() == RecordMode::THRESHOLD)
        {
            if(sliceEngine.CheckThreshold(in_l, in_r))
                sliceEngine.OnThresholdExceeded();
        }

        // ── Recording ──────────────────────────────────
        if(sliceEngine.GetState() == RecordState::RECORDING)
            sliceEngine.RecordSample(in_l, in_r);

        // ── Output ─────────────────────────────────────
        RecordState current = sliceEngine.GetState();

        float mix_l, mix_r;

        bool is_active = (current == RecordState::PLAYING ||
                          current == RecordState::OVERDUBBING)
                         && playbackEngine.IsPlaying()
                         && !playbackEngine.IsOneshotDone();

        if(is_active)
        {
            float play_l = 0.0f;
            float play_r = 0.0f;
            playbackEngine.Process(play_l, play_r);

            // ── Overdub: write input into buffer at play position ──
            if(current == RecordState::OVERDUBBING)
            {
                uint32_t abs_pos = playbackEngine.GetAbsoluteReadPos();
                sliceEngine.OverdubSample(abs_pos, in_l, in_r,
                                          active_settings.overdub_feedback);
            }

            // Always sum input + playback.
            // The Versio has no hardware bypass — if we don't output
            // the input signal, it's gone. Player needs to hear both
            // the loop AND their live input to jam/perform.
            mix_l = HardLimit(in_l + play_l);
            mix_r = HardLimit(in_r + play_r);
        }
        else
        {
            // Not playing: input monitoring at VCA level
            mix_l = in_l;
            mix_r = in_r;
        }

        out[0][i] = mix_l * output_volume;
        out[1][i] = mix_r * output_volume;
    }

    // ── LEDs ───────────────────────────────────────────────

    RecordState led_state = sliceEngine.GetState();
    float progress = 0.0f;

    if(led_state == RecordState::RECORDING)
        progress = sliceEngine.GetRecordProgress();
    else if(led_state == RecordState::PLAYING ||
            led_state == RecordState::OVERDUBBING)
        progress = playbackEngine.GetPlaybackProgress();

    float clear_progress = 0.0f;
    if(!in_settings_mode && tap_held && tap_hold_counter > 0)
    {
        clear_progress = static_cast<float>(tap_hold_counter)
                       / static_cast<float>(HOLD_CLEAR_THRESHOLD);
        if(clear_progress > 1.0f) clear_progress = 1.0f;
    }

    RecordState slot_states[NUM_SLOTS];
    for(uint32_t s = 0; s < NUM_SLOTS; s++)
        slot_states[s] = sliceEngine.GetSlotState(s);

    bool show_range = (!in_settings_mode) && (knob_vis_timer > 0) &&
                      (led_state == RecordState::PLAYING ||
                       led_state == RecordState::OVERDUBBING ||
                       led_state == RecordState::STOPPED);

    bool cyan_flash = (!in_settings_mode) &&
                      playbackEngine.StartSliceChanged() &&
                      (led_state == RecordState::PLAYING ||
                       led_state == RecordState::OVERDUBBING ||
                       led_state == RecordState::STOPPED);
    playbackEngine.TickFlashTimer();

    if(in_settings_mode)
    {
        if(config_tap_held && config_hold_counter >= CONFIG_RESET_THRESHOLD)
        {
            hw.SetLed(0, 1.0f, 1.0f, 1.0f);
            hw.SetLed(1, 1.0f, 1.0f, 1.0f);
            hw.SetLed(2, 1.0f, 1.0f, 1.0f);
            hw.SetLed(3, 1.0f, 1.0f, 1.0f);
        }
        else if(config_tap_held &&
                config_hold_counter >= CONFIG_WARN_THRESHOLD)
        {
            float hold_frac = static_cast<float>(
                config_hold_counter - CONFIG_WARN_THRESHOLD)
                / static_cast<float>(
                    CONFIG_RESET_THRESHOLD - CONFIG_WARN_THRESHOLD);
            float v = 0.3f + hold_frac * 0.7f;
            hw.SetLed(0, v, 0.0f, 0.0f);
            hw.SetLed(1, v, 0.0f, 0.0f);
            hw.SetLed(2, v, 0.0f, 0.0f);
            hw.SetLed(3, v, 0.0f, 0.0f);
        }
        else if(merge_done_timer > 0)
        {
            float v = static_cast<float>(merge_done_timer) / 300.0f;
            if(v > 1.0f) v = 1.0f;
            hw.SetLed(0, v, v, v);
            hw.SetLed(1, v, v, v);
            hw.SetLed(2, v, v, v);
            hw.SetLed(3, v, v, v);
        }
        else if(merge_state == MergeState::CONFIRM_1)
        {
            hw.SetLed(0, 0.25f, 0.0f, 0.0f);
            hw.SetLed(1, 0.25f, 0.0f, 0.0f);
            hw.SetLed(2, 0.25f, 0.0f, 0.0f);
            hw.SetLed(3, 0.25f, 0.0f, 0.0f);
        }
        else if(merge_state == MergeState::CONFIRM_2)
        {
            hw.SetLed(0, 0.7f, 0.0f, 0.0f);
            hw.SetLed(1, 0.7f, 0.0f, 0.0f);
            hw.SetLed(2, 0.7f, 0.0f, 0.0f);
            hw.SetLed(3, 0.7f, 0.0f, 0.0f);
        }
        else
        {
            // Config: FIXED brightness. NO knob-proportional values.
            // ADC jitter on knobs was causing brightness oscillation.
            hw.SetLed(0, 0.15f, 0.15f, 0.15f);  // white = Bars
            hw.SetLed(1, 0.0f, 0.08f, 0.15f);   // cyan  = BPM
            hw.SetLed(2, 0.0f, 0.15f, 0.0f);    // green = Slices
            hw.SetLed(3, 0.0f, 0.0f, 0.0f);     // OFF

            // Magenta flash: solid on/off when param changes
            if(config_param_flash > 0 && config_flash_led < 3)
                hw.SetLed(config_flash_led, 0.5f, 0.0f, 0.5f);
        }
    }
    else
    {
        bool oneshot_waiting = (play_mode == PlaybackMode::ONESHOT)
                              && playbackEngine.IsOneshotDone()
                              && (led_state == RecordState::PLAYING);
    ledManager.Update(led_state, progress, sliceEngine.GetCurrentSlot(),
                          slot_states, clear_progress,
                          show_range, start_idx, end_idx, num_slices,
                          cyan_flash, start_idx, oneshot_waiting);

        for(uint32_t l = 0; l < 4; l++)
        {
            const LedManager::LedColor& c = ledManager.GetLed(l);
            hw.SetLed(l, c.r, c.g, c.b);
        }
    }

    hw.UpdateLeds();
    if(config_param_flash > 0) config_param_flash--;
}

int main(void)
{
    hw.Init(true);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);
    hw.SetAudioBlockSize(256);

    float sample_rate = hw.AudioSampleRate();

    active_settings.bars   = 1;
    active_settings.slices = 16;
    active_settings.bpm    = 120.0f;
    staged_settings = active_settings;

    prev_staged_bars   = 1;
    prev_staged_slices = 16;
    prev_staged_bpm    = 120.0f;

    sliceEngine.Init(sdram_buf_l, sdram_buf_r,
                     preroll_l, preroll_r, sample_rate);
    sliceEngine.SetSettings(active_settings);

    playbackEngine.Init(&sliceEngine, sample_rate);
    ledManager.Init(sample_rate);

    // ── Persistence: init and load slot 0 from flash ──────
    persistManager.Init(&hw.seed.qspi, &sliceEngine,
                        sdram_buf_l, sdram_buf_r, MAX_SLOT_SAMPLES);

    bool loaded = persistManager.LoadOnBoot();
    if(loaded)
    {
        // Restore settings from flash
        if(persistManager.HasLoadedSettings())
        {
            active_settings = persistManager.GetLoadedSettings();
            staged_settings = active_settings;
            sliceEngine.SetSettings(active_settings);

            // Sync prev_staged so settings page doesn't flash on first enter
            prev_staged_bars   = active_settings.bars;
            prev_staged_slices = active_settings.slices;
            prev_staged_bpm    = active_settings.bpm;
        }
        // Start playback of restored slot 0
        playbackEngine.StartPlayback();
    }

    ck_start.Catch(0.0f);
    ck_speed.Catch(0.5f);
    ck_pitch.Catch(0.5f);
    ck_end.Catch(1.0f);
    ck_gate.Catch(1.0f);
    ck_input_vol.Catch(1.0f);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // ── Main loop: persistence + LED save feedback ────────
    for(;;)
    {
        // Check if audio callback requested a save
        if(save_slot0_pending && !persistManager.IsSaving())
        {
            save_slot0_pending = false;
            persistManager.StartSave();
        }

        // Tick the persistence state machine (chunked writes)
        persistManager.Tick();

        hw.DelayMs(1);
    }
}
