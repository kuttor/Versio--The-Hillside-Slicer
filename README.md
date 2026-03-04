# THE HILLSIDE SLICER

**Custom beat-slicing firmware for the Daisy Versio (Electrosmith)**
**Version 0.5-alpha**

---

## What Is It

The Hillside Slicer turns a Daisy Versio into a stereo beat slicer. Record a loop, slice it into segments, then manipulate those slices with independent time-stretching and pitch-shifting. Designed for live performance with external clock sync, 8 sample slots, and a single-knob-per-function interface.

Built on the Daisy Versio hardware platform (STM32H750, Cortex-M7, 96kHz stereo audio).

---

## Hardware Panel Map

The firmware maps to any Versio faceplate. Reference below uses Electrosmith's standard labeling.

```
            +----------------------------+
            |       DAISY VERSIO         |
            |                            |
  KNOB_0    |  [START]       [SPEED]     |  KNOB_2
  top-left  |                            |  top-center
            |                            |
  KNOB_1    |  [IN VOL]      [PITCH]     |  KNOB_3
  bot-left  |                            |  bot-center
            |                            |
            |            [END]           |  KNOB_4
            |            top-right       |
            |                            |
  SW_0      |  [ABC]   [GATE]   [SLOT]   |  KNOB_6
  left      |          right-mid         |  bot-right
            |                            |
  SW_1      |  [XYZ]                     |  KNOB_5
  right     |                            |  right-mid
            |                            |
            |  [TAP]                     |
            |                            |
            |  IN L  IN R  OUT L  OUT R  |
            +----------------------------+
```

### Controls -- Normal Mode

| Control | Function | Range / Behavior |
|---------|----------|-----------------|
| KNOB_0 (top-left) | **Start Slice** | Selects which slice playback starts from. Sweeps across all available slices. |
| KNOB_1 (bottom-left) | **Input Volume** | Controls incoming signal level for both monitoring and recording. Squared taper, 2% floor = silence. |
| KNOB_2 (top-center) | **Speed** | Time-stretch. Noon = 1x. CW = faster (up to 4x). CCW = slower (down to 0.25x). Dead zone at noon (0.45-0.55). |
| KNOB_3 (bottom-center) | **Pitch** | Granular pitch shift, quantized to integer semitones. Noon = unity. CW = +1 to +12 semitones. CCW = -1 to -24 semitones. Dead zone at noon (~8%). |
| KNOB_4 (top-right) | **End Slice** | Selects which slice playback ends at. Combined with Start, defines the active playback range. |
| KNOB_5 (right-middle) | **Slice Gate** | Controls how much of each slice plays before going silent. Full CW = 100% (full slice). CCW = shorter gate for rhythmic gating. |
| KNOB_6 (bottom-right) | **Slot Select** | Selects active sample slot (1-8). Switching slots is instant. |
| SW_0 (ABC) | **Record Mode** | A = Immediate, B = Clock Sync, C = Threshold |
| SW_1 (XYZ) | **Play Mode / Settings** | X = Loop, Y = OneShot, Z = Settings Page |
| TAP button | **Record / Stop / Clear** | Tap = toggle record/stop. Hold 1.2s = clear current slot. |
| GATE jack | **24PPQN Clock Input** | External clock sync. 24 pulses per quarter note. Auto-detects; falls back to internal clock after 2 seconds of no signal. |

### Controls -- Settings Mode (SW_1 = Z position)

Settings are **staged**: changes are shown on LEDs but only applied when you flip SW_1 back to Loop or OneShot. Audio continues uninterrupted while adjusting.

**Exception:** Output volume (KNOB_1 in settings) is real-time, not staged.

| Control | Settings Function | Range |
|---------|-------------------|-------|
| KNOB_0 | **Bars** | 1, 2, 3, or 4 bars |
| KNOB_1 | **Merge Destination Slot** / **Output Volume** | Slot 1-8 (for merge). Also controls end-of-chain output volume in real-time. |
| KNOB_2 | **BPM** or **Clock Divider** | Internal: 40-200 BPM in steps of 5. When external clock detected: divider /1, /2, /4, /8. |
| KNOB_3 | **Merge Source Slot** | Slot 1-8 (source for buffer merge) |
| KNOB_4 | **Slices** | 4, 8, 16, 32, or 64 slices per recording |
| KNOB_5 | **Overdub Feedback** | Full left = 1.0 (pure sum/overdub). Full right = 0.0 (full replace). |

---

## Workflow

### Basic Recording and Playback

1. Set SW_1 to **Loop** (X position).
2. Set SW_0 to your preferred recording trigger:
   - **A (Immediate):** Recording starts the instant you press TAP.
   - **B (Clock Sync):** Recording arms on TAP, starts on the next clock tick (internal or external).
   - **C (Threshold):** Recording arms on TAP, starts when incoming audio exceeds the detection threshold (~-36dB). 100ms pre-roll captures the transient that triggered it.
3. Press **TAP** to begin recording.
4. Recording length is determined by your BPM and Bars settings. At the default 120 BPM and 1 bar (16 clock ticks), recording is 2 seconds.
5. Recording auto-stops when the bar count is reached. You can also tap again to stop manually.
6. Playback begins immediately after recording stops.
7. Use Start and End knobs to select a slice range. Use Speed and Pitch to manipulate playback.

### What Happens to Knobs When Recording Starts

- **Pitch** and **Speed** snap to their defaults (noon = unity). They stay frozen at default until you physically move the knob more than 3% from its current physical position. The first movement activates the knob instantly at whatever position it's at -- no catch-up hunting.
- **Start** and **End** read their current physical position directly. Wherever the knobs are sitting when you hit record is where the slice range is set. No reset, no catch-up.
- **Gate** resets to fully open (1.0).

### OneShot Mode

1. Flip SW_1 to **Y (OneShot)**.
2. Playback stops. LEDs show dim cyan on the active slice range, indicating the slicer is armed and waiting.
3. Press **TAP** to trigger playback. Audio plays once through the Start-to-End slice range and stops.
4. LEDs return to dim cyan. Press TAP again to retrigger.

### Slot Management

- 8 independent stereo sample slots stored in SDRAM.
- Each slot holds up to approximately 9.4 seconds at 96kHz stereo.
- KNOB_6 switches between slots instantly. A brief LED overlay shows all 8 slot states (filled vs. empty, which is selected).
- **Slot 1 (index 0) auto-saves to QSPI flash** after recording finishes. This slot survives power cycles and is restored on boot.
- All other slots (2-8) are volatile and lost on power off.

### Overdub

1. While a slot is playing, press **TAP** to enter overdub mode.
2. LEDs turn red (with glow trail). Incoming audio is written to the buffer at the current playback position.
3. Press **TAP** again to exit overdub and return to normal playback.
4. The overdub feedback setting (KNOB_5 in settings mode) controls the mix: full left = pure sum (new audio added on top), full right = full replace (new audio overwrites existing).
5. If slot 1 was overdubbed, it auto-saves to flash.

### Buffer Merge

Merges one slot's audio into another (destructive operation):

1. Enter Settings mode (SW_1 = Z).
2. Set KNOB_1 to the **destination** slot and KNOB_3 to the **source** slot.
3. Triple-tap TAP to execute:
   - **Tap 1:** LEDs show dim solid red (2-second timeout to continue or cancel).
   - **Tap 2:** LEDs show bright solid red (2-second timeout).
   - **Tap 3:** Merge executes. Source audio is summed sample-by-sample into the destination buffer.
4. White LED flash confirms success.
5. If source is longer than destination, destination extends to match. Result is automatically re-sliced.

### Clearing a Slot

- In normal mode, **hold TAP for 1.2 seconds**.
- LEDs sweep red right-to-left as a visual countdown.
- On release, the current slot's audio is erased and playback stops.
- Pitch shifter buffers are also cleared to prevent leftover artifacts.

### Full Module Reset

- In **Settings mode**, hold TAP for 2 seconds.
- At 1.5s the LEDs flash red as a warning.
- At 2.0s all 8 slots are cleared and config resets to defaults (1 bar, 16 slices, 120 BPM).

### Bar Extension

If you increase the Bars setting in settings mode (e.g., 1 bar to 2 bars), the existing recording is extended with silence to fill the new length, then re-sliced. This lets you grow a loop after the fact.

---

## External Clock (24PPQN)

The **Gate jack** accepts standard 24 pulses-per-quarter-note clock (the same rate as MIDI clock).

**Important:** The clock must be on the Gate jack (the digital input), not on any CV jack. The Versio's ADC inputs have lowpass filtering that destroys clock pulses.

### How It Works

The firmware counts rising edges on the Gate jack. Every 24 edges = 1 beat. BPM is derived from the time elapsed between beats. A bar boundary is calculated from your Bars setting (e.g., 4 beats x 1 bar = 4 beats per bar).

### Clock Behavior by State

| State | What Happens |
|-------|-------------|
| **Recording** | External beat ticks replace the internal clock for timing the recording. Recording length syncs to the external tempo. |
| **Playing (Loop mode)** | Bar ticks reset the playhead to the start of the loop. This keeps the slicer locked to your sequencer's bar boundaries. |
| **Stopped with content** | The first beat tick auto-starts loop playback and resets the bar counter. Start your sequencer and the slicer starts with it. |
| **No clock for 2 seconds** | Auto-fallback to internal clock. Playback continues free-running. |

### Clock Divider

When external clock is detected, the BPM knob (KNOB_2) in settings mode becomes a clock divider:

| Knob Position | Divider | Effect |
|--------------|---------|--------|
| Left quarter | /1 | Bar tick every bar |
| Center-left | /2 | Bar tick every 2 bars |
| Center-right | /4 | Bar tick every 4 bars |
| Right quarter | /8 | Bar tick every 8 bars |

This controls how often the playhead resets, effectively extending the loop period relative to the external clock.

---

## Play Modes

### Loop (SW_1 = X)

- Playback loops continuously through the Start-to-End slice range.
- External clock bar ticks reset the playhead for sync.
- TAP during playback enters overdub. TAP again exits.
- Switching from OneShot to Loop auto-resumes playback if it was stopped.

### OneShot (SW_1 = Y)

- Plays through the slice range once, then stops.
- LEDs show dim cyan on the active range while waiting for trigger.
- TAP fires playback. When playback finishes, returns to waiting state.
- Switching from Loop to OneShot stops playback immediately and enters waiting.

---

## DSP Architecture

### Pitch Shifter

4-grain OLA (Overlap-Add) time-domain granular pitch shifter. No FFT.

- 4 grains at 25% phase offset. Hann windows sum to exactly 2.0 at every sample point; output normalized by 0.5 for unity gain.
- Grain size: 4096 samples (~42ms at 96kHz).
- Circular buffer: 16384 samples per channel (~170ms).
- Unity bypass when pitch factor is within 1% of 1.0 (avoids processing cost and artifacts at noon).
- Pitch range: factor 0.25 to 2.0 (corresponds to -24 to +12 semitones).
- Quantized to integer semitones via `roundf()`.
- Output hard-clamped to +/-1.0.
- CPU cost: less than 1% per channel.

### Time Stretcher

Tape-style speed control integrated into the grain engine's playback read position.

- Dead zone at noon (0.45-0.55 normalized = exactly 1.0x).
- Outside dead zone: exponential mapping via `pow(2, mapped)` where mapped is -2.0 to +2.0.
- Effective range: 0.25x to 4.0x speed.
- Speed and pitch are independent. Speed changes playback rate without affecting pitch. Pitch changes pitch without affecting playback rate.

### Slice Gate

Controls what fraction of each slice is audible.

- Full CW = entire slice plays.
- CCW = progressively shorter gate (rhythmic gating effect).
- 384-sample fade (~4ms) on gate close to prevent clicks.
- State-tracked: smooth fade-out on close, instant open on open.

### Crossfading

Adaptive crossfade at slice boundaries and playback transitions.

- Maximum crossfade: 1024 samples (~10.7ms at 96kHz).
- Adaptive length: never exceeds 12% of slice length (keeps short slices clean).
- Minimum: 16 samples (always click-free).
- Applied at: slice advance, start/end knob changes, playhead resets from external clock.

---

## LED Feedback

4 RGB LEDs provide state feedback.

| State | LED Behavior |
|-------|-------------|
| Empty | All off |
| Armed | Steady warm orange (all 4 LEDs) |
| Recording | Red fill sweeps left-to-right tracking record progress |
| Playing (Loop) | Green playhead sweeps across the active range. Dim green background on in-range LEDs. Out-of-range LEDs off. |
| OneShot Waiting | Dim cyan on in-range LEDs |
| Overdubbing | Red playhead with glow trail (same sweep pattern as playing) |
| Stopped | Dim green on in-range LEDs |
| Slot Switch | Brief overlay: all 8 slots mapped to 4 LEDs (blue = even slots, purple = odd, bright = selected) |
| Clear Sweep | Red sweep right-to-left while holding TAP |
| Config Flash | Brief purple flash on the LED corresponding to the changed parameter |
| Save in Progress | LED 0 dim white |
| Save Complete | LED 0 bright green |

The LED system uses a priority stack: Clear Sweep (highest) > Slot Display > Normal state rendering (lowest). Higher-priority states override lower ones.

---

## Persistence

- **Slot 1 only** (index 0) saves to 8MB QSPI flash.
- Triggered automatically after recording finishes or after a merge into slot 1.
- Full 32-bit float audio quality preserved (no compression or bit reduction).
- Background save: writes in small chunks so audio DMA continues uninterrupted. QSPI and SDRAM are on separate buses.
- Restored automatically on power-up.
- LED 0 shows dim white during save, bright green flash when complete.

---

## Memory Layout

| Resource | Size | Usage |
|----------|------|-------|
| SDRAM | ~14.4 MB | 8 stereo sample buffers (900K samples x 8 slots x 2 ch x 4 bytes) |
| SDRAM | ~75 KB | Pre-roll buffers (9600 samples x 2 ch x 4 bytes) |
| SRAM | ~128 KB | Pitch shifter circular buffers (16384 x 2 instances x 4 bytes) |
| SRAM | ~30 KB | Engine state, slice headers, LED state, clock tracking, catch-up knobs |
| QSPI Flash | up to ~7.2 MB | Slot 0 persistence (4KB header + stereo audio) |

Total SRAM usage: approximately 30% of 512KB.

---

## Build

Requires the [DaisyExamples](https://github.com/electro-smith/DaisyExamples) environment with libDaisy and DaisySP compiled for the Versio platform.

```bash
# From the HillsideSlicer directory:
make

# Flash via USB (device in DFU mode):
make program-dfu
```

### Source Files

| File | Purpose |
|------|---------|
| `HillsideSlicer.cpp` | Main firmware: audio callback, control routing, state machine, settings page |
| `slice_engine.h` | Recording engine, slice math, overdub, buffer merge, bar extension |
| `grain_engine.h` | Playback engine: tape-style speed, slice gate with fade, crossfading, pitch shift integration |
| `pitch_shifter.h` | 4-grain OLA granular pitch shifter |
| `led_manager.h` | LED rendering for all states with priority system |
| `gate_detect.h` | Gate/CV edge detection, clock tracking, 24PPQN clock detector |
| `persistence_manager.h` | QSPI flash save/restore for slot 0 |
| `lofi.h` | Lo-fi effect module (present but not currently wired into signal chain) |
| `Makefile` | Build configuration for arm-none-eabi toolchain |

---

## Version History

### 0.5-alpha (current)

- External 24PPQN clock on Gate jack with auto-detect and 2-second fallback to internal clock.
- Bar-sync playback: external clock resets playhead on bar boundaries in loop mode.
- Auto-start: clock arriving while stopped with content triggers loop playback.
- Clock divider in settings mode (BPM knob becomes /1, /2, /4, /8 selector when externally clocked).
- OneShot mode: playback stops on mode switch, shows dim cyan LEDs while waiting, TAP retriggers.
- Clean OneShot-to-Loop and Loop-to-OneShot transitions.
- First-move-activates for speed/pitch knobs on record start (frozen at default until moved 3%).
- Start/End knobs use physical position on record arm (no reset, no catch-up).
- Threshold recording sensitivity lowered to ~-36dB (was ~-26dB).
- Single-slice LED display fix (at least 1 LED lights when playing only the first slice).

### 0.4-alpha

- 4-grain pitch shifter replacing 2-grain OLA (eliminates tremolo at high pitch factors).
- Increased crossfade to 1024 samples and gate fade to 384 samples for cleaner transitions.
- Output volume control in settings mode (KNOB_1, real-time).
- Grain size doubled to 4096 samples for smoother pitch shifting.

### 0.3-alpha

- Granular pitch shifter (replaced earlier WSOLA and phase vocoder attempts).
- Adaptive crossfade system at slice boundaries.
- ResyncSlices for live slice reconfiguration.
- Catch-up knobs on mode transitions.
- LED glow trail system for playback and overdub visualization.
- QSPI persistence for slot 0.
- Buffer merge with triple-tap confirmation.
- Overdub with adjustable feedback.
- Settings page with staged parameters.
- Bar extension (increase bars setting to extend existing recording with silence).
- Full module reset (hold TAP 2s in settings).

---

## Known Limitations

- Only slot 1 persists across power cycles (QSPI flash storage constraint).
- Maximum recording length per slot: ~9.4 seconds at 96kHz.
- Pitch range is asymmetric: +12 semitones up, -24 semitones down (pitch factor clamped to 0.25-2.0).
- At extreme speed and pitch combinations, audio quality degrades (expected with granular processing).
- External clock must use the Gate jack, not any CV jack. The Versio ADC inputs have lowpass filtering that destroys fast clock pulses.
- Lo-fi module (`lofi.h`) exists in the codebase but is not currently connected to the signal chain.

---

*Custom firmware by Andy. Built on the Electrosmith Daisy platform.*
