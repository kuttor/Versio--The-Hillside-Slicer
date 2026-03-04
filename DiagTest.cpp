/**
 * HILLSIDE SLICER — DIAGNOSTIC TEST FIRMWARE
 * 
 * Flash this instead of HillsideSlicer.cpp to verify hardware basics.
 * 
 * WHAT IT DOES:
 *   - LED 0: Breathes green = audio callback is running
 *   - LED 1: Brightness tracks KNOB_0 = ADC working
 *   - LED 2: Blue when gate input is HIGH = gate jack working
 *   - LED 3: Red when TAP is held = button working
 *   - Audio: Passes input straight to output
 *
 * If LED 0 breathes green, your Daisy is alive and the callback runs.
 * If it doesn't, something is fundamentally broken at init.
 */

#include "daisy_versio.h"

using namespace daisy;

DaisyVersio hw;

// Simple phase accumulator for breathing LED
static float phase = 0.0f;

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    hw.ProcessAllControls();
    // ProcessAllControls on Versio only does analog (knobs).
    // Tap button must be debounced manually. Toggle switches (Switch3) don't need it.
    hw.tap.Debounce();

    // ── LED 0: Breathing green (proves callback runs) ──────
    phase += 0.00005f * static_cast<float>(size);
    if(phase > 1.0f) phase -= 1.0f;
    // Triangle wave: 0→1→0
    float breath = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
    breath = breath * breath; // Quadratic for smooth feel
    hw.SetLed(0, 0.0f, breath * 0.8f, 0.0f); // Green breathing

    // ── LED 1: Tracks KNOB_0 brightness (proves ADC works) ─
    float k0 = hw.GetKnobValue(DaisyVersio::KNOB_0);
    hw.SetLed(1, k0 * 0.6f, k0 * 0.6f, k0 * 0.6f); // White, brightness = knob

    // ── LED 2: Gate input indicator (proves gate jack works) ─
    bool gate_high = hw.Gate();
    hw.SetLed(2, 0.0f, 0.0f, gate_high ? 0.8f : 0.02f); // Blue when gate high

    // ── LED 3: TAP button indicator (proves button works) ───
    bool tap = hw.tap.Pressed();
    hw.SetLed(3, tap ? 0.8f : 0.02f, 0.0f, 0.0f); // Red when held

    hw.UpdateLeds();

    // ── Audio passthrough ──────────────────────────────────
    for(size_t i = 0; i < size; i++)
    {
        out[0][i] = in[0][i];
        out[1][i] = in[1][i];
    }
}

int main(void)
{
    hw.Init(true);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_96KHZ);
    hw.SetAudioBlockSize(256);
    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    for(;;)
    {
        hw.DelayMs(1);
    }
}
