// CRT Scope tube renderer: a port of crtscope.py's phosphor simulation, drawn
// at full pixel resolution instead of terminal braille dots. Plain C++ with no
// plugin code, so it can be tested on its own (see tests/render_test.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace crt {

struct Phosphor {
    const char* name;
    float flash[3];  // colour where the beam lands
    float tauF;      // how fast that flash fades (seconds)
    float glow[3];   // colour of the afterglow
    float tauG;      // how long the afterglow lasts (seconds)
};

// The 4 real phosphors, then the Apple II's composite colours (same numbers as crtscope.py)
inline const std::vector<Phosphor>& phosphors()
{
    static const std::vector<Phosphor> list = [] {
        std::vector<Phosphor> v = {
            {"P7 radar",        {170, 195, 255}, 0.03f, {175, 255, 50},  1.2f},
            {"P1 green",        {190, 255, 190}, 0.02f, {40, 255, 70},   0.25f},
            {"P12 orange",      {255, 215, 140}, 0.04f, {255, 120, 15},  0.8f},
            {"P31 green-short", {215, 255, 215}, 0.01f, {70, 255, 120},  0.06f},
        };
        struct A { const char* name; float r, g, b; };
        static const A apple[] = {
            {"A2 Dark Green", 10, 67, 34},   {"A2 Royal Blue", 4, 50, 175},  {"A2 Medium Blue", 53, 159, 231},
            {"A2 Dark Red", 154, 25, 27},    {"A2 Gray", 109, 110, 93},      {"A2 Purple", 164, 82, 210},
            {"A2 Periwinkle", 132, 174, 239}, {"A2 Dark Yel-Green", 54, 39, 21}, {"A2 Green", 40, 141, 19},
            {"A2 Gray 2", 103, 91, 78},      {"A2 Aquamarine", 83, 175, 162}, {"A2 Red", 196, 79, 17},
            {"A2 Yellow", 195, 149, 38},     {"A2 Rose", 205, 123, 144},     {"A2 White", 182, 168, 186},
        };
        for (const A& a : apple) {
            float g[3] = {a.r * 255 / 186, a.g * 255 / 186, a.b * 255 / 186};  // the photo's white was 186
            const float peak = std::max({g[0], g[1], g[2]});
            if (peak < 150)  // keep the darkest ones visible on black
                for (float& c : g) c *= 150 / peak;
            Phosphor p{a.name, {}, 0.02f, {}, 0.35f};
            for (int i = 0; i < 3; ++i) {
                p.glow[i] = std::min(g[i], 255.f);
                p.flash[i] = std::min(g[i] + (255 - g[i]) * 0.55f, 255.f);
            }
            v.push_back(p);
        }
        return v;
    }();
    return list;
}

enum Mode { kXY = 0, kGonio = 1, kWave = 2, kImage = 3 };

class Tube {
public:
    // ---- settings (set them any time)
    int mode = kXY;
    int phosphor = 0;
    float afterglow = 1.0f;   // multiplies the phosphor's afterglow time
    float intensity = 1.0f;   // beam brightness
    float gain = 1.0f;
    bool autoGain = true;
    bool graticule = true;
    int flaws = 1;            // 0 off, 1 subtle (hum, soft rim, flicker), 2 worn
    float sweepMs = 20.0f;    // waveform mode: time across the screen

    // picture mode: the outline to draw this step (-1..1, y up; blank = beam
    // off on the way to that point). Owned by the caller.
    const float* shapeX = nullptr;
    const float* shapeY = nullptr;
    const uint8_t* shapeBlank = nullptr;
    int shapeN = 0;
    bool shapeMoving = false;  // animated: short afterglow so frames don't smear

    void resize(int w, int h)
    {
        if (w == W && h == H)
            return;
        W = std::max(w, 16);
        H = std::max(h, 16);
        R = std::max(8.0f, std::min(W, H) / 2.0f - 16.0f);
        cx = W / 2.0f;
        cy = H / 2.0f;
        flash.assign(W * H, 0.f);
        glow.assign(W * H, 0.f);
        rgbaBuf.assign(W * H * 4, 0);
        hw = (W + 1) / 2;
        hh = (H + 1) / 2;
        half.assign(hw * hh, 0.f);
        tmp.assign(hw * hh, 0.f);
        fv.assign(W * H, 0.f);
        gv.assign(W * H, 0.f);
        buildStatic();
        if (satLut.empty()) {
            satLut.resize(kSatN + 1);
            for (int i = 0; i <= kSatN; ++i) satLut[i] = 1 - std::exp(-kSatMax * i / kSatN);
            powF.resize(kCurveN + 1);
            powG.resize(kCurveN + 1);
            for (int i = 0; i <= kCurveN; ++i) {
                powF[i] = std::pow((float)i / kCurveN, 1.2f);
                powG[i] = std::pow((float)i / kCurveN, 0.7f);
            }
        }
    }

    // New audio from the plugin. Kept until the next step().
    void feed(const float* left, const float* right, int n, float sampleRate)
    {
        if (sampleRate > 0)
            sr = sampleRate;
        for (int i = 0; i < n; ++i) {
            pendL.push_back(left[i]);
            pendR.push_back(right[i]);
        }
        const size_t cap = (size_t)sr;  // never pile up more than a second
        if (pendL.size() > cap) {
            pendL.erase(pendL.begin(), pendL.end() - cap);
            pendR.erase(pendR.begin(), pendR.end() - cap);
        }
    }

    // Advance the simulation by dt seconds: fade the phosphor, sweep the beam
    // through the new audio.
    void step(float dt)
    {
        if (W == 0)
            return;
        dt = std::clamp(dt, 1e-4f, 0.25f);
        const Phosphor& ph = phosphors()[std::clamp(phosphor, 0, (int)phosphors().size() - 1)];
        const float tauF = ph.tauF;
        float tauG = ph.tauG * afterglow;
        if (mode == kImage && shapeMoving)
            tauG = std::min(tauG, 0.12f);
        const float kf = std::exp(-dt / tauF), kg = std::exp(-dt / tauG);
        for (size_t i = 0; i < flash.size(); ++i) {
            flash[i] *= kf;
            glow[i] *= kg;
        }

        std::vector<float> L, Rt;
        L.swap(pendL);
        Rt.swap(pendR);
        const float power = 900.0f * intensity;

        if (mode == kImage) {
            if (shapeN < 2)
                return;
            // the music pushes the picture about: it swells with the level and
            // the stereo signal nudges the beam
            float lvl = 0;
            for (size_t i = 0; i < L.size(); ++i)
                lvl = std::max(lvl, std::max(std::fabs(L[i]), std::fabs(Rt[i])));
            peak = std::max({lvl, peak * std::exp(-dt / 1.5f), 0.01f});
            const float rel = lvl / peak;
            const float size = R * gain * (1.0f + 0.05f * rel);
            std::vector<float> px(shapeN), py(shapeN);
            const size_t m = L.size();
            for (int i = 0; i < shapeN; ++i) {
                float wx = 0, wy = 0;
                if (m) {
                    const size_t j = (size_t)i * m / shapeN;
                    wx = L[j] / peak * 0.015f;
                    wy = Rt[j] / peak * 0.015f;
                }
                px[i] = cx + (shapeX[i] + wx) * size;
                py[i] = cy - (shapeY[i] + wy) * size;
            }
            trace(px, py, shapeN / dt, power * 3.0f, tauF, tauG, shapeBlank);
            return;
        }

        if (mode == kWave) {
            for (size_t i = 0; i < L.size(); ++i)
                waveBuf.push_back((L[i] + Rt[i]) * 0.5f);
            const int win = std::max(16, (int)(sweepMs / 1000.0f * sr));
            const size_t keep = std::max((size_t)(3 * win), (size_t)(sr / 4));
            if (waveBuf.size() > keep)
                waveBuf.erase(waveBuf.begin(), waveBuf.end() - keep);
            float wpeak = 0;
            for (float v : waveBuf) wpeak = std::max(wpeak, std::fabs(v));
            if (wpeak < 1e-4f || (int)waveBuf.size() < 2)
                return;  // silence: beam off, the screen just fades
            std::vector<float> seg = trigger(win);
            lastWave = seg;
            const float scale = (autoGain ? 0.8f / std::max(wpeak, 0.01f) : 1.0f) * gain * R;
            std::vector<float> px(seg.size()), py(seg.size());
            for (size_t i = 0; i < seg.size(); ++i) {
                px[i] = cx + (-1.0f + 2.0f * i / (seg.size() - 1)) * R;
                py[i] = cy - seg[i] * scale;
            }
            // a real scope sweeps over and over: about dt / sweep sweeps per frame
            const float sweeps = std::min(dt / (sweepMs / 1000.0f), 8.0f);
            trace(px, py, sr, power * sweeps, tauF, tauG);
            return;
        }

        if (L.empty())
            return;
        std::vector<float> x(L.size()), y(L.size());
        float cur = 0;
        for (size_t i = 0; i < L.size(); ++i) {
            if (mode == kGonio) {  // rotate 45 degrees: mono = vertical
                x[i] = (L[i] - Rt[i]) * 0.7071f;
                y[i] = (L[i] + Rt[i]) * 0.7071f;
            } else {
                x[i] = L[i];
                y[i] = Rt[i];
            }
            cur = std::max(cur, std::hypot(x[i], y[i]));
        }
        if (cur < 1e-4f)
            return;  // silence: don't burn a dot into the middle of the screen
        peak = std::max({cur, peak * std::exp(-dt / 1.5f), 0.01f});
        const float scale = (autoGain ? 0.92f / peak : 1.0f) * gain * R;

        // 4x Catmull-Rom upsampling: at 44.1/48 kHz there are few samples per
        // cycle, and straight lines between them look like a polygon
        const int up = 4;
        std::vector<float> px, py;
        px.reserve(x.size() * up + 4);
        py.reserve(x.size() * up + 4);
        auto at = [&](std::vector<float>& v, float last, long i) {
            if (i < 0) return last;
            return v[std::min<long>(i, (long)v.size() - 1)];
        };
        for (long i = 0; i < (long)x.size(); ++i) {
            const float x0 = at(x, lastX, i - 1), x1 = x[i], x2 = at(x, 0, i + 1), x3 = at(x, 0, i + 2);
            const float y0 = at(y, lastY, i - 1), y1 = y[i], y2 = at(y, 0, i + 1), y3 = at(y, 0, i + 2);
            for (int k = 0; k < up; ++k) {
                const float t = (float)k / up;
                px.push_back(cx + cr(x0, x1, x2, x3, t) * scale);
                py.push_back(cy - cr(y0, y1, y2, y3, t) * scale);
            }
        }
        lastX = x.back();
        lastY = y.back();
        trace(px, py, sr * up, power, tauF, tauG);
    }

    // The picture as RGBA bytes (W x H).
    const uint8_t* render()
    {
        const Phosphor& ph = phosphors()[std::clamp(phosphor, 0, (int)phosphors().size() - 1)];
        // phosphor response 1 - e^-x, looked up instead of computed per pixel
        for (size_t i = 0; i < flash.size(); ++i) {
            fv[i] = sat(flash[i]);
            gv[i] = sat(glow[i]);
        }
        // ---- bloom: brightness at half resolution, blurred, added back as a halo
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < hw; ++x) {
                float s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int X = std::min(2 * x + dx, W - 1), Y = std::min(2 * y + dy, H - 1);
                        const int i = Y * W + X;
                        s += fv[i] + gv[i];
                    }
                half[y * hw + x] = s * 0.25f;
            }
        blur(half, 3);
        blur(half, 3);

        const float flick = flaws ? 1 + (flaws == 1 ? 0.025f : 0.07f) * (2.0f * std::rand() / RAND_MAX - 1) : 1.0f;
        const float rimSoft = flaws == 0 ? 0.0f : flaws == 1 ? 0.35f : 0.8f;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int i = y * W + x;
                uint8_t* o = &rgbaBuf[i * 4];
                o[3] = 255;
                const float face = faceMask[i];
                float r = kPanel[0], g = kPanel[1], b = kPanel[2];
                if (face > 0) {
                    const float v = vignette[i] * flick;
                    const float halo = half[(y / 2) * hw + x / 2] * v * (0.55f + rimSoft * rr2[i]);
                    const float fp = curve(powF, fv[i] * v), gp = curve(powG, gv[i] * v);
                    float c[3];
                    for (int k = 0; k < 3; ++k)
                        c[k] = ph.glow[k] * gp + ph.flash[k] * fp + ph.glow[k] * halo * 0.6f;
                    const float pk = std::max({c[0], c[1], c[2]});
                    if (pk > 255)  // overdriven: saturate toward white like a real tube
                        for (float& ck : c) ck = ck * 255 / pk + (pk - 255) * 0.35f;
                    // glass tint, then the etched graticule shows through under the trace
                    float gr = kGlass[0] * vignette[i], gg = kGlass[1] * vignette[i], gb = kGlass[2] * vignette[i];
                    if (graticule && grat[i]) {
                        gr = std::max(gr, kGrat[0] * grat[i]);
                        gg = std::max(gg, kGrat[1] * grat[i]);
                        gb = std::max(gb, kGrat[2] * grat[i]);
                    }
                    r = c[0] + gr; g = c[1] + gg; b = c[2] + gb;
                    r = r * face + kPanel[0] * (1 - face);
                    g = g * face + kPanel[1] * (1 - face);
                    b = b * face + kPanel[2] * (1 - face);
                }
                const float bz = bezel[i];
                if (bz > 0) {
                    r += (kBezel[0] - r) * bz;
                    g += (kBezel[1] - g) * bz;
                    b += (kBezel[2] - b) * bz;
                }
                o[0] = (uint8_t)std::clamp(r, 0.f, 255.f);
                o[1] = (uint8_t)std::clamp(g, 0.f, 255.f);
                o[2] = (uint8_t)std::clamp(b, 0.f, 255.f);
            }
        }
        return rgbaBuf.data();
    }

    int width() const { return W; }
    int height() const { return H; }

private:
    static constexpr float kPanel[3] = {16, 17, 19};
    static constexpr float kGlass[3] = {7, 10, 8};
    static constexpr float kGrat[3] = {52, 64, 56};
    static constexpr float kBezel[3] = {70, 70, 74};

    int W = 0, H = 0, hw = 0, hh = 0;
    float R = 8, cx = 0, cy = 0, sr = 48000;
    float peak = 0.05f, lastX = 0, lastY = 0, humT = 0;
    std::vector<float> flash, glow, half, tmp, fv, gv;
    std::vector<float> satLut, powF, powG;
    static constexpr int kSatN = 4096, kCurveN = 1024;
    static constexpr float kSatMax = 10.0f;

    float sat(float x) const  // 1 - e^-x
    {
        if (x <= 0) return 0;
        if (x >= kSatMax) return 1;
        const float p = x * (kSatN / kSatMax);
        const int i = (int)p;
        return satLut[i] + (satLut[i + 1] - satLut[i]) * (p - i);
    }

    static float curve(const std::vector<float>& lut, float x)  // x^1.2 or x^0.7 on [0, 1]
    {
        if (x <= 0) return 0;
        if (x >= 1) return 1;
        const float p = x * kCurveN;
        const int i = (int)p;
        return lut[i] + (lut[i + 1] - lut[i]) * (p - i);
    }
    std::vector<float> faceMask, vignette, rr2, grat, bezel;
    std::vector<uint8_t> rgbaBuf;
    std::vector<float> pendL, pendR, waveBuf, lastWave;

    static float cr(float p0, float p1, float p2, float p3, float t)
    {
        return 0.5f * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                       + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
    }

    void buildStatic()
    {
        faceMask.assign(W * H, 0.f);
        vignette.assign(W * H, 0.f);
        rr2.assign(W * H, 0.f);
        grat.assign(W * H, 0.f);
        bezel.assign(W * H, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int i = y * W + x;
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                const float rr = d / R;
                faceMask[i] = std::clamp(R - d + 0.5f, 0.f, 1.f);  // antialiased rim
                vignette[i] = std::clamp(1 - 0.45f * std::pow(rr, 4.f), 0.3f, 1.f);  // dimmer toward the rim
                rr2[i] = std::min(rr, 1.f) * std::min(rr, 1.f);
                bezel[i] = std::clamp(1.6f - std::fabs(d - (R + 6)), 0.f, 1.f);
            }
        // graticule: 10 x 10 divisions of dotted lines, finer ticks along the centre axes
        const float div = R / 5;
        auto dot = [&](int x, int y, float a) {
            if (x >= 0 && x < W && y >= 0 && y < H && faceMask[y * W + x] > 0.5f)
                grat[y * W + x] = std::max(grat[y * W + x], a);
        };
        for (int k = -5; k <= 5; ++k) {
            const int gx = (int)std::lround(cx + k * div), gy = (int)std::lround(cy + k * div);
            const float a = k == 0 ? 1.0f : 0.75f;
            for (int t = 0; t < std::max(W, H); t += 3) {
                dot(gx, t, a);
                dot(t, gy, a);
            }
        }
        for (int k = -25; k <= 25; ++k) {
            const int tx = (int)std::lround(cx + k * div / 5), ty = (int)std::lround(cy + k * div / 5);
            for (int s = -3; s <= 3; ++s) {
                dot(tx, (int)cy + s, 0.9f);
                dot((int)cx + s, ty, 0.9f);
            }
        }
    }

    // Box blur in place, radius r, both directions (on the half-size buffer)
    void blur(std::vector<float>& a, int r)
    {
        const float n = 2.0f * r + 1;
        for (int y = 0; y < hh; ++y) {
            float s = 0;
            for (int x = -r; x <= r; ++x) s += a[y * hw + std::clamp(x, 0, hw - 1)];
            for (int x = 0; x < hw; ++x) {
                tmp[y * hw + x] = s / n;
                s += a[y * hw + std::min(x + r + 1, hw - 1)] - a[y * hw + std::max(x - r, 0)];
            }
        }
        for (int x = 0; x < hw; ++x) {
            float s = 0;
            for (int y = -r; y <= r; ++y) s += tmp[std::clamp(y, 0, hh - 1) * hw + x];
            for (int y = 0; y < hh; ++y) {
                a[y * hw + x] = s / n;
                s += tmp[std::min(y + r + 1, hh - 1) * hw + x] - tmp[std::max(y - r, 0) * hw + x];
            }
        }
    }

    // Scope trigger: the window starts where the wave last rose through half its
    // height; if several places qualify, the one that best matches what's on screen.
    std::vector<float> trigger(int win)
    {
        const std::vector<float>& s = waveBuf;
        const int n = (int)s.size();
        if (n <= win + 2)
            return std::vector<float>(s.begin(), s.end());
        const int k = 24;
        std::vector<float> sm(n, 0.f);
        float acc = 0, mx = -1e9f;
        for (int i = 0; i < n; ++i) {
            acc += s[i];
            if (i >= k) acc -= s[i - k];
            sm[i] = acc / k;
            mx = std::max(mx, sm[i]);
        }
        const float level = 0.5f * mx;
        std::vector<int> cand;
        for (int i = 1; i <= n - win; ++i)
            if (sm[i - 1] < level && sm[i] >= level)
                cand.push_back(i);
        int start = n - win;
        if (!cand.empty()) {
            start = cand.back();
            if ((int)lastWave.size() == win && cand.size() > 1) {
                const int step = std::max(1, win / 400);
                double best = 1e30;
                for (size_t c = cand.size() > 24 ? cand.size() - 24 : 0; c < cand.size(); ++c) {
                    double e = 0;
                    for (int j = 0; j < win; j += step) {
                        const double d = s[cand[c] + j] - lastWave[j];
                        e += d * d;
                    }
                    if (e < best) { best = e; start = cand[c]; }
                }
            }
        }
        return std::vector<float>(s.begin() + start, s.begin() + start + win);
    }

    // Sweep the beam through the points. Every point gets the same beam time,
    // spread along its path, so slow parts of the trace glow brightest.
    void trace(std::vector<float>& px, std::vector<float>& py, float rate, float power,
               float tauF, float tauG, const uint8_t* blank = nullptr)
    {
        const size_t n = px.size();
        if (n < 2)
            return;
        if (flaws) {
            // an old tube's amplifiers pick up the 60Hz mains hum and drift a little
            const float a = (flaws == 1 ? 0.0035f : 0.009f) * R;
            for (size_t i = 0; i < n; ++i) {
                const float t = humT + i / rate;
                py[i] += a * std::sin(6.2831853f * 60 * t);
                px[i] += 0.6f * a * std::sin(6.2831853f * 0.23f * t + 1.3f);
            }
            humT = std::fmod(humT + n / rate, 100.0f);
        }
        const float scale = power * (R / 100.0f);
        const float addF = scale * 0.7f / tauF, addG = scale / tauG;
        for (size_t i = 1; i < n; ++i) {
            if (blank && blank[i])
                continue;  // beam off while it jumps to the next stroke
            const float x0 = px[i - 1], y0 = py[i - 1];
            const float dx = px[i] - x0, dy = py[i] - y0;
            const int steps = std::clamp((int)std::ceil(std::hypot(dx, dy)), 1, 64);
            const float e = (1.0f / rate) / steps;
            for (int s = 0; s < steps; ++s) {
                const float t = (float)s / steps;
                splat(x0 + dx * t, y0 + dy * t, e * addF, e * addG);
            }
        }
    }

    // Deposit beam energy at a sub-pixel position (bilinear: smooth, thin lines)
    void splat(float x, float y, float ef, float eg)
    {
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - x0, fy = y - y0;
        const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
        for (int k = 0; k < 4; ++k) {
            if (xs[k] < 0 || xs[k] >= W || ys[k] < 0 || ys[k] >= H)
                continue;
            const int i = ys[k] * W + xs[k];
            flash[i] += ef * w[k];
            glow[i] += eg * w[k];
        }
    }
};

} // namespace crt
