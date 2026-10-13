// The tube flaws applied to the sound itself: the same 60Hz mains hum, slow
// drift and flicker the picture gets, turned into audio. Plain C++ (no plugin
// code) so it can be tested on its own.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace crt {

class TubeAmp {
public:
    // flaws: 0 off (exact passthrough), 1 subtle, 2 worn. wet: 0 = dry, 1 = all flaws
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n,
                 int flaws, float wet, float sampleRate)
    {
        wet = std::clamp(wet, 0.0f, 1.0f);
        if (flaws <= 0 || wet <= 0) {
            for (uint32_t i = 0; i < n; ++i) {
                outL[i] = inL[i];
                outR[i] = inR[i];
            }
            env = 0;
            return;
        }
        const bool worn = flaws >= 2;
        const float humAmt = worn ? 0.06f : 0.016f;     // hum level relative to the music (-24 / -36 dB)
        const float driftAmt = worn ? 0.05f : 0.02f;    // slow left/right sway
        const float flickAmt = worn ? 0.07f : 0.025f;   // random level flutter, like the picture's flicker
        const float twoPi = 6.2831853f;
        const float humStep = twoPi * 60.0f / sampleRate, driftStep = twoPi * 0.23f / sampleRate;
        const float release = std::exp(-1.0f / (0.3f * sampleRate));
        const float smooth = 1.0f - std::exp(-1.0f / (0.01f * sampleRate));
        const uint32_t flickEvery = std::max<uint32_t>(1, (uint32_t)(sampleRate / 60.0f));

        for (uint32_t i = 0; i < n; ++i) {
            const float l = inL[i], r = inR[i];
            // the hum rides on the signal, like it only shows when the beam is on
            env = std::max(std::max(std::fabs(l), std::fabs(r)), env * release);
            const float hum = (std::sin(humPhase) + 0.5f * std::sin(2 * humPhase) + 0.25f * std::sin(3 * humPhase))
                            / 1.75f * env * humAmt;
            humPhase = std::fmod(humPhase + humStep, twoPi);
            driftPhase = std::fmod(driftPhase + driftStep, twoPi);
            if (++flickCount >= flickEvery) {
                flickCount = 0;
                rng = rng * 1664525u + 1013904223u;
                flickTarget = flickAmt * ((rng >> 8) * (2.0f / 16777216.0f) - 1.0f);
            }
            flick += (flickTarget - flick) * smooth;
            const float pan = driftAmt * std::sin(driftPhase);
            const float g = 1.0f + flick;
            outL[i] = l + wet * (l * g * (1.0f - pan) + hum - l);
            outR[i] = r + wet * (r * g * (1.0f + pan) + hum - r);
        }
    }

private:
    float env = 0, humPhase = 0, driftPhase = 0, flick = 0, flickTarget = 0;
    uint32_t flickCount = 0, rng = 12345u;
};

} // namespace crt
