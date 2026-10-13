// IMAGE mode on the sound: the traced picture shapes the audio passing through.
// The beam's path around the outline becomes a tremolo (left/right position
// moves the left channel's level, up/down the right's), and how busy each
// frame is sets a low-pass filter (sparse = dark, detailed = bright). A GIF
// changes both on every frame. Plain C++ so it can be tested on its own.
#pragma once

#include "ImageTrace.hpp"

namespace crt {

// What the audio needs from a picture: each frame's outline resampled to a
// fixed-size table, its timing, and how busy it is.
struct PictureMod {
    static constexpr int kN = 256;
    std::vector<float> x, y;      // frames * kN, -0.8..0.8
    std::vector<float> busy;      // per frame, 0..1
    std::vector<float> starts;    // seconds
    float length = 0;

    explicit PictureMod(const Picture& p)
    {
        const size_t nf = p.frames.size();
        x.assign(nf * kN, 0);
        y.assign(nf * kN, 0);
        busy.assign(nf, 0);
        starts = p.starts;
        length = p.length;
        for (size_t f = 0; f < nf; ++f) {
            const Frame& fr = p.frames[f];
            const size_t n = fr.x.size();
            busy[f] = std::min(1.0f, n / 2500.0f);
            for (int k = 0; k < kN; ++k) {
                const size_t j = n ? (size_t)k * n / kN : 0;
                x[f * kN + k] = n ? fr.x[j] : 0.8f;  // blank frame: no dip
                y[f * kN + k] = n ? fr.y[j] : 0.8f;
            }
        }
    }

    size_t frameAt(double t) const
    {
        if (starts.size() <= 1 || length <= 0)
            return 0;
        const float tt = (float)std::fmod(t, (double)length);
        const size_t i = std::upper_bound(starts.begin(), starts.end(), tt) - starts.begin();
        return i == 0 ? 0 : i - 1;
    }
    float frameLength(size_t f) const
    {
        if (starts.size() <= 1)
            return 0.5f;  // a still picture: trace it twice a second
        return (f + 1 < starts.size() ? starts[f + 1] : length) - starts[f];
    }
};

class PictureFx {
public:
    // In place on the audio. Called on the audio thread: no allocation.
    void process(float* l, float* r, uint32_t n, const PictureMod* pic, float wet, float sampleRate)
    {
        wet = std::clamp(wet, 0.0f, 1.0f);
        if (!pic || pic->busy.empty() || wet <= 0) {
            gl = gr = 1;
            lpL = lpR = 0;
            primed = false;
            return;
        }
        const float smoothG = 1.0f - std::exp(-1.0f / (0.004f * sampleRate));   // no clicks at outline jumps
        const float smoothC = 1.0f - std::exp(-1.0f / (0.03f * sampleRate));
        const float dt = 1.0f / sampleRate;
        for (uint32_t i = 0; i < n; ++i) {
            const size_t f = pic->frameAt(t);
            const float len = std::max(pic->frameLength(f), 0.02f);
            phase += dt / len;
            if (phase >= 1) phase -= std::floor(phase);
            const float p = phase * PictureMod::kN;
            const int k0 = (int)p % PictureMod::kN, k1 = (k0 + 1) % PictureMod::kN;
            const float a = p - std::floor(p);
            const float* X = &pic->x[f * PictureMod::kN];
            const float* Y = &pic->y[f * PictureMod::kN];
            const float vx = X[k0] + (X[k1] - X[k0]) * a, vy = Y[k0] + (Y[k1] - Y[k0]) * a;
            // -0.8..0.8 -> 30%..100% level (only ever turns down, never clips)
            const float tl = 1.0f - 0.7f * (0.8f - std::clamp(vx, -0.8f, 0.8f)) / 1.6f;
            const float tr = 1.0f - 0.7f * (0.8f - std::clamp(vy, -0.8f, 0.8f)) / 1.6f;
            // busy frame = bright, sparse = dark: 400 Hz .. 16 kHz
            const float cutoff = 400.0f * std::pow(40.0f, pic->busy[f]);
            const float tc = 1.0f - std::exp(-6.2831853f * std::min(cutoff, 0.45f * sampleRate) / sampleRate);
            if (!primed) {
                gl = tl; gr = tr; coef = tc;
                lpL = l[i]; lpR = r[i];
                primed = true;
            }
            gl += (tl - gl) * smoothG;
            gr += (tr - gr) * smoothG;
            coef += (tc - coef) * smoothC;
            lpL += (l[i] - lpL) * coef;
            lpR += (r[i] - lpR) * coef;
            l[i] += wet * (lpL * gl - l[i]);
            r[i] += wet * (lpR * gr - r[i]);
            t += dt;
        }
        if (t > 1e6) t = 0;
    }

private:
    double t = 0;
    float phase = 0, gl = 1, gr = 1, coef = 1, lpL = 0, lpR = 0;
    bool primed = false;
};

} // namespace crt
