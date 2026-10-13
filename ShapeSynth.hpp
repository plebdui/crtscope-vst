// SHAPE: the shape on the tube played as sound, the way oscilloscope music
// works (left = x, right = y, one trip around the outline per cycle). The
// pitch follows the note in the incoming audio and the level follows its
// loudness, so the shape plays along with the track. The SHAPE knob blends it
// with the input. Plain C++ so it can be tested on its own.
#pragma once

#include "PictureFx.hpp"
#include <utility>

namespace crt {

class ShapeSynth {
public:
    // mix: 0 = only the input (untouched), 1 = only the shape
    void process(float* l, float* r, uint32_t n, const PictureMod* pic, float mix, float sampleRate)
    {
        if (!pic || pic->busy.empty()) {
            listen(l, r, n, sampleRate);
            return;
        }
        run(l, r, n, mix, sampleRate, [pic](double t) {
            const size_t f = pic->frameAt(t);
            return std::make_pair(&pic->x[f * PictureMod::kN], &pic->y[f * PictureMod::kN]);
        });
    }

    // Same, from one outline table (kN points) that the caller refreshes every block (EQUATION mode)
    void process(float* l, float* r, uint32_t n, const float* X, const float* Y, float mix, float sampleRate)
    {
        run(l, r, n, mix, sampleRate, [=](double) { return std::make_pair(X, Y); });
    }

    // no shape to play: keep following the input so the pitch is ready
    void listen(const float* l, const float* r, uint32_t n, float sampleRate)
    {
        for (uint32_t i = 0; i < n; ++i)
            track(l[i], r[i], sampleRate);
    }

    float pitch() const { return freq; }  // Hz, for tests

private:
    template <class Source>
    void run(float* l, float* r, uint32_t n, float mix, float sampleRate, Source source)
    {
        mix = std::clamp(mix, 0.0f, 1.0f);
        if (mix <= 0) {
            listen(l, r, n, sampleRate);
            return;
        }
        const float glide = 1.0f - std::exp(-1.0f / (0.015f * sampleRate));
        const float dc = std::exp(-6.2831853f * 10.0f / sampleRate);  // shapes aren't centred on 0: keep that out of the speakers
        const float dt = 1.0f / sampleRate;
        for (uint32_t i = 0; i < n; ++i) {
            track(l[i], r[i], sampleRate);
            freq *= std::pow(target / freq, glide);
            phase += freq * dt;
            if (phase >= 1) phase -= std::floor(phase);
            const auto xy = source(t);
            const float p = phase * PictureMod::kN;
            const int k0 = (int)p % PictureMod::kN, k1 = (k0 + 1) % PictureMod::kN;
            const float a = p - std::floor(p);
            const float x = xy.first[k0] + (xy.first[k1] - xy.first[k0]) * a;
            const float y = xy.second[k0] + (xy.second[k1] - xy.second[k0]) * a;
            const float sx = x / 0.8f * env, sy = y / 0.8f * env;
            const float ox = sx - dcInX + dc * dcOutX, oy = sy - dcInY + dc * dcOutY;
            dcInX = sx; dcOutX = ox;
            dcInY = sy; dcOutY = oy;
            l[i] += mix * (ox - l[i]);
            r[i] += mix * (oy - r[i]);
            t += dt;
        }
        if (t > 1e6) t = 0;
    }

    // Loudness and pitch of the input. The pitch is found a few dozen times a
    // second (YIN) from a copy slowed to about 12 kHz; between notes or on
    // noise it holds the last one.
    void track(float l, float r, float sampleRate)
    {
        const float a = std::max(std::fabs(l), std::fabs(r));
        if (sampleRate != rate) {
            rate = sampleRate;
            decim = std::max(1, (int)std::lround(sampleRate / 12000.0f));
            attack = 1.0f - std::exp(-1.0f / (0.002f * sampleRate));
            release = 1.0f - std::exp(-1.0f / (0.15f * sampleRate));
            fill = acc = 0;
            count = 0;
        }
        env += (a - env) * (a > env ? attack : release);
        acc += 0.5f * (l + r);
        if (++count < decim)
            return;
        buf[fill++] = acc / decim;
        acc = 0;
        count = 0;
        if (fill == kBuf) {
            detect(rate / decim);
            std::copy(buf + kHop, buf + kBuf, buf);  // keep the newest, analyse again a hop later
            fill = kBuf - kHop;
        }
    }

    void detect(float fs)
    {
        float energy = 0;
        for (int j = 0; j < kWin; ++j)
            energy += buf[kBuf - kWin + j] * buf[kBuf - kWin + j];
        if (energy / kWin < 1e-7f)  // about -70 dB: silence, keep the last note
            return;
        const int minLag = std::max(2, (int)(fs / 1000)), maxLag = std::min(kBuf - kWin - 1, (int)(fs / 40));
        const float* x = buf + kBuf - kWin - maxLag;  // window ends at the newest sample
        float sum = 0, best = 1e9f;
        int bestLag = -1;
        for (int tau = 1; tau <= maxLag; ++tau) {
            float d = 0;
            for (int j = 0; j < kWin; ++j) {
                const float e = x[maxLag + j] - x[maxLag + j - tau];
                d += e * e;
            }
            sum += d;
            cmnd[tau] = sum > 0 ? d * tau / sum : 1;
        }
        for (int tau = minLag; tau <= maxLag; ++tau) {
            if (cmnd[tau] < 0.15f) {  // first clear dip, then down to its bottom
                while (tau + 1 <= maxLag && cmnd[tau + 1] < cmnd[tau]) ++tau;
                bestLag = tau;
                break;
            }
            if (cmnd[tau] < best) { best = cmnd[tau]; bestLag = tau; }
        }
        if (bestLag < 0 || cmnd[bestLag] > 0.35f)  // no clear note (noise, drums)
            return;
        float lag = (float)bestLag;
        if (bestLag > minLag && bestLag < maxLag) {
            const float y0 = cmnd[bestLag - 1], y1 = cmnd[bestLag], y2 = cmnd[bestLag + 1];
            const float den = y0 - 2 * y1 + y2;
            if (den > 0) lag += 0.5f * (y0 - y2) / den;
        }
        target = std::clamp(fs / lag, 40.0f, 1000.0f);
    }

    static constexpr int kBuf = 1024, kWin = 512, kHop = 256;
    float buf[kBuf] = {}, cmnd[kBuf] = {};
    int fill = 0, count = 0, decim = 1;
    float acc = 0, rate = 0, attack = 1, release = 1, env = 0;
    float target = 110, freq = 110, phase = 0;
    float dcInX = 0, dcOutX = 0, dcInY = 0, dcOutY = 0;
    double t = 0;
};

} // namespace crt
