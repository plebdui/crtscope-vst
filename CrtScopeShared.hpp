// What the plugin (audio) and its window share: the parameter list and the
// audio ring buffer the window reads from.
#pragma once

#include "DistrhoPlugin.hpp"
#include <atomic>

START_NAMESPACE_DISTRHO

enum Params {
    kMode = 0,      // XY / goniometer / waveform
    kPhosphor,      // 0-3 real phosphors, 4-18 Apple II colours
    kAfterglow,     // afterglow length multiplier
    kIntensity,     // beam brightness
    kGain,          // how big the picture is
    kAutoGain,      // keep the picture filling the tube
    kGraticule,     // grid on the glass
    kFlaws,         // off / subtle / worn
    kSweep,         // waveform mode: milliseconds across the screen
    kParamCount
};

// Single writer (audio thread), single reader (window). The writer never waits.
struct AudioRing {
    static constexpr uint32_t kSize = 1u << 17;  // ~2.7s at 48kHz
    static constexpr uint32_t kMask = kSize - 1;
    float left[kSize] = {};
    float right[kSize] = {};
    std::atomic<uint32_t> written{0};

    void write(const float* l, const float* r, uint32_t n) noexcept
    {
        uint32_t w = written.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i, ++w) {
            left[w & kMask] = l[i];
            right[w & kMask] = r[i];
        }
        written.store(w, std::memory_order_release);
    }
};

class CrtScopePlugin : public Plugin {
public:
    CrtScopePlugin();
    AudioRing ring;

protected:
    const char* getLabel() const override { return "CRTScope"; }
    const char* getDescription() const override
    {
        return "A 1960s military CRT oscilloscope: XY Lissajous figures, goniometer and a triggered waveform, with phosphor glow.";
    }
    const char* getMaker() const override { return "plebdui"; }
    const char* getHomePage() const override { return "https://github.com/plebdui/crtscope"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override;
    void initParameter(uint32_t index, Parameter& parameter) override;
    float getParameterValue(uint32_t index) const override;
    void setParameterValue(uint32_t index, float value) override;
    void run(const float** inputs, float** outputs, uint32_t frames) override;

private:
    float fParams[kParamCount];
    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrtScopePlugin)
};

END_NAMESPACE_DISTRHO
