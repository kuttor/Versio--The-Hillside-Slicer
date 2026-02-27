#pragma once
#include <cstdint>

/**
 * GateDetector - Detects rising edges on CV inputs repurposed as gate jacks.
 * 
 * CRITICAL DESIGN NOTE:
 * On the Versio, each knob's pot and CV jack are summed in hardware before
 * the ADC. We can't read them separately. This means the pot position adds
 * a DC offset to any incoming gate signal.
 * 
 * Solution: We use DELTA detection (rate of change) rather than absolute
 * thresholds. A 5V gate causes a rapid ~0.5-1.0 jump in one sample,
 * while a pot turn is much slower. We detect fast transitions.
 * We also maintain a baseline tracker that slowly follows the signal
 * to adapt to any pot position.
 */
struct GateDetector
{
    bool  state  = false;
    bool  rising = false;
    float baseline = 0.0f;
    float prev_value = 0.0f;

    static constexpr float RISE_THRESHOLD = 0.15f;  // Min delta for rising edge
    static constexpr float FALL_THRESHOLD = 0.10f;  // Min delta for falling edge
    static constexpr float BASELINE_RATE  = 0.0001f; // Slow baseline tracking

    void Process(float value)
    {
        rising = false;

        // Baseline slowly tracks the signal (follows pot, ignores gates)
        baseline += (value - baseline) * BASELINE_RATE;

        float delta = value - prev_value;
        float above_baseline = value - baseline;

        if(!state)
        {
            // Look for rapid upward jump AND being significantly above baseline
            if(delta > RISE_THRESHOLD && above_baseline > 0.2f)
            {
                state  = true;
                rising = true;
            }
        }
        else
        {
            // Look for signal dropping back toward baseline
            if(above_baseline < 0.1f || delta < -FALL_THRESHOLD)
            {
                state = false;
            }
        }

        prev_value = value;
    }

    /** Returns true only on the sample where the gate went high */
    bool RisingEdge() const { return rising; }

    /** Returns true while gate is held high */
    bool High() const { return state; }
};

/**
 * ClockTracker - Measures period between rising edges for tempo detection.
 * Also provides phase accumulator for smooth playback sync.
 */
struct ClockTracker
{
    uint32_t samples_since_last_ = 0;
    uint32_t last_period_        = 0;
    uint32_t period_accum_       = 0;
    uint32_t period_count_       = 0;
    uint32_t avg_period_         = 0;
    bool     has_clock_          = false;
    bool     tick_               = false;

    static constexpr uint32_t TIMEOUT_SAMPLES = 96000 * 4; // 4 sec at 96kHz = no clock
    static constexpr uint32_t MIN_PERIOD      = 96000 / 30; // Max 1800 BPM (sanity)
    static constexpr uint32_t AVG_WINDOW      = 4;

    void Process(bool rising_edge)
    {
        tick_ = false;
        samples_since_last_++;

        if(rising_edge && samples_since_last_ > MIN_PERIOD)
        {
            last_period_        = samples_since_last_;
            samples_since_last_ = 0;
            has_clock_          = true;
            tick_               = true;

            // Running average of period for stability
            period_accum_ += last_period_;
            period_count_++;
            if(period_count_ >= AVG_WINDOW)
            {
                avg_period_   = period_accum_ / AVG_WINDOW;
                period_accum_ = 0;
                period_count_ = 0;
            }
        }

        // Timeout - no clock present
        if(samples_since_last_ > TIMEOUT_SAMPLES)
        {
            has_clock_ = false;
        }
    }

    /** True on the sample a clock tick arrived */
    bool Tick() const { return tick_; }

    /** True if we've been receiving regular clock pulses */
    bool HasClock() const { return has_clock_; }

    /** Average period in samples between clock ticks */
    uint32_t GetPeriod() const { return avg_period_ > 0 ? avg_period_ : last_period_; }

    /** BPM derived from clock period */
    float GetBPM(float sample_rate) const
    {
        uint32_t p = GetPeriod();
        if(p == 0)
            return 120.0f;
        return (sample_rate * 60.0f) / static_cast<float>(p);
    }
};
