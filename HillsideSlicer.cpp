/**
 * THE HILLSIDE SLICER 0.7-alpha - Custom Versio Firmware
 *
 * Panel (Play Mode):
 *   KNOB_0 (top-left)      : Start Slice
 *   KNOB_1 (bottom-left)   : Crossfader (CV accessible)
 *   KNOB_2 (top-center)    : Speed (0.5x-2x)
 *   KNOB_3 (bottom-center) : Pitch Shift (±12 semitones)
 *   KNOB_4 (top-right)     : Slice Length
 *   KNOB_5 (right-middle)  : Swing (left=straight, right=heavy)
 *   KNOB_6 (bottom-right)  : Slot select (1-8)
 *
 * Panel (Config Mode — SW_1 down):
 *   KNOB_0 : Bars (1-4)
 *   KNOB_1 : Input Volume (set and forget)
 *   KNOB_2 : BPM / Clock Divider (external)
 *   KNOB_3 : (reserved)
 *   KNOB_4 : Slices per bar (4-32)
 *   KNOB_5 : (reserved)
 *   KNOB_6 : unused
 *
 *   ABC / SW_0 : Immediate / ClockSync / Threshold
 *   XYZ / SW_1 : Loop / OneShot / SETTINGS
 *   FSU / TAP  : Record / Stop / Hold=Clear
 *   GATE IN    : Clock In (1 pulse per beat)
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


// Active settings (what the engine uses)
static RecordSettings active_settings;

// Staged settings (what the config page edits, applied on exit)
static RecordSettings staged_settings;

// Persistence: flag set in audio callback, handled in main loop
static volatile bool save_slot0_pending = false;

// ── Buffer Merge (config mode) ────────────────────────────────
// Triple-tap confirmation to overdub one slot into another.
// Knob 5 = destination slot, Knob 3 = source slot (in config mode).
// ── Effect enums ──────────────────────────────────────────────
enum class MergeState
{
    IDLE,
    CONFIRM_1,
    CONFIRM_2,
};
static MergeState  merge_state       = MergeState::IDLE;
static uint32_t   tap_tempo_count   = 0;
static uint32_t   tap_tempo_samples = 0;
static uint32_t   tap_tempo_prev    = 0;
static uint32_t   tap_tempo_timeout = 0;
static uint32_t    merge_timeout     = 0;
static uint32_t    merge_done_timer  = 0;  // White flash after merge

// Config knob snapshots: only apply if knob moved from entry position
static float cfg_snap_k0 = 0.5f;  // Bars
static float cfg_snap_k1 = 0.5f;  // Resample
static float cfg_snap_k2 = 0.5f;  // BPM
static float cfg_snap_k3 = 0.5f;  // Audio effect
static float cfg_snap_k4 = 0.5f;  // Slices
static float cfg_snap_k5 = 0.5f;  // Play effect
static bool  cfg_moved_k0 = false;
static bool  cfg_moved_k1 = false;
static bool  cfg_moved_k2 = false;
static bool  cfg_moved_k3 = false;
static bool  cfg_moved_k4 = false;
static bool  cfg_moved_k5 = false;
static constexpr float CFG_MOVE_THRESH = 0.05f;

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
static float stored_input_vol_ = 1.0f;  // Input volume, set in config
static float frozen_crossfader = 0.5f;   // Crossfader position saved on config entry

// Active effect selections (applied immediately, not staged)
static uint32_t    effect_vis_timer    = 0;
static uint32_t startup_mute = 4800;  // 100ms silence at boot (48kHz)  // false=slice gate, true=crossfader
static uint32_t frozen_slot = 0;
static PlaybackMode frozen_play_mode = PlaybackMode::LOOP;

// First-move-activates for speed/pitch during recording
static bool  speed_frozen     = false;
static bool  pitch_frozen     = false;
static float speed_freeze_raw = 0.5f;
static float pitch_freeze_raw = 0.5f;
static bool  threshold_rearm = false;
static bool     auto_overdub   = false;   // Threshold-triggered overdub: auto-stop on loop
static uint32_t overdub_start_slice = 0;
static uint32_t overdub_sample_count = 0;  // Slice where overdub started
static uint32_t threshold_grace = 0;  // Grace period after arming  // Re-arm threshold while playing

// State
static bool     tap_held          = false;
static uint32_t tap_hold_counter  = 0;
static uint32_t prev_slot         = 0;
static float    prev_slot_knob    = 0.0f;
static bool     was_recording     = false;
static bool     in_settings_mode  = false;
static PlaybackMode prev_play_mode = PlaybackMode::LOOP;
static bool     prev_settings_mode = false;
static uint32_t sw1_stable_frames  = 99;  // Frames since last switch change
static PlaybackMode sw1_last_mode  = PlaybackMode::LOOP;
static bool         sw1_last_settings = false;

// Internal clock
static uint32_t internal_clock_counter = 0;
static bool     clock_mode_external    = false;  // User's choice (TAP hold toggle)
static bool     external_clock_active  = false;  // Actually receiving edges
static uint32_t ext_clock_timeout      = 0;

// Gate
static bool     prev_gate_state   = false;
static uint32_t gate_flash_timer  = 0;
static uint32_t ext_clock_samples = 0;  // Samples between clock edges
static uint32_t last_clock_period = 0;  // Last measured period (for BPM display)
static float    recorded_bpm     = 120.0f;  // BPM at time of recording
static float    ext_clock_mult   = 1.0f;   // Clock divider/multiplier from config
static uint32_t ext_beat_count    = 0;  // Beats counted for bar sync

// Knob vis
static float    prev_knob_start  = -1.0f;
static float    prev_knob_end    = -1.0f;
static uint32_t knob_vis_timer   = 0;
static float    prev_knob_gate   = -1.0f;
static uint32_t xfade_vis_timer  = 0;
static uint32_t resample_vis_timer = 0;

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
static constexpr uint32_t CONFIG_WARN_THRESHOLD  = 72000;   // 0.75s blue ramp
static constexpr uint32_t CONFIG_RESET_THRESHOLD = 115200;  // 1.2s toggle clock
static constexpr uint32_t EXT_CLOCK_TIMEOUT      = 96000;   // 2s at 48kHz
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

/** Read SW_1 with debounce. Only updates in_settings_mode and
 *  returns a new play_mode after the switch has been stable for
 *  3+ frames. This prevents switch-travel through CENTER from
 *  triggering ONESHOT when moving UP↔DOWN. */
PlaybackMode ReadSW1()
{
    auto pos = hw.sw[DaisyVersio::SW_1].Read();

    PlaybackMode new_mode;
    bool new_settings;
    switch(pos)
    {
        case Switch3::POS_UP:
            new_settings = false; new_mode = PlaybackMode::LOOP; break;
        case Switch3::POS_CENTER:
            new_settings = false; new_mode = PlaybackMode::ONESHOT; break;
        case Switch3::POS_DOWN:
            new_settings = true;  new_mode = PlaybackMode::LOOP; break;
        default:
            new_settings = false; new_mode = PlaybackMode::LOOP; break;
    }

    // Track stability
    if(new_mode != sw1_last_mode || new_settings != sw1_last_settings)
    {
        sw1_stable_frames = 0;
        sw1_last_mode = new_mode;
        sw1_last_settings = new_settings;
    }
    else if(sw1_stable_frames < 99)
    {
        sw1_stable_frames++;
    }

    // Only commit after 3 stable frames (~0.8ms at 256-sample blocks)
    if(sw1_stable_frames >= 10)
    {
        in_settings_mode = new_settings;
        return new_mode;
    }

    // During instability, keep previous state
    in_settings_mode = prev_settings_mode;
    return (prev_settings_mode) ? frozen_play_mode : prev_play_mode;
}

uint32_t KnobToSliceIndex(float value, uint32_t num_slices)
{
    if(num_slices == 0) return 0;
    uint32_t idx = static_cast<uint32_t>(value * static_cast<float>(num_slices));
    if(idx >= num_slices) idx = num_slices - 1;
    return idx;
}

/** Map 0.0-1.0 to slice length 1..num_slices.
 *  Far left = 1 slice, far right = all slices. */
uint32_t KnobToSliceLength(float value, uint32_t num_slices)
{
    if(num_slices == 0) return 1;
    uint32_t len = 1 + static_cast<uint32_t>(value * static_cast<float>(num_slices - 1) + 0.5f);
    if(len > num_slices) len = num_slices;
    if(len < 1) len = 1;
    return len;
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

/** Map KNOB_4 to slices per bar: 4, 8, 16 (noon), 24, 32.
 *  5 positions with dead zones. Noon default = 16. */
uint32_t KnobToSlicesPerBar(float value)
{
    if(value < 0.15f) return 4;
    if(value < 0.35f) return 8;
    if(value < 0.65f) return 16;   // Dead zone at noon
    if(value < 0.85f) return 24;
    return 32;
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
    uint32_t si = KnobToSliceIndex(raw_s, ns);
    uint32_t len = KnobToSliceLength(raw_e, ns);
    uint32_t ei = si + len - 1;
    if(ei >= ns) ei = ns - 1;
    playbackEngine.SetStartSlice(si);
    playbackEngine.SetEndSlice(ei);
}

/** Apply staged settings. If bars increased, extend the current
 *  slot's recording with silence. Then re-slice. */
void CommitStagedSettings()
{
    RecordSettings old = active_settings;
    active_settings = staged_settings;
    sliceEngine.SetSettings(active_settings);

    bool changed = false;
    if(active_settings.bars != old.bars)
    {
        sliceEngine.ExtendToBarCount(active_settings.bars, old.bars);
        changed = true;
    }
    if(active_settings.slices != old.slices)
    {
        sliceEngine.ResliceCurrentSlot();
        changed = true;
    }
    if(changed)
        playbackEngine.ResyncSlices();

    // BPM change on existing buffer: scale the internal clock period.
    // This effectively speeds up or slows down playback to match.
    // The buffer samples don't change — just the rate we iterate.
    // (No reslice needed — slice boundaries are sample-based.)
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
    active_settings.overdub_feedback = 0.0f;
    active_settings.resample_passes = 0;  // Default: replace
    staged_settings = active_settings;
    sliceEngine.SetSettings(active_settings);

    clock_mode_external = false;
    external_clock_active = false;
    recorded_bpm = 120.0f;
    ext_clock_mult = 1.0f;
    stored_input_vol_ = 1.0f;

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

    // KNOB_1: Play = Crossfader, Config = Input Volume (set and forget)
    float crossfader_pos = 0.5f;  // noon = both channels equal
    float input_vol;
    if(!in_settings_mode)
    {
        // Play mode: KNOB_1 is crossfader (always live)
        crossfader_pos = knob_1_raw;
        // Input volume: use stored value
        input_vol = stored_input_vol_;
    }
    else
    {
        // Config mode: KNOB_1 is input volume (direct, no catch-up)
        if(cfg_moved_k1)
        {
            float iv = knob_1_raw;
            input_vol = (iv < 0.02f) ? 0.0f : iv * iv;
            stored_input_vol_ = input_vol;
        }
        else
        {
            input_vol = stored_input_vol_;
        }
        crossfader_pos = frozen_crossfader;  // Maintain play-mode position
    }

    // ── Mode switch ────────────────────────────────────────
    // Read switch FIRST, but don't act on play_mode transitions
    // until we've handled the settings mode transition.
    PlaybackMode play_mode = ReadSW1();

    // Detect settings transition BEFORE processing play mode
    bool entering_settings = (in_settings_mode && !prev_settings_mode);
    bool leaving_settings  = (!in_settings_mode && prev_settings_mode);

    // Override play_mode while in settings to prevent switch-travel damage
    if(in_settings_mode || entering_settings)
        play_mode = frozen_play_mode;

    playbackEngine.SetPlayMode(play_mode);

    // Only process play_mode transitions when NOT entering/leaving settings
    if(!in_settings_mode && !leaving_settings)
    {
        if(play_mode == PlaybackMode::ONESHOT && prev_play_mode == PlaybackMode::LOOP)
        {
            if(playbackEngine.IsPlaying() && !playbackEngine.IsOneshotDone())
                playbackEngine.SetOneshotDone();
        }
        if(play_mode == PlaybackMode::LOOP && prev_play_mode == PlaybackMode::ONESHOT)
        {
            if(playbackEngine.IsPlaying() && playbackEngine.IsOneshotDone())
                playbackEngine.StartPlayback();
        }
        prev_play_mode = play_mode;
    }

    // ── Mode transitions ───────────────────────────────────
    if(entering_settings)
    {
        staged_settings = active_settings;
        prev_staged_bars   = staged_settings.bars;
        prev_staged_bpm    = staged_settings.bpm;
        prev_staged_slices = staged_settings.slices;
        config_tap_held     = false;
        config_hold_counter = 0;
        config_reset_done   = false;
        frozen_slot = sliceEngine.GetCurrentSlot();
        frozen_play_mode = prev_play_mode;

        // QSPI save happens HERE (config entry) — never during playback.
        // QSPI erase/write blocks all interrupts for 50-200ms.
        // Saving during recording/playback caused the panel freeze bug.
        if(sliceEngine.GetCurrentSlot() == 0)
            save_slot0_pending = true;
        frozen_crossfader = crossfader_pos;

        // Snapshot knob positions — only apply settings if moved
        cfg_snap_k0 = hw.GetKnobValue(DaisyVersio::KNOB_0);
        cfg_snap_k1 = hw.GetKnobValue(DaisyVersio::KNOB_1);
        cfg_snap_k2 = hw.GetKnobValue(DaisyVersio::KNOB_2);
        cfg_snap_k3 = hw.GetKnobValue(DaisyVersio::KNOB_3);
        cfg_snap_k4 = hw.GetKnobValue(DaisyVersio::KNOB_4);
        cfg_snap_k5 = hw.GetKnobValue(DaisyVersio::KNOB_5);
        cfg_moved_k0 = cfg_moved_k1 = cfg_moved_k2 = false;
        cfg_moved_k3 = cfg_moved_k4 = cfg_moved_k5 = false;

        // Playback should continue uninterrupted. No restart needed
        // since ReadSW1 now debounces through CENTER.
    }
    else if(leaving_settings)
    {
        CommitStagedSettings();

        // KNOB_5 returns to swing position with catch-up
        ck_gate.Set(ck_gate.target);

        sliceEngine.SetCurrentSlot(frozen_slot);
        prev_slot = frozen_slot;
        prev_slot_knob = hw.GetKnobValue(DaisyVersio::KNOB_6);

        // Restore catch-up knobs to saved positions
        ck_start.Set(ck_start.target);
        ck_end.Set(ck_end.target);
        ck_speed.Set(ck_speed.target);
        ck_pitch.Set(ck_pitch.target);
        ck_gate.Catch(0.0f);  // Match the frozen default
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
        eff_gate=ck_gate.Update(knob_5_raw);
    }
    else
    {
        // Settings mode: freeze pitch/gate at caught values
        eff_pitch = ck_pitch.target;
        eff_gate  = ck_gate.target;

        // KNOB_1 (bottom-left) = Input Volume in config
        // (handled above in play/config KNOB_1 section)
        if(resample_vis_timer > 0) resample_vis_timer--;

        // KNOB_3 (bottom-center) = reserved
        if(!cfg_moved_k3 && fabsf(knob_3_raw - cfg_snap_k3) > CFG_MOVE_THRESH)
            cfg_moved_k3 = true;

        // KNOB_5 (right-middle) = reserved
        if(!cfg_moved_k5 && fabsf(knob_5_raw - cfg_snap_k5) > CFG_MOVE_THRESH)
            cfg_moved_k5 = true;

        if(effect_vis_timer > 0) effect_vis_timer--;
    }

    // ── Settings mode: edit staged values (NOT live) ───────
    if(in_settings_mode)
    {
        // Check which knobs have moved from snapshot
        if(!cfg_moved_k0 && fabsf(knob_0_raw - cfg_snap_k0) > CFG_MOVE_THRESH)
            cfg_moved_k0 = true;
        if(!cfg_moved_k2 && fabsf(knob_2_raw - cfg_snap_k2) > CFG_MOVE_THRESH)
            cfg_moved_k2 = true;
        if(!cfg_moved_k4 && fabsf(knob_4_raw - cfg_snap_k4) > CFG_MOVE_THRESH)
            cfg_moved_k4 = true;
        if(!cfg_moved_k1 && fabsf(knob_1_raw - cfg_snap_k1) > CFG_MOVE_THRESH)
            cfg_moved_k1 = true;

        // Only update settings for knobs that actually moved
        uint32_t new_bars   = cfg_moved_k0 ? KnobToBars(knob_0_raw)
                                            : staged_settings.bars;
        float new_bpm;
        if(cfg_moved_k2)
        {
            if(clock_mode_external && last_clock_period > 0)
            {
                // External clock: knob = divider/multiplier around noon
                // Noon (0.5) = 1:1. Left = divide (slower). Right = multiply (faster).
                float centered = (knob_2_raw - 0.5f) * 2.0f;
                if(centered > -0.1f && centered < 0.1f)
                    ext_clock_mult = 1.0f;
                else if(centered > 0.0f)
                {
                    float t = (centered - 0.1f) / 0.9f;
                    ext_clock_mult = powf(4.0f, t);  // 1× to 4×
                }
                else
                {
                    float t = (centered + 0.1f) / 0.9f;
                    ext_clock_mult = powf(4.0f, t);  // 1× down to 0.25×
                }
                // BPM display = raw clock × multiplier
                float raw_bpm = (48000.0f * 60.0f) / static_cast<float>(last_clock_period);
                new_bpm = raw_bpm * ext_clock_mult;
            }
            else
                new_bpm = KnobToBPM(knob_2_raw);
        }
        else
            new_bpm = staged_settings.bpm;

        uint32_t new_slices = cfg_moved_k4 ? KnobToSlicesPerBar(knob_4_raw)
                                            : staged_settings.slices;

        // Detect parameter changes → flash all 4 LEDs magenta
        if(new_bars != prev_staged_bars
           || new_slices != prev_staged_slices
           || fabsf(new_bpm - prev_staged_bpm) > 1.0f)
        {
            config_param_flash = 200;
            config_flash_led = 0;  // All 4 magenta
        }

        prev_staged_bars   = new_bars;
        prev_staged_bpm    = new_bpm;
        prev_staged_slices = new_slices;

        staged_settings.bars   = new_bars;
        staged_settings.bpm    = new_bpm;
        staged_settings.slices = new_slices;

        // BPM applies live so you hear tempo changes in real time.
        // (Bars and slices stay staged — they alter the buffer.)
        active_settings.bpm = new_bpm;
    }

    // ── Playback params ────────────────────────────────────
    uint32_t num_slices = playbackEngine.GetNumSlices();
    if(num_slices == 0) num_slices = active_settings.slices;

    uint32_t start_idx = 0;
    uint32_t end_idx   = num_slices > 0 ? num_slices - 1 : 0;

    if(!in_settings_mode)
    {
        start_idx = KnobToSliceIndex(eff_start, num_slices);
        // End knob = slice length (1..num_slices from start)
        uint32_t length = KnobToSliceLength(eff_end, num_slices);
        end_idx = start_idx + length - 1;
        if(end_idx >= num_slices) end_idx = num_slices - 1;

        playbackEngine.SetStartSlice(start_idx);
        playbackEngine.SetEndSlice(end_idx);
        playbackEngine.SetSpeed(eff_speed);

        // BPM-linked speed: buffer stays locked to tempo.
        // Applied AFTER SetSpeed so the knob mapping stays correct.
        float live_bpm = active_settings.bpm;
        if(external_clock_active && last_clock_period > 0)
            live_bpm = (48000.0f * 60.0f) / static_cast<float>(last_clock_period)
                       * ext_clock_mult;

        if(recorded_bpm > 1.0f)
            playbackEngine.SetBpmRatio(live_bpm / recorded_bpm);
    }

    playbackEngine.SetPitch(eff_pitch);

    // Swing: KNOB_5 in play mode (far left = straight, far right = heavy)
    playbackEngine.SetSwing(eff_gate);

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

        // Crossfader vis: KNOB_1 movement in play mode
        if(fabsf(knob_1_raw - prev_knob_gate) > 0.01f)
        {
            xfade_vis_timer = KNOB_VIS_DURATION;
            prev_knob_gate = knob_1_raw;
        }
        if(xfade_vis_timer > 0) xfade_vis_timer--;
    }

    // ── Slot ───────────────────────────────────────────────
    if(in_settings_mode)
    {
        // KNOB_6 in settings: freed (was crossfader/gate, now in play effect)
    }
    else
    {
        // Hysteresis: only switch if knob moved >3% from last switch point
        if(fabsf(knob_6_raw - prev_slot_knob) > 0.06f)
        {
            uint32_t new_slot = GetSlotFromKnob(knob_6_raw);
            if(new_slot != prev_slot)
            {
                sliceEngine.SetCurrentSlot(new_slot);
                threshold_rearm = false;
                ledManager.ShowSlotDisplay();
                prev_slot = new_slot;
                prev_slot_knob = knob_6_raw;
            }
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

            // Hold 2s = toggle internal/external clock
            if(!config_reset_done &&
               config_hold_counter >= CONFIG_RESET_THRESHOLD)
            {
                clock_mode_external = !clock_mode_external;
                ext_clock_mult = 1.0f;
                ext_clock_samples = 0;
                config_reset_done = true;  // Only toggle once per hold
            }
        }
        else if(config_tap_held)
        {
            // Button released in config mode
            config_tap_held = false;

            // Short tap: tap tempo (3 taps sets BPM)
            // Disabled in external clock mode — clock comes from jack
            if(config_hold_counter < CONFIG_WARN_THRESHOLD && !config_reset_done
               && !clock_mode_external)
            {
                // Blue flash on all 4 LEDs for each tap
                config_param_flash = 80;
                config_flash_led = 99;  // Special: all-blue tap tempo

                tap_tempo_count++;
                if(tap_tempo_count == 1)
                {
                    tap_tempo_samples = 0;
                    tap_tempo_timeout = 48000;  // 1s timeout
                }
                else if(tap_tempo_count == 2)
                {
                    tap_tempo_prev = tap_tempo_samples;
                    tap_tempo_samples = 0;
                    tap_tempo_timeout = 48000;
                }
                else if(tap_tempo_count >= 3)
                {
                    // Average the two intervals
                    uint32_t avg = (tap_tempo_prev + tap_tempo_samples) / 2;
                    if(avg > 0)
                    {
                        float new_bpm = (48000.0f * 60.0f) / static_cast<float>(avg);
                        if(new_bpm >= 40.0f && new_bpm <= 200.0f)
                        {
                            staged_settings.bpm = new_bpm;
                            active_settings.bpm = new_bpm;
                            prev_staged_bpm = new_bpm;
                        }
                    }
                    tap_tempo_count = 0;
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
                    playbackEngine.ResetPitchShifters();
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
                                threshold_rearm = false;
                }
                else if(st == RecordState::PLAYING)
                {
                    if(play_mode == PlaybackMode::ONESHOT
                       && playbackEngine.IsOneshotDone())
                    {
                        playbackEngine.StartPlayback();
                    }
                    else if(GetRecordMode() == RecordMode::THRESHOLD)
                    {
                        // Threshold: arm for re-record while still playing.
                        // Don't call ArmRecording (destroys header/stops playback).
                        // Just set flag; audio callback checks threshold while playing.
                        threshold_rearm = !threshold_rearm;
                        threshold_grace = 9600;
                    }
                    else if(GetRecordMode() == RecordMode::IMMEDIATE)
                    {
                        // Instant mode with content: use resample mode
                        if(active_settings.resample_passes == 0)
                        {
                            // Replace: stop, clear, re-record
                            playbackEngine.StopPlayback();
                            {
                                uint32_t ns = active_settings.slices;
                                uint32_t si = KnobToSliceIndex(eff_start, ns);
                                uint32_t len = KnobToSliceLength(eff_end, ns);
                                uint32_t ei = si + len - 1;
                                if(ei >= ns) ei = ns - 1;
                                sliceEngine.SetRecordRange(si, ei);
                            }
                            sliceEngine.ArmRecording(RecordMode::IMMEDIATE);
                            ApplyRecordDefaults();
                        }
                        else
                        {
                            // Frippertronics or unity: overdub
                            sliceEngine.StartOverdub();
                        }
                    }
                    else
                    {
                        // Clock-sync: normal overdub
                        sliceEngine.StartOverdub();
                    }
                }
                else if(st == RecordState::OVERDUBBING)
                {
                    // Stop overdubbing → back to normal playback
                    sliceEngine.StopOverdub();
                    auto_overdub = false;
                    // Save deferred to config entry
                }
                else if(st == RecordState::STOPPED ||
                        st == RecordState::EMPTY)
                {
                    // Empty/stopped → arm new recording
                    playbackEngine.StopPlayback();
                    {
                        uint32_t ns = active_settings.slices;
                        uint32_t si = KnobToSliceIndex(eff_start, ns);
                        uint32_t len = KnobToSliceLength(eff_end, ns);
                        uint32_t ei = si + len - 1;
                        if(ei >= ns) ei = ns - 1;
                        sliceEngine.SetRecordRange(si, ei);
                    }
                    sliceEngine.ArmRecording(GetRecordMode());
                    ApplyRecordDefaults();
                }
                else if(st == RecordState::ARMED)
                {
                    // ARMED -> cancel, go back to previous state
                    const SlotHeader& hdr = sliceEngine.GetHeader();
                    if(hdr.has_content && hdr.total_samples > 0)
                    {
                        sliceEngine.CancelArm();
                        playbackEngine.StartPlayback();
                    }
                    else
                    {
                        sliceEngine.CancelArm();
                    }
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
        recorded_bpm = active_settings.bpm;
        playbackEngine.ResetPitchShifters();
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
        threshold_rearm = false;

        // Save deferred to config entry
    }
    was_recording = (current_state == RecordState::RECORDING);

    // ── Internal clock (uses ACTIVE settings, not staged) ──
    uint32_t int_clock_period = sliceEngine.GetInternalClockPeriod();

    // Cache per-block values to avoid repeated switch/function reads
    RecordMode cached_record_mode = GetRecordMode();

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

        // == Clock: simple trigger-to-trigger measurement ==
        // Each rising edge = one beat (MultiVersio style).
        // No PPQN counting. Whatever your clock sends, we use.
        // Clock divider knob in config scales it.
        if(gate_rising)
        {
            if(ext_clock_samples > 100)
                last_clock_period = ext_clock_samples;
            ext_clock_samples = 0;
        }
        else
        {
            ext_clock_samples++;
        }
        RecordState st_now = sliceEngine.GetState();

        if(st_now == RecordState::ARMED || st_now == RecordState::RECORDING)
        {
            bool got_tick = false;
            uint32_t tick_period = int_clock_period;

            if(clock_mode_external && gate_rising)
            {
                got_tick = true;
                tick_period = last_clock_period > 100
                            ? last_clock_period : int_clock_period;
                external_clock_active = true;
                ext_clock_timeout = 0;
            }
            else if(!clock_mode_external)
            {
                // Internal clock mode
                internal_clock_counter++;
                if(internal_clock_counter >= int_clock_period)
                {
                    internal_clock_counter = 0;
                    got_tick = true;
                }
            }
            else
            {
                // External mode but no edge this sample
                ext_clock_timeout++;
                if(ext_clock_timeout >= EXT_CLOCK_TIMEOUT)
                    external_clock_active = false;
            }

            if(got_tick)
                sliceEngine.OnClockTick(tick_period);
        }
        else if(st_now == RecordState::PLAYING ||
                st_now == RecordState::OVERDUBBING)
        {
            // Track clock during playback for BPM-linked speed
            if(clock_mode_external && gate_rising)
            {
                external_clock_active = true;
                ext_clock_timeout = 0;
            }
            else if(clock_mode_external)
            {
                ext_clock_timeout++;
                if(ext_clock_timeout >= EXT_CLOCK_TIMEOUT)
                {
                    // Clock lost — stop playback in external mode
                    external_clock_active = false;
                    if(play_mode == PlaybackMode::LOOP)
                        playbackEngine.StopPlayback();
                }
            }

            // Internal clock always runs during playback for timing
            internal_clock_counter++;
            if(internal_clock_counter >= int_clock_period)
                internal_clock_counter = 0;
        }
        else if(st_now == RecordState::STOPPED)
        {
            if(clock_mode_external && gate_rising && play_mode == PlaybackMode::LOOP)
            {
                const SlotHeader& hdr = sliceEngine.GetHeader();
                if(hdr.num_slices > 0 && hdr.total_samples > 0)
                {
                    playbackEngine.StartPlayback();
                    external_clock_active = true;
                    ext_clock_timeout = 0;
                    ext_beat_count = 0;
                }
            }
        }
        else
        {
            if(clock_mode_external && gate_rising)
            { external_clock_active = true; ext_clock_timeout = 0; }
        }
        // ── Tap tempo counter (runs during config) ───
        if(tap_tempo_count > 0 && tap_tempo_count < 3)
        {
            tap_tempo_samples++;
            if(tap_tempo_timeout > 0)
                tap_tempo_timeout--;
            else
                tap_tempo_count = 0;  // Timed out, reset
        }

        // ── Threshold ──────────────────────────────────
        if(st_now == RecordState::ARMED &&
           cached_record_mode == RecordMode::THRESHOLD)
        {
            if(threshold_grace > 0)
                threshold_grace--;
            else if(sliceEngine.CheckThreshold(in_l, in_r))
            {
                sliceEngine.OnThresholdExceeded();
                internal_clock_counter = 0;
            }
        }

        // ── Threshold re-arm while playing ─────────────
        if(threshold_rearm
           && st_now == RecordState::PLAYING)
        {
            if(threshold_grace > 0)
                threshold_grace--;
            else if(sliceEngine.CheckThreshold(in_l, in_r))
            {
                threshold_rearm = false;
                sliceEngine.StartOverdub();
                auto_overdub = true;
                overdub_sample_count = 0;
                overdub_start_slice = playbackEngine.GetCurrentSlice();
            }
        }

        // ── Recording ──────────────────────────────────
        if(st_now == RecordState::RECORDING)
            sliceEngine.RecordSample(in_l, in_r);

        // ── Output ─────────────────────────────────────
        RecordState current = st_now;

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

            // Auto-stop threshold overdub after one full loop
            if(auto_overdub && current == RecordState::OVERDUBBING)
            {
                overdub_sample_count++;
                if(overdub_sample_count > 4096)
                {
                    uint32_t cur_sl = playbackEngine.GetCurrentSlice();
                    if(cur_sl == overdub_start_slice
                       && playbackEngine.GetSliceProgress() < 0.1f)
                    {
                        sliceEngine.StopOverdub();
                        auto_overdub = false;
                        overdub_sample_count = 0;
                        // Save deferred to config entry
                    }
                }
            }

            // Crossfader: always active, reads from KNOB_1.
            // Left = input only. Center = both. Right = buffer only.
            {
                float pos = crossfader_pos;
                if(pos < 0.05f) pos = 0.0f;
                if(pos > 0.95f) pos = 1.0f;
                float gain_a, gain_b;
                if(pos <= 0.5f) {
                    gain_a = 1.0f;
                    gain_b = pos * 2.0f;
                } else {
                    gain_a = (1.0f - pos) * 2.0f;
                    gain_b = 1.0f;
                }
                mix_l = HardLimit(in_l * gain_a + play_l * gain_b);
                mix_r = HardLimit(in_r * gain_a + play_r * gain_b);
            }
        }
        else
        {
            // Not playing: crossfader still applies to input
            {
                float pos = crossfader_pos;
                if(pos < 0.05f) pos = 0.0f;
                if(pos > 0.95f) pos = 1.0f;
                float gain_a = (pos <= 0.5f) ? 1.0f : (1.0f - pos) * 2.0f;
                mix_l = in_l * gain_a;
                mix_r = in_r * gain_a;
            }
        }

        if(startup_mute > 0)
        {
            out[0][i] = 0.0f;
            out[1][i] = 0.0f;
            startup_mute--;
        }
        else
        {
            out[0][i] = mix_l;
            out[1][i] = mix_r;


        }
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
            // Clock toggled — bright blue flash
            float blink = ((config_hold_counter / 9600) % 2 == 0) ? 1.0f : 0.3f;
            hw.SetLed(0, 0.0f, 0.0f, blink);
            hw.SetLed(1, 0.0f, 0.0f, blink);
            hw.SetLed(2, 0.0f, 0.0f, blink);
            hw.SetLed(3, 0.0f, 0.0f, blink);
        }
        else if(config_tap_held &&
                config_hold_counter >= CONFIG_WARN_THRESHOLD)
        {
            // Approaching clock toggle — blue ramp
            float hold_frac = static_cast<float>(
                config_hold_counter - CONFIG_WARN_THRESHOLD)
                / static_cast<float>(
                    CONFIG_RESET_THRESHOLD - CONFIG_WARN_THRESHOLD);
            float v = 0.3f + hold_frac * 0.7f;
            hw.SetLed(0, 0.0f, 0.0f, v);
            hw.SetLed(1, 0.0f, 0.0f, v);
            hw.SetLed(2, 0.0f, 0.0f, v);
            hw.SetLed(3, 0.0f, 0.0f, v);
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
            // Config: show WHITE playhead from playback engine
            // so user can see loop position while tweaking settings
            RecordState cfg_led_state = sliceEngine.GetState();
            bool cfg_playing = (cfg_led_state == RecordState::PLAYING ||
                                cfg_led_state == RecordState::OVERDUBBING);
            if(cfg_playing && playbackEngine.IsPlaying()
               && !playbackEngine.IsOneshotDone())
            {
                float cfg_prog = playbackEngine.GetPlaybackProgress();
                uint32_t ns = playbackEngine.GetNumSlices();
                uint32_t rs = playbackEngine.GetStartSlice();
                uint32_t re = playbackEngine.GetEndSlice();
                if(re > 0) re--;
                float norm_s = (ns > 0) ? static_cast<float>(rs) / static_cast<float>(ns) : 0.0f;
                float norm_e = (ns > 0) ? static_cast<float>(re + 1) / static_cast<float>(ns) : 1.0f;
                if(norm_e > 1.0f) norm_e = 1.0f;
                float abs_pos = norm_s + cfg_prog * (norm_e - norm_s);
                int head_q = static_cast<int>(abs_pos * 4.0f);
                if(head_q > 3) head_q = 3;
                if(head_q < 0) head_q = 0;

                for(uint32_t l = 0; l < 4; l++)
                {
                    if(static_cast<int>(l) == head_q)
                    {
                        if(clock_mode_external)
                            hw.SetLed(l, 0.0f, 0.0f, 1.0f);   // Bright blue
                        else
                            hw.SetLed(l, 0.8f, 0.8f, 0.8f);   // Bright white
                    }
                    else
                    {
                        if(clock_mode_external)
                            hw.SetLed(l, 0.02f, 0.02f, 0.08f); // Dim blue
                        else
                            hw.SetLed(l, 0.05f, 0.05f, 0.05f); // Dim white
                    }
                }
            }
            else
            {
                for(uint32_t l = 0; l < 4; l++)
                {
                    if(clock_mode_external)
                        hw.SetLed(l, 0.02f, 0.02f, 0.08f);  // Dim blue
                    else
                        hw.SetLed(l, 0.05f, 0.05f, 0.05f);  // Dim white
                }
            }

            // Purple parameter flash overlays
            if(config_param_flash > 0 && config_flash_led == 99)
            {
                // Tap tempo: all 4 LEDs blue
                for(uint32_t l = 0; l < 4; l++)
                    hw.SetLed(l, 0.0f, 0.0f, 0.7f);
            }
            else if(config_param_flash > 0 && config_flash_led < 4)
            {
                // ALL 4 LEDs flash magenta for any top-row param change
                for(uint32_t l = 0; l < 4; l++)
                    hw.SetLed(l, 0.5f, 0.0f, 0.5f);
            }

            // Resample mode vis overlay (KNOB_1 moving)

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
                          cyan_flash, start_idx, oneshot_waiting,
                          threshold_rearm);

        for(uint32_t l = 0; l < 4; l++)
        {
            const LedManager::LedColor& c = ledManager.GetLed(l);
            hw.SetLed(l, c.r, c.g, c.b);
        }

    }

    // Crossfader position LED overlay (white indicator while moving KNOB_1)
    if(!in_settings_mode && xfade_vis_timer > 0
       && !ledManager.IsSlotDisplayActive())
    {
        float pos = knob_1_raw;
        float fade = (xfade_vis_timer > 100) ? 1.0f
                     : static_cast<float>(xfade_vis_timer) / 100.0f;

        // Position indicator: active LED bright, others off
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f;
        if(pos < 0.2f)
            b0 = 0.9f;
        else if(pos < 0.4f)
            b1 = 0.9f;
        else if(pos < 0.6f)
        { b1 = 0.9f; b2 = 0.9f; }
        else if(pos < 0.8f)
            b2 = 0.9f;
        else
            b3 = 0.9f;
        hw.SetLed(0, b0*fade, b0*fade, b0*fade);
        hw.SetLed(1, b1*fade, b1*fade, b1*fade);
        hw.SetLed(2, b2*fade, b2*fade, b2*fade);
        hw.SetLed(3, b3*fade, b3*fade, b3*fade);
    }

    // LED colors set above; hw.UpdateLeds() runs in main loop at ~1kHz
    // (fixes dim LED flicker caused by 375Hz refresh in audio callback)
    if(config_param_flash > 0) config_param_flash--;
}


int main(void)
{
    hw.Init(true);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(256);

    float sample_rate = hw.AudioSampleRate();


    // ── Startup LED animation ──────────────────────────────
    // Outside-in to solid white, then fade out.
    // Multiple UpdateLeds calls per step to ensure hardware registers.
    for(int r = 0; r < 5; r++)
    {
        hw.SetLed(0, 0, 0, 0); hw.SetLed(1, 0, 0, 0);
        hw.SetLed(2, 0, 0, 0); hw.SetLed(3, 0, 0, 0);
        hw.UpdateLeds(); hw.DelayMs(20);
    }
    hw.DelayMs(100);

    // Step 1: Outer LEDs solid white
    for(int r = 0; r < 10; r++)
    {
        hw.SetLed(0, 0.7f, 0.7f, 0.7f); hw.SetLed(1, 0, 0, 0);
        hw.SetLed(2, 0, 0, 0);           hw.SetLed(3, 0.7f, 0.7f, 0.7f);
        hw.UpdateLeds(); hw.DelayMs(25);
    }

    // Step 2: Inner LEDs join — all 4 solid white
    for(int r = 0; r < 10; r++)
    {
        hw.SetLed(0, 0.7f, 0.7f, 0.7f); hw.SetLed(1, 0.7f, 0.7f, 0.7f);
        hw.SetLed(2, 0.7f, 0.7f, 0.7f); hw.SetLed(3, 0.7f, 0.7f, 0.7f);
        hw.UpdateLeds(); hw.DelayMs(25);
    }

    // Step 3: Brighten
    for(int r = 0; r < 12; r++)
    {
        hw.SetLed(0, 1, 1, 1); hw.SetLed(1, 1, 1, 1);
        hw.SetLed(2, 1, 1, 1); hw.SetLed(3, 1, 1, 1);
        hw.UpdateLeds(); hw.DelayMs(25);
    }

    // Step 4: Slow fade out
    for(int s = 20; s >= 0; s--)
    {
        float v = static_cast<float>(s) / 20.0f;
        hw.SetLed(0, v, v, v); hw.SetLed(1, v, v, v);
        hw.SetLed(2, v, v, v); hw.SetLed(3, v, v, v);
        hw.UpdateLeds(); hw.DelayMs(40);
    }
    hw.DelayMs(100);

    active_settings.bars   = 1;
    active_settings.slices = 16;
    active_settings.bpm    = 120.0f;
    active_settings.overdub_feedback = 0.0f;
    active_settings.resample_passes = 0;
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
        if(persistManager.HasLoadedSettings())
        {
            active_settings = persistManager.GetLoadedSettings();
            staged_settings = active_settings;
            sliceEngine.SetSettings(active_settings);
            prev_staged_bars   = active_settings.bars;
            prev_staged_slices = active_settings.slices;
            prev_staged_bpm    = active_settings.bpm;
            recorded_bpm       = active_settings.bpm;
        }
        playbackEngine.StartPlayback();
    }

    ck_start.Catch(0.0f);
    ck_speed.Catch(0.5f);
    ck_pitch.Catch(0.5f);
    ck_end.Catch(1.0f);
    ck_gate.Catch(0.0f);   // Swing: 0 = straight
    stored_input_vol_ = 1.0f;

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    for(;;)
    {
        if(save_slot0_pending && !persistManager.IsSaving())
        {
            save_slot0_pending = false;
            persistManager.StartSave();
        }
        persistManager.Tick();
        hw.UpdateLeds();
        hw.DelayMs(1);
    }
}
