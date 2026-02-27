# The Hillside Slicer v1.0.0

### A Granular Timestretch Sample Slicer for Noise Engineering Versio

---

```
██╗  ██╗██╗██╗     ██╗     ███████╗██╗██████╗ ███████╗
██║  ██║██║██║     ██║     ██╔════╝██║██╔══██╗██╔════╝
███████║██║██║     ██║     ███████╗██║██║  ██║█████╗  
██╔══██║██║██║     ██║     ╚════██║██║██║  ██║██╔══╝  
██║  ██║██║███████╗███████╗███████║██║██████╔╝███████╗
╚═╝  ╚═╝╚═╝╚══════╝╚══════╝╚══════╝╚═╝╚═════╝ ╚══════╝

███████╗██╗     ██╗ ██████╗███████╗██████╗
██╔════╝██║     ██║██╔════╝██╔════╝██╔══██╗
███████╗██║     ██║██║     █████╗  ██████╔╝
╚════██║██║     ██║██║     ██╔══╝  ██╔══██╗
███████║███████╗██║╚██████╗███████╗██║  ██║
╚══════╝╚══════╝╚═╝ ╚═════╝╚══════╝╚═╝  ╚═╝

🔪 Record. Slice. Stretch. Re-Re-Re-Re-Repeat. 🔪
```

---

## Overview

**The Hillside Slicer** is a custom firmware for the Noise Engineering Versio platform that turns your module into a stereo sample slicer with granular timestretch, dual trigger modes, and lo-fi degradation. Record audio into 8 memory slots, automatically slice it, then timestretch and pitch-shift independently using overlap-add granular synthesis.

It's built for performance. Three gestures run the entire recording system. Two trigger inputs give you rhythmic control. One knob destroys everything beautifully.

**Core philosophy:** Clean timestretching with musical character control — not a granular synth, not a looper, not a delay. A sample slicer that respects your source material until you tell it not to.

---

## Features

### 🎙️ 8-Slot Sample Memory

| Spec | Detail |
| --- | --- |
| **Slots** | 8 independent stereo buffers |
| **Time Per Slot** | ~10.4 seconds at 96kHz |
| **Total Memory** | 64MB SDRAM (full capacity) |
| **Bit Depth** | 32-bit float |
| **Sample Rate** | 96kHz stereo |

### ✂️ Automatic Slicing

Recorded audio is automatically divided into even slices on capture. Switch between 16, 32, or 64 slices per buffer with the toggle switch. Slice boundaries are calculated on the fly — no menus, no screens, no waiting.

### 🔊 Granular Timestretch Engine

Decoupled pitch and time via overlap-add granular synthesis:

* **Speed** — 0.25x to 4x playback speed in musical ratios (/4, /3, /2, x1, x2, x3, x4)
* **Pitch** — ±12 semitones, fully independent of transport speed
* **Overlap** — 2x (rhythmic, choppy) to 8x (lush, smooth) grain density
* **12-voice polyphonic** grain pool with Hanning window crossfades

Slow a breakbeat to half speed without dropping pitch. Pitch a vocal up an octave without changing timing. Or crank both and enter the shadow realm.

### ⚡ Dual Trigger System: Choke & Open

Two CV trigger inputs repurposed from KNOB_0 and KNOB_1 jacks:

| Trigger | Behavior |
| --- | --- |
| **Choke** (KNOB_0 CV) | Kills all grains instantly, advances to next slice, resets transport |
| **Open** (KNOB_1 CV) | Lets current grains fade naturally via window, starts new slice underneath |

**Auto-detect:** When neither CV jack is patched, the module enters auto-advance mode — slices advance automatically on the master clock. Patch detection uses activity monitoring with a 3-second timeout.

### 🔥 Lo-Fi Degradation

One knob sweeps from pristine 96kHz/24-bit to crushed 8kHz/8-bit:

```
KNOB_5 Position    Bit Depth    Sample Rate    Character
─────────────────────────────────────────────────────────
0%                 24-bit       96 kHz         Pristine
25%                16-bit       48 kHz         CD quality
50%                12-bit       26 kHz         SP-1200 zone
75%                10-bit       16 kHz         Crunchy
100%                8-bit        8 kHz         Destroyed
```

Quadratic curve gives you more resolution in the subtle 12-16 bit sweet spot where things start getting interesting.

### 🎙️ Three Record Modes

| Mode | Switch Position | Behavior |
| --- | --- | --- |
| **Immediate** | SW_0 Up | Starts recording the instant you press TAP |
| **Clock-Sync** | SW_0 Center | Waits for next clock pulse on Gate In, then starts |
| **Threshold** | SW_0 Down | Arms and waits for audio input to exceed threshold, with 100ms pre-roll buffer so you never miss the transient |

### 💡 LED Feedback System

Four RGB LEDs provide instant visual status:

| State | LED Behavior |
| --- | --- |
| **Empty Slot** | Dim white |
| **Armed** | Breathing orange (sine wave, 3Hz) |
| **Recording** | Progressive red fill, current quarter flashes |
| **Playing** | Progressive green fill, current position flashes |
| **Slot View** | 1-4 LEDs lit — blue (slots 0-3) or purple (slots 4-7), brightness indicates content |
| **Clearing** | Red drain animation right-to-left |

---

## Control Surface

```
┌─────────────────────────────────────────────────────────┐
│                    THE HILLSIDE SLICER                  │
│                                                         │
│   KNOB_0          KNOB_1          KNOB_2       KNOB_3   │
│  ┌──────┐        ┌──────┐        ┌──────┐    ┌──────┐   │
│  │START │        │LENGTH│        │SPEED │    │PITCH │   │
│  │SLICE │        │SLICES│        │ x1   │    │ 0 st │   │
│  └──┬───┘        └──┬───┘        └──────┘    └──────┘   │
│     │CV=CHOKE       │CV=OPEN                            │
│                                                         │
│   KNOB_4          KNOB_5          KNOB_6                │
│  ┌──────┐        ┌──────┐        ┌──────┐               │
│  │OVER- │        │LO-FI │        │ SLOT │               │
│  │ LAP  │        │      │        │ 1-8  │               │
│  └──────┘        └──────┘        └──┬───┘               │
│                                     │CV=SLOT SELECT     |
│                                                         │
│   [SW_0: REC MODE]    [SW_1: SLICES]    [TAP]  [GATE]   │
│    ↑IMM  •CLOCK  ↓THR   ↑16  •32  ↓64   ●      ⊡        │
│                                                         │
│           ◉ LED_0  ◉ LED_1  ◉ LED_2  ◉ LED_3            │
└─────────────────────────────────────────────────────────┘
```

### Parameter Reference

| Control | Function | Range | Notes |
| --- | --- | --- | --- |
| **KNOB_0 pot** | Start slice position | 0-100% of buffer | Selects which slice to begin playback from |
| **KNOB_0 CV** | **CHOKE trigger** | Gate/trigger | Kills grains, advances slice, resets transport |
| **KNOB_1 pot** | Play length | 1 slice to full buffer | How many slices to play before looping |
| **KNOB_1 CV** | **OPEN trigger** | Gate/trigger | Natural grain fade, starts new slice |
| **KNOB_2 + CV** | Timestretch speed | /4, /3, /2, x1, x2, x3, x4 | Musical ratios, quantized |
| **KNOB_3 + CV** | Pitch shift | ±12 semitones | Independent of speed — true pitch/time decoupling |
| **KNOB_4 + CV** | Grain overlap | 2x – 8x | Low = rhythmic/choppy, high = lush/smooth |
| **KNOB_5 + CV** | Lo-fi amount | Pristine → 8-bit/8kHz | Bit depth + sample rate degrade together |
| **KNOB_6 pot** | Slot select | Slots 1-8 | 8 positions with dead zones to prevent slipping |
| **KNOB_6 CV** | Slot select CV | 0-5V across 8 slots | Voltage-controlled sample switching |
| **SW_0** | Record mode | Immediate / Clock-sync / Threshold | 3-position toggle |
| **SW_1** | Slice count | 16 / 32 / 64 | 3-position toggle |
| **TAP short** | Record / Stop | Momentary | Press to arm/record, press again to stop |
| **TAP hold 2s** | Clear slot | Momentary | Erases current slot |
| **Gate In** | Master clock | Gate/clock | Clock tracking with running average (4-sample window) |

---

## Slot Knob Mapping

KNOB_6 maps across 8 discrete positions with dead zones to prevent accidental switching during performance:

```
Knob Position      Slot    LED Color     LED Count
───────────────────────────────────────────────────
0.000 – 0.110      0      Blue          ◉○○○
0.140 – 0.235      1      Blue          ◉◉○○
0.265 – 0.360      2      Blue          ◉◉◉○
0.390 – 0.485      3      Blue          ◉◉◉◉
0.515 – 0.610      4      Purple        ◉○○○
0.640 – 0.735      5      Purple        ◉◉○○
0.765 – 0.860      6      Purple        ◉◉◉○
0.890 – 1.000      7      Purple        ◉◉◉◉

Dead zones between positions prevent accidental switching.
Brightness indicates whether slot contains audio.
```

---

## Recording Workflow

```
                    ┌──────────────┐
                    │  TURN KNOB_6 │
                    │  Select Slot │
                    └──────┬───────┘
                           │
                    ┌──────┴───────┐
                    │  TAP PRESS   │
                    │  Arm Record  │
                    └──────┬───────┘
                           │
              ┌────────────┼───────────┐
              │            │           │
        ┌─────┴─────┐┌─────┴───┐┌──────┴──────┐
        │ IMMEDIATE ││  CLOCK  ││  THRESHOLD  │
        │ Records   ││ Waits   ││ Waits for   │
        │ instantly ││ for     ││ audio >     │
        │           ││ clock   ││ threshold   │
        │           ││ pulse   ││ (100ms      │
        │           ││         ││  pre-roll)  │
        └─────┬─────┘└─────┬───┘└──────┬──────┘
              │            │           │
              └────────────┼───────────┘
                           │
                    ┌──────┴──────┐
                    │  RECORDING  │
                    │  LED: Red   │
                    │  fill →→→→  │
                    └──────┬──────┘
                           │
                    ┌──────┴───────┐
                    │  TAP PRESS   │
                    │  Stop Record │
                    └──────┬───────┘
                           │
                    ┌──────┴───────┐
                    │  AUTO-SLICE  │
                    │  16/32/64    │
                    │  divisions   │
                    └──────┬───────┘
                           │
                    ┌──────┴───────┐
                    │  PLAYING     │
                    │  LED: Green  │
                    │  fill →→→→   │
                    └──────────────┘

        HOLD TAP 2s at any time → CLEAR current slot
```

---

## Signal Flow

```
                                              ┌──────────┐
                                              │ GATE IN  │
                                              │ (clock)  │
                                              └────┬─────┘
                                                   │
    AUDIO IN L/R ─────────────────┐           ┌────┴─────┐
                                  │           │  CLOCK   │
                                  │           │ TRACKER  │
                                  │           └────┬─────┘
                                  │                │
                             ┌────┴─────┐     ┌────┴────────────────────┐
                             │ RECORD   │     │ TRIGGER DETECT          │
                             │ ENGINE   │     │                         │
                             │          │     │  KNOB_0 CV → CHOKE      │
                             │ Pre-roll │     │  KNOB_1 CV → OPEN       │
                             │ 100ms    │     │                         │
                             └────┬─────┘     │  No patch? → Auto mode  │
                                  │           └────┬────────────────────┘
                                  │                │
                    ┌─────────────┴────────────────┘
                    │
              ┌─────┴──────────────────────────────────────┐
              │              SDRAM (64MB)                  │
              │                                            │
              │  ┌────────┐ ┌────────┐     ┌────────┐      │
              │  │ SLOT 0 │ │ SLOT 1 │ ... │ SLOT 7 │      │
              │  │ ~10.4s │ │ ~10.4s │     │ ~10.4s │      │
              │  │ stereo │ │ stereo │     │ stereo │      │
              │  └────────┘ └────────┘     └────────┘      │
              │                                            │
              │  KNOB_6 selects active slot ◄──────────    │
              └─────────────────┬──────────────────────────┘
                                │
                          ┌─────┴───────────────────────┐
                          │  SLICE                      │
                          │  ENGINE                     │
                          │                             │
                          │  KNOB_0 → Start position    │
                          │  KNOB_1 → Play length       │
                          │  SW_1   → 16/32/64 slices   │
                          └─────┬───────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │    GRAIN ENGINE       │
                    │    (12 voices)        │
                    │                       │
                    │  KNOB_2 → Speed       │
                    │  KNOB_3 → Pitch       │
                    │  KNOB_4 → Overlap     │
                    │                       │
                    │  Transport ──→ speed  │
                    │  Grains ────→ pitch   │
                    │  (independent)        │
                    │                       │
                    │  Hanning windows      │
                    │  Overlap-add sum      │
                    │  Soft clip output     │
                    └───────────┬───────────┘
                                │
                          ┌─────┴──────┐
                          │  LO-FI     │
                          │  ENGINE    │
                          │            │
                          │  KNOB_5 →  │
                          │  Bit crush │
                          │  +         │
                          │  Decimate  │
                          └─────┬──────┘
                                │
                           OUTPUT L/R
```

---

## Trigger Behavior

### Choke vs Open — What's the Difference?

```
CHOKE (hard cut):
  ┃▓▓▓▓▓▓▓▓▓│           │▓▓▓▓▓▓▓▓▓ │          │▓▓▓▓▓▓▓▓▓  │
  ┃ slice 1  │ silence  │ slice 2  │ silence  │ slice 3   │
  ┃──────────┴──────────┴──────────┴──────────┴───────────→

OPEN (crossfade):
  ┃▓▓▓▓▓▓▓▓▓▓▓▓░░░░░│                                     │
  ┃     slice 1  ╲   │                                    │
  ┃               ╲▓▓▓▓▓▓▓▓▓▓▓▓░░░░░│                     │
  ┃                    slice 2   ╲   │                    │
  ┃                               ╲▓▓▓▓▓▓▓▓▓▓▓▓░░░░░│     │
  ┃                                    slice 3            │
  ┃───────────────────────────────────────────────────────→

AUTO (no triggers patched):
  ┃▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓     │
  ┃ slice 1 → slice 2 → slice 3 → ...  (advances on clock)│
  ┃───────────────────────────────────────────────────────→
```

---

## Installation

### Prerequisites

* [Daisy Toolchain](https://daisy.audio) (arm-none-eabi-gcc)
* [libDaisy](https://github.com/electro-smith/libDaisy) — built (`make` in libDaisy directory)
* [DaisySP](https://github.com/electro-smith/DaisySP) — built (`make` in DaisySP directory)

### Build

```bash
# Directory structure should be:
#   YourFolder/
#   ├── libDaisy/          (built)
#   ├── DaisySP/           (built)
#   └── HillsideSlicer/    (this project)

cd HillsideSlicer

# Verify Makefile paths point to your lib locations:
#   LIBDAISY_DIR = ../../libDaisy
#   DAISYSP_DIR  = ../../DaisySP

make
```

### Flash

```bash
# Option 1: DFU (hold BOOT on Daisy Seed, plug USB, release BOOT)
make program-dfu

# Option 2: Daisy Web Programmer
# https://electro-smith.github.io/Programmer/
# Drag build/HillsideSlicer.bin onto the page
```

### Restore Stock Firmware

Flash any official Noise Engineering firmware via the [NE Portal](https://portal.noiseengineering.us/).

---

## Usage Tips

### First Boot

1. Power on — 4 dim white LEDs confirm empty slots
2. Turn **KNOB_6** — LEDs shift between blue and purple as you sweep slots
3. Patch audio into **IN L** (and optionally **IN R**)
4. Set **SW_0** to Immediate (up position)
5. Press **TAP** — LED breathes orange (armed), then red (recording)
6. Press **TAP** again to stop — audio is sliced, LED goes green
7. You're playing. Turn knobs.

### Classic Beat Slicer

1. Set **SW_1** to 16 slices
2. Record a 4-bar drum loop
3. Patch clock to **Gate In**, choke trigger to **KNOB_0 CV**
4. Send 16th-note triggers to choke input
5. Set **Speed** to x1, **Pitch** to 0
6. Turn **Start Slice** to scan through the break

### Timestretched Pads

1. Record a chord or texture
2. Set **Speed** to /4 (quarter speed)
3. Set **Overlap** to 8x (full smooth)
4. **Pitch** to taste
5. Patch nothing to triggers — let it auto-advance
6. Float away

### SP-1200 Mode

1. Record anything
2. Crank **KNOB_5** to ~50% for the 12-bit/26kHz sweet spot
3. Set **SW_1** to 32 slices
4. Choke triggers at 8th notes
5. Congratulations, it's 1987

### Voltage-Controlled Sample Switching

1. Record different material into several slots
2. Patch a step sequencer or LFO into **KNOB_6 CV**
3. Each voltage step selects a different slot
4. Combine with choke triggers for sample-hopping chaos
5. This is generative sample mangling

### Glitch Destruction

1. Record anything clean
2. **Speed** at /2, **Pitch** at +7 semitones
3. **Overlap** at 2x (choppy grains)
4. **Lo-fi** at 75%+
5. Open triggers with fast irregular timing
6. Now you're making Autechre jealous

---

## Technical Specifications

| Specification | Value |
| --- | --- |
| **Platform** | Noise Engineering Versio (Daisy Seed) |
| **Sample Rate** | 96 kHz |
| **Bit Depth** | 32-bit float (internal) |
| **Audio I/O** | Stereo in / Stereo out |
| **Channels** | 2 (stereo) |
| **Memory Slots** | 8 × ~10.4 seconds |
| **Total SDRAM** | 64 MB (95.48% utilized) |
| **Flash Usage** | 88.6 KB (67.61% of 128 KB) |
| **Grain Voices** | 12 polyphonic |
| **Grain Size Range** | 2.7ms – 200ms (internal, not user-facing) |
| **Default Grain Size** | ~50ms at 0.5 normalized |
| **Slice Counts** | 16 / 32 / 64 (switch-selectable) |
| **Pre-roll Buffer** | 100ms (9600 samples) for threshold mode |
| **Clock Timeout** | 4 seconds (auto-detect lost clock) |
| **Patch Detection** | 3-second activity timeout on CV jacks |
| **Latency** | Zero (direct synthesis) |
| **Toolchain** | arm-none-eabi-gcc (Daisy Toolchain) |

### Granular Engine Internals

```
Grain Size:      256 – 19,200 samples (2.7ms – 200ms)
Default:         4,800 samples (~50ms)
Window:          Hanning — 0.5 × (1 - cos(2π × phase))
Overlap:         2x – 8x (KNOB_4)
Normalization:   2.0 / overlap (compensates Hanning sum)
Grain Interval:  grain_size / overlap
Voice Stealing:  Oldest grain (highest phase) gets replaced
Output Clip:     Soft clip via fast tanh approximation
```

### Lo-Fi Engine Internals

```
Bit Reduction:   Quantize to 2^N levels (24 → 16 → 12 → 10 → 8)
Decimation:      Hold-and-repeat (counter-based)
Rate Sweep:      96kHz → 48 → 26 → 16 → 8kHz
Curve:           Quadratic (more resolution in subtle range)
```

---

## File Structure

```
HillsideSlicer/
├── HillsideSlicer.cpp    Main firmware — audio callback, control logic, init
├── grain_engine.h        Granular timestretch engine (12-voice overlap-add)
├── slice_engine.h        Recording, buffer management, slice calculation
├── gate_detect.h         Gate detection, clock tracking, trigger handling
├── lofi.h                Bit crush + sample rate decimation
├── led_manager.h         4× RGB LED state machine with animations
├── Makefile              Build configuration for Daisy toolchain
└── README.md             You are here
```

---

## Changelog

### v1.0.0 (February 2026)

* 🎉 **Initial release**
* ✂️ 8-slot stereo sample slicer at 96kHz
* 🔊 12-voice granular timestretch with independent pitch/time
* ⚡ Dual choke/open trigger system with auto-detect
* 🔥 Lo-fi degradation (24-bit pristine → 8-bit crushed)
* 🎙️ Three record modes: Immediate, Clock-sync, Threshold (with 100ms pre-roll)
* 💡 4× RGB LED feedback with animations
* 🎛️ Voltage-controllable slot selection

---

## Credits

**The Hillside Slicer** was built for the [Noise Engineering Versio](https://noiseengineering.us/) platform using [Electrosmith's Daisy Seed](https://www.electro-smith.com/daisy/daisy) and [libDaisy](https://github.com/electro-smith/libDaisy).

Inspired by:

* The E-mu SP-1200 and its beautiful 12-bit grit
* Granular synthesis techniques from Curtis Roads and Barry Truax
* The "just make it simple" design philosophy of hardware that gets out of your way
* Coffee, frustration, and the Reno high desert

---

## License

This firmware is provided as-is for use with Noise Engineering Versio hardware. Do whatever you want with it. Make music. Break things. Share it.

---

## Support

Got bugs? Got ideas? Got weird sounds you made with this thing?

**GitHub**: [github.com/kuttor/Versio--Hillside-Slicer](https://github.com/kuttor/Versio--Hillside-Slicer)

---

```
        🔪🔪🔪🔪🔪🔪🔪🔪
      🔪                🔪
     🔪   SLICE  IT    🔪
      🔪                🔪
        🔪🔪🔪🔪🔪🔪🔪🔪
```

**The Hillside Slicer** — *64 megabytes of bad decisions.*
