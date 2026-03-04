#pragma once
#include <cstdint>

struct GateDetector
{
    bool  state  = false;
    bool  rising = false;
    float baseline = 0.0f;
    float prev_value = 0.0f;
    static constexpr float RISE_THRESHOLD = 0.15f;
    static constexpr float FALL_THRESHOLD = 0.10f;
    static constexpr float BASELINE_RATE  = 0.0001f;
    void Process(float value)
    {
        rising = false;
        baseline += (value - baseline) * BASELINE_RATE;
        float delta = value - prev_value;
        float above_baseline = value - baseline;
        if(!state)
        {
            if(delta > RISE_THRESHOLD && above_baseline > 0.2f)
            { state = true; rising = true; }
        }
        else
        {
            if(above_baseline < 0.1f || delta < -FALL_THRESHOLD)
                state = false;
        }
        prev_value = value;
    }
    bool RisingEdge() const { return rising; }
    bool High() const { return state; }
};

struct ClockTracker
{
    uint32_t samples_since_last_ = 0;
    uint32_t last_period_        = 0;
    uint32_t period_accum_       = 0;
    uint32_t period_count_       = 0;
    uint32_t avg_period_         = 0;
    bool     has_clock_          = false;
    bool     tick_               = false;
    static constexpr uint32_t TIMEOUT_SAMPLES = 96000 * 4;
    static constexpr uint32_t MIN_PERIOD      = 96000 / 30;
    static constexpr uint32_t AVG_WINDOW      = 4;
    void Process(bool rising_edge)
    {
        tick_ = false;
        samples_since_last_++;
        if(rising_edge && samples_since_last_ > MIN_PERIOD)
        {
            last_period_ = samples_since_last_;
            samples_since_last_ = 0;
            has_clock_ = true;
            tick_ = true;
            period_accum_ += last_period_;
            period_count_++;
            if(period_count_ >= AVG_WINDOW)
            {
                avg_period_ = period_accum_ / AVG_WINDOW;
                period_accum_ = 0;
                period_count_ = 0;
            }
        }
        if(samples_since_last_ > TIMEOUT_SAMPLES)
            has_clock_ = false;
    }
    bool Tick() const { return tick_; }
    bool HasClock() const { return has_clock_; }
    uint32_t GetPeriod() const { return avg_period_ > 0 ? avg_period_ : last_period_; }
    float GetBPM(float sample_rate) const
    {
        uint32_t p = GetPeriod();
        if(p == 0) return 120.0f;
        return (sample_rate * 60.0f) / static_cast<float>(p);
    }
};

// 24PPQN clock on Gate jack (digital input).
// 24 rising edges = 1 beat. bar_tick every bar_length*divider beats.
struct PPQNClockDetector
{
    static constexpr uint32_t PPQN    = 24;
    static constexpr uint32_t TIMEOUT = 96000 * 2;
    bool     has_clock  = false;
    bool     beat_tick  = false;
    bool     bar_tick   = false;
    float    bpm        = 120.0f;
    uint32_t divider    = 1;
    uint32_t bar_length = 4;
    void Process(bool rising_edge)
    {
        beat_tick = false;
        bar_tick  = false;
        since_++;
        beat_samp_++;
        if(rising_edge)
        {
            pc_++;
            if(pc_ >= PPQN)
            {
                pc_ = 0;
                beat_tick = true;
                if(beat_samp_ > 0)
                    bpm = (96000.0f * 60.0f) / static_cast<float>(beat_samp_);
                beat_samp_ = 0;
                bib_++;
                uint32_t eb = bar_length * divider;
                if(eb == 0) eb = 4;
                if(bib_ >= eb) { bib_ = 0; bar_tick = true; }
                has_clock = true;
            }
            since_ = 0;
        }
        if(since_ > TIMEOUT)
        { has_clock = false; pc_ = 0; bib_ = 0; beat_samp_ = 0; }
    }
    void ResetBar() { bib_ = 0; }
    float GetEffBPM() const
    { return has_clock ? bpm / static_cast<float>(divider) : 0.0f; }
    uint32_t GetBeatPeriod() const
    {
        if(!has_clock || bpm < 20.0f) return 0;
        return static_cast<uint32_t>((96000.0f * 60.0f) / bpm);
    }
  private:
    uint32_t pc_        = 0;
    uint32_t since_     = 0;
    uint32_t beat_samp_ = 0;
    uint32_t bib_       = 0;
};
