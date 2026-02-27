#pragma once
#include <cstdint>
#include <cmath>
#include "slice_engine.h"

/**
 * LedManager - Controls the 4 RGB LEDs based on module state.
 * 
 * States:
 *   EMPTY     → All dim white
 *   ARMED     → All pulsing orange (breathing)
 *   RECORDING → Progressive red fill (quarter by quarter)
 *   PLAYING   → Progressive green showing playback position
 *   SLOT_VIEW → Blue (persistent) or purple (session) slot indicator
 *   CLEARING  → Red drain animation
 */
class LedManager
{
  public:
    struct LedColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    LedManager() {}

    void Init(float sr)
    {
        sample_rate_   = sr;
        phase_         = 0.0f;
        clear_progress_ = 0.0f;
        force_slot_display_ = false;
        slot_display_timer_ = 0;
    }

    /** Call once per control-rate update */
    void Update(RecordState state, float progress, uint32_t current_slot,
                const RecordState* slot_states, bool clearing)
    {
        phase_ += 3.0f / sample_rate_ * 256.0f; // ~3Hz breathing rate at 256-sample blocks
        if(phase_ > 6.2831853f)
            phase_ -= 6.2831853f;

        // Slot display timer countdown
        if(slot_display_timer_ > 0)
        {
            slot_display_timer_--;
            if(slot_display_timer_ == 0)
                force_slot_display_ = false;
        }

        if(clearing)
        {
            // Clearing animation: red drain from right to left
            RenderClearing();
        }
        else if(force_slot_display_)
        {
            // Show slot status (after knob turn)
            RenderSlotView(current_slot, slot_states);
        }
        else
        {
            switch(state)
            {
                case RecordState::EMPTY:
                    RenderEmpty();
                    break;
                case RecordState::ARMED:
                    RenderArmed();
                    break;
                case RecordState::RECORDING:
                    RenderRecording(progress);
                    break;
                case RecordState::PLAYING:
                    RenderPlaying(progress);
                    break;
                case RecordState::CLEARING:
                    RenderClearing();
                    break;
            }
        }
    }

    /** Force slot display mode (called when slot knob changes) */
    void ShowSlotDisplay(uint32_t duration_ticks = 400)
    {
        force_slot_display_ = true;
        slot_display_timer_ = duration_ticks;
    }

    /** Get LED color for a given index (0-3) */
    const LedColor& GetLed(uint32_t idx) const
    {
        return (idx < 4) ? leds_[idx] : leds_[0];
    }

  private:
    void RenderEmpty()
    {
        // Dim white on all LEDs
        for(int i = 0; i < 4; i++)
        {
            leds_[i] = {0.05f, 0.05f, 0.05f};
        }
    }

    void RenderArmed()
    {
        // All LEDs pulsing orange (breathing)
        float breath = (sinf(phase_) + 1.0f) * 0.5f; // 0-1 sine breathing
        breath = breath * breath; // Quadratic for more pleasing curve
        float r = breath * 0.8f;
        float g = breath * 0.3f;
        for(int i = 0; i < 4; i++)
        {
            leds_[i] = {r, g, 0.0f};
        }
    }

    void RenderRecording(float progress)
    {
        // Progressive red fill - each LED = 25% of recording
        // Current quarter flashes, completed quarters solid
        for(int i = 0; i < 4; i++)
        {
            float quarter_start = static_cast<float>(i) * 0.25f;
            float quarter_end   = quarter_start + 0.25f;

            if(progress >= quarter_end)
            {
                // This quarter is complete: solid red
                leds_[i] = {0.8f, 0.0f, 0.0f};
            }
            else if(progress >= quarter_start)
            {
                // Currently recording this quarter: flashing red
                float flash = (sinf(phase_ * 4.0f) + 1.0f) * 0.5f;
                leds_[i] = {flash * 0.9f, 0.0f, 0.0f};
            }
            else
            {
                // Not yet reached: dark
                leds_[i] = {0.0f, 0.0f, 0.0f};
            }
        }
    }

    void RenderPlaying(float progress)
    {
        // Progressive green fill - same logic as recording but green
        for(int i = 0; i < 4; i++)
        {
            float quarter_start = static_cast<float>(i) * 0.25f;
            float quarter_end   = quarter_start + 0.25f;

            if(progress >= quarter_end)
            {
                // Passed: solid green
                leds_[i] = {0.0f, 0.6f, 0.0f};
            }
            else if(progress >= quarter_start)
            {
                // Current position: bright flashing green
                float flash = (sinf(phase_ * 3.0f) + 1.0f) * 0.5f;
                leds_[i] = {0.0f, 0.3f + flash * 0.5f, 0.0f};
            }
            else
            {
                // Not yet: dim green
                leds_[i] = {0.0f, 0.05f, 0.0f};
            }
        }
    }

    void RenderSlotView(uint32_t current_slot, const RecordState* slot_states)
    {
        // Show slot number (1-8) using LED count and color
        // Slots 0-3: blue (persistent concept), Slots 4-7: purple (session)
        bool persistent = (current_slot < 4);
        uint32_t display_count = (current_slot % 4) + 1; // 1-4 LEDs

        bool has_content = (slot_states[current_slot] == RecordState::PLAYING);

        for(uint32_t i = 0; i < 4; i++)
        {
            if(i < display_count)
            {
                float brightness = has_content ? 0.8f : 0.15f;
                if(persistent)
                    leds_[i] = {0.0f, brightness * 0.2f, brightness}; // Blue
                else
                    leds_[i] = {brightness * 0.6f, 0.0f, brightness}; // Purple
            }
            else
            {
                leds_[i] = {0.0f, 0.0f, 0.0f};
            }
        }
    }

    void RenderClearing()
    {
        // Red drain animation from right to left
        clear_progress_ += 0.005f;
        if(clear_progress_ > 1.0f)
            clear_progress_ = 1.0f;

        for(int i = 3; i >= 0; i--)
        {
            float threshold = 1.0f - (static_cast<float>(i) + 1.0f) * 0.25f;
            if(clear_progress_ > threshold)
                leds_[i] = {0.0f, 0.0f, 0.0f}; // Drained
            else
                leds_[i] = {0.5f, 0.0f, 0.0f}; // Still red
        }
    }

    LedColor leds_[4];
    float    sample_rate_      = 96000.0f;
    float    phase_            = 0.0f;
    float    clear_progress_   = 0.0f;
    bool     force_slot_display_ = false;
    uint32_t slot_display_timer_ = 0;
};
