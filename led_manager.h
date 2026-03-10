#pragma once
#include <cstdint>
#include <cmath>
#include "slice_engine.h"

/**
 * LED Manager for Hillside Slicer
 *
 * 4 RGB LEDs on Daisy Versio.
 *
 * RENDERING PRIORITY (highest first):
 *   1. Clear sweep — red animation while holding TAP
 *   2. Slot display — brief overlay on slot change
 *   3. State: Empty / Armed / Recording / Playing / Overdub / Stopped
 *
 * PLAYBACK RANGE RULE:
 *   LEDs outside the start→end range are ALWAYS off (0,0,0).
 *   This applies in both PLAYING and OVERDUBBING states.
 *   BG brightness only shows on in-range, non-playhead LEDs.
 */
class LedManager
{
  public:
    struct LedColor { float r = 0.0f; float g = 0.0f; float b = 0.0f; };

    LedManager() {}
    void Init(float sr) { (void)sr; }

    void Update(RecordState state, float progress, uint32_t current_slot,
                const RecordState* slot_states, float clear_progress,
                bool show_range, uint32_t range_start,
                uint32_t range_end, uint32_t total_slices,
                bool cyan_flash, uint32_t cyan_slice,
                bool oneshot_waiting = false,
                bool threshold_armed = false)
    {
        (void)show_range;
        // Cache normalized range for RenderArmed
        if(total_slices > 0)
        {
            armed_ns_ = static_cast<float>(range_start)
                      / static_cast<float>(total_slices);
            armed_ne_ = static_cast<float>(range_end + 1)
                      / static_cast<float>(total_slices);
            if(armed_ne_ > 1.0f) armed_ne_ = 1.0f;
            if(armed_ns_ >= armed_ne_) armed_ne_ = armed_ns_ + 0.01f;
        }
        (void)cyan_flash;
        (void)cyan_slice;

        if(slot_display_timer_ > 0)
            slot_display_timer_--;

        // ── PRIORITY 1: Clear sweep (user holding TAP) ─────
        if(clear_progress > 0.001f)
        {
            slot_display_timer_ = 0;
            RenderClearSweep(clear_progress);
            return;
        }

        // ── PRIORITY 2: Slot display overlay ────────────────
        if(slot_display_timer_ > 0)
        {
            RenderSlotDisplay(current_slot, slot_states);
            return;
        }

        // ── PRIORITY 3: State rendering ─────────────────────
        switch(state)
        {
            case RecordState::EMPTY:
                RenderAllOff();
                break;

            case RecordState::ARMED:
                RenderArmed();
                break;

            case RecordState::RECORDING:
                RenderRecording(progress);
                break;

            case RecordState::PLAYING:
                if(oneshot_waiting)
                    RenderAllOff();  // Oneshot waiting: dark until triggered
                else if(threshold_armed)
                    RenderPlayback(progress, range_start, range_end,
                                   total_slices, false, true);
                else
                    RenderPlayback(progress, range_start, range_end,
                                   total_slices, false);
                break;

            case RecordState::OVERDUBBING:
                RenderPlayback(progress, range_start, range_end,
                               total_slices, true);
                break;

            case RecordState::STOPPED:
                RenderStopped(range_start, range_end, total_slices);
                break;

            case RecordState::CLEARING:
                RenderAllOff();
                break;
        }
    }

    void ShowSlotDisplay(uint32_t ticks = 400) { slot_display_timer_ = ticks; }
    bool IsSlotDisplayActive() const { return slot_display_timer_ > 0; }

    const LedColor& GetLed(uint32_t i) const
    {
        return (i < 4) ? leds_[i] : leds_[0];
    }

    void OverrideLed(uint32_t i, float r, float g, float b)
    {
        if(i < 4) leds_[i] = {r, g, b};
    }

  private:
    // Background brightness for in-range, non-playhead LEDs.
    // 0.10 is clearly visible and well above PWM flicker threshold.
    static constexpr float BG = 0.10f;

    // ── Render functions ────────────────────────────────────

    void RenderAllOff()
    {
        for(int i = 0; i < 4; i++)
            leds_[i] = {0.0f, 0.0f, 0.0f};
    }

    void RenderArmed()
    {
        // Warm orange on in-range LEDs only, off on out-of-range.
        // Uses cached range from last Update call.
        for(int i = 0; i < 4; i++)
        {
            float q_lo = static_cast<float>(i) * 0.25f;
            float q_hi = q_lo + 0.25f;
            if(q_lo < armed_ne_ && q_hi > armed_ns_)
                leds_[i] = {0.3f, 0.08f, 0.0f};
            else
                leds_[i] = {0.0f, 0.0f, 0.0f};
        }
    }

    void RenderRecording(float progress)
    {
        // Red fill within the slice range only.
        // Uses cached armed_ns_/armed_ne_ from Update().
        if(progress > 1.0f) progress = 1.0f;
        // Map progress into the range
        float fill_pos = armed_ns_ + progress * (armed_ne_ - armed_ns_);

        for(int i = 0; i < 4; i++)
        {
            float q_lo = static_cast<float>(i) * 0.25f;
            float q_hi = q_lo + 0.25f;

            // Out of range: off
            if(q_lo >= armed_ne_ || q_hi <= armed_ns_)
            {
                leds_[i] = {0.0f, 0.0f, 0.0f};
                continue;
            }

            if(fill_pos >= q_hi)
                leds_[i] = {0.6f, 0.0f, 0.0f};      // Filled
            else if(fill_pos > q_lo)
            {
                float f = (fill_pos - q_lo) / 0.25f;
                leds_[i] = {0.25f + f * 0.35f, 0.0f, 0.0f};
            }
            else
                leds_[i] = {0.08f, 0.0f, 0.0f};      // In range, not yet
        }
    }

    /**
     * Playback rendering — used for both PLAYING and OVERDUBBING.
     *
     * @param overdub  true = red colors (overdubbing), false = green (playing)
     *
     * Range rule: LEDs whose quadrant midpoint falls OUTSIDE the
     * start→end range are completely off. No background, no playhead.
     */
    void RenderPlayback(float progress, uint32_t range_start,
                        uint32_t range_end, uint32_t total_slices,
                        bool overdub, bool threshold_armed = false)
    {
        if(total_slices == 0) { RenderAllOff(); return; }

        // Normalize range to 0.0–1.0
        float norm_s = static_cast<float>(range_start)
                     / static_cast<float>(total_slices);
        float norm_e = static_cast<float>(range_end + 1)
                     / static_cast<float>(total_slices);
        if(norm_e > 1.0f) norm_e = 1.0f;
        if(norm_s >= norm_e) norm_e = norm_s + 0.01f;

        // Playhead absolute position
        float abs_pos = norm_s + progress * (norm_e - norm_s);
        int head_q = static_cast<int>(abs_pos * 4.0f);
        if(head_q > 3) head_q = 3;
        if(head_q < 0) head_q = 0;

        for(int i = 0; i < 4; i++)
        {
            // Overlap test: does LED quadrant overlap the slice range?
            float q_lo = static_cast<float>(i) * 0.25f;
            float q_hi = q_lo + 0.25f;

            // RANGE CHECK: no overlap -> completely off
            if(q_lo >= norm_e || q_hi <= norm_s)
            {
                leds_[i] = {0.0f, 0.0f, 0.0f};
                continue;
            }

            // PLAYHEAD: full brightness
            if(i == head_q)
            {
                if(overdub)
                    leds_[i] = {1.0f, 0.0f, 0.0f};       // Red
                else if(threshold_armed)
                    leds_[i] = {0.8f, 0.5f, 0.0f};       // Yellow/amber
                else
                    leds_[i] = {0.0f, 1.0f, 0.0f};       // Green
            }
            else
            {
                if(overdub)
                    leds_[i] = {BG, 0.0f, 0.0f};         // Dim red
                else if(threshold_armed)
                    leds_[i] = {BG, BG * 0.6f, 0.0f};    // Dim amber
                else
                    leds_[i] = {0.0f, BG, 0.0f};         // Dim green
            }
        }
    }

    void RenderOneshotWaiting(uint32_t rs, uint32_t re, uint32_t ts)
    {
        if(ts == 0) { RenderAllOff(); return; }
        float ns = static_cast<float>(rs) / static_cast<float>(ts);
        float ne = static_cast<float>(re + 1) / static_cast<float>(ts);
        if(ne > 1.0f) ne = 1.0f;
        if(ns >= ne) ne = ns + 0.01f;
        for(int i = 0; i < 4; i++)
        {
            float q_lo = static_cast<float>(i) * 0.25f;
            float q_hi = q_lo + 0.25f;
            if(q_lo < ne && q_hi > ns)
                leds_[i] = {0.0f, BG * 0.5f, BG};
            else
                leds_[i] = {0.0f, 0.0f, 0.0f};
        }
    }

    void RenderStopped(uint32_t range_start, uint32_t range_end,
                       uint32_t total_slices)
    {
        // Dim green on in-range LEDs, off on out-of-range
        if(total_slices == 0) { RenderAllOff(); return; }
        float norm_s = static_cast<float>(range_start)
                     / static_cast<float>(total_slices);
        float norm_e = static_cast<float>(range_end + 1)
                     / static_cast<float>(total_slices);
        if(norm_e > 1.0f) norm_e = 1.0f;
        if(norm_s >= norm_e) norm_e = norm_s + 0.01f;

        for(int i = 0; i < 4; i++)
        {
            float q_lo = static_cast<float>(i) * 0.25f;
            float q_hi = q_lo + 0.25f;
            if(q_lo < norm_e && q_hi > norm_s)
                leds_[i] = {0.0f, BG, 0.0f};
            else
                leds_[i] = {0.0f, 0.0f, 0.0f};
        }
    }

    /** Red sweep RIGHT→LEFT while holding TAP to clear */
    void RenderClearSweep(float progress)
    {
        if(progress > 1.0f) progress = 1.0f;
        for(int i = 0; i < 4; i++)
        {
            // LED 3 fills first (progress 0-25%), LED 0 fills last (75-100%)
            float q_start = static_cast<float>(3 - i) * 0.25f;
            float q_end   = q_start + 0.25f;

            if(progress >= q_end)
                leds_[i] = {0.8f, 0.0f, 0.0f};       // Full
            else if(progress >= q_start)
            {
                float f = (progress - q_start) / 0.25f;
                leds_[i] = {0.2f + f * 0.6f, 0.0f, 0.0f}; // Filling
            }
            else
                leds_[i] = {0.0f, 0.0f, 0.0f};       // Not yet
        }
    }

    /** 8 slots on 4 LEDs: even=blue, odd=purple */
    void RenderSlotDisplay(uint32_t current_slot,
                           const RecordState* slot_states)
    {
        (void)slot_states;
        // Clean: ONLY the selected slot LED is lit, all others off.
        // Slots 0-3 = purple, slots 4-7 = blue.
        bool upper_bank = (current_slot >= 4);
        uint32_t led_idx = current_slot & 3;  // 0-3 within bank
        for(int i = 0; i < 4; i++)
        {
            if(static_cast<uint32_t>(i) == led_idx)
            {
                if(upper_bank)
                    leds_[i] = {0.0f, 0.0f, 0.9f};     // Bright blue
                else
                    leds_[i] = {0.5f, 0.0f, 0.9f};     // Bright purple
            }
            else
                leds_[i] = {0.0f, 0.0f, 0.0f};
        }
    }

    float       armed_ns_ = 0.0f;
    float       armed_ne_ = 1.0f;
    LedColor    leds_[4];
    uint32_t    slot_display_timer_ = 0;
};
