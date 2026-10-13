// The plugin window: the tube on top, a strip of buttons along the bottom.
// Left-click / scroll up = next or more, right-click / scroll down = back or less.
#include "DistrhoUI.hpp"
#include "CrtScopeShared.hpp"
#include "CrtTube.hpp"
#include "OpenGL.hpp"
#include <vector>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

START_NAMESPACE_DISTRHO

static constexpr uint kTubeSize = 640;
static constexpr uint kStrip = 46;

struct Button {
    uint32_t param;
    const char* caption;
};
static const Button kButtons[] = {
    {kMode, "MODE"},     {kPhosphor, "PHOSPHOR"}, {kAfterglow, "GLOW"},
    {kIntensity, "BEAM"}, {kGain, "GAIN"},         {kAutoGain, "AUTO"},
    {kGraticule, "GRID"}, {kFlaws, "FLAWS"},       {kSweep, "SWEEP"},
};
static constexpr int kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);
// how wide each button is, relative (phosphor names are long)
static const float kWeights[kNumButtons] = {1.0f, 1.9f, 0.9f, 0.9f, 0.9f, 0.8f, 0.8f, 1.0f, 1.0f};

class CrtScopeUI : public UI {
public:
    CrtScopeUI()
        : UI(kTubeSize, kTubeSize + kStrip)
    {
        loadSharedResources();
        for (int i = 0; i < kParamCount; ++i)
            fValues[i] = 0;
        fValues[kAfterglow] = fValues[kIntensity] = fValues[kGain] = 1;
        fValues[kAutoGain] = fValues[kGraticule] = fValues[kFlaws] = 1;
        fValues[kSweep] = 20;
        apply();
        fLast = std::chrono::steady_clock::now();
        if (const char* p = std::getenv("CRTSCOPE_DUMP"))  // testing: save a frame to this file
            fDumpPath = p;
    }

protected:
    // ---- host -> window
    void parameterChanged(uint32_t index, float value) override
    {
        if (index >= kParamCount)
            return;
        fValues[index] = value;
        apply();
        repaint();
    }

    // ---- called by the host many times a second: pull in new audio, move the beam
    void uiIdle() override
    {
        CrtScopePlugin* const plugin = static_cast<CrtScopePlugin*>(getPluginInstancePointer());
        if (plugin != nullptr) {
            AudioRing& ring = plugin->ring;
            const uint32_t written = ring.written.load(std::memory_order_acquire);
            uint32_t avail = written - fRead;
            if (avail > AudioRing::kSize / 2) {  // fell far behind (window was hidden): skip ahead
                fRead = written - AudioRing::kSize / 4;
                avail = AudioRing::kSize / 4;
            }
            float l[1024], r[1024];
            while (avail > 0) {
                const uint32_t n = std::min<uint32_t>(avail, 1024);
                for (uint32_t i = 0; i < n; ++i) {
                    l[i] = ring.left[(fRead + i) & AudioRing::kMask];
                    r[i] = ring.right[(fRead + i) & AudioRing::kMask];
                }
                fTube.feed(l, r, (int)n, (float)getSampleRate());
                fRead += n;
                avail -= n;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - fLast).count();
        fLast = now;
        fTube.step(dt);
        repaint();
    }

    void onNanoDisplay() override
    {
        const double scale = getScaleFactor();
        const uint stripH = (uint)std::lround(kStrip * scale);
        const uint w = getWidth(), h = getHeight() > stripH ? getHeight() - stripH : 1;
        fTube.resize((int)w, (int)h);
        const uint8_t* pixels = fTube.render();

        if (!fImage.isValid() || fImageW != w || fImageH != h) {
            fImage = createImageFromRGBA(w, h, pixels, 0);
            fImageW = w;
            fImageH = h;
        } else {
            fImage.update(pixels);
        }
        beginPath();
        rect(0, 0, w, h);
        fillPaint(imagePattern(0, 0, w, h, 0, fImage, 1.0f));
        fill();

        drawStrip((float)h, (float)stripH, (float)scale);

        // testing hook: snapshot the whole window (buttons included) 3s and 7s after opening
        if (!fDumpPath.empty() && fDumps < 2) {
            const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - fOpened).count();
            if (age > (fDumps == 0 ? 3.0f : 7.0f)) {
                static_cast<NanoVG*>(this)->endFrame();
                dumpFrontBuffer(fDumpPath + (fDumps == 0 ? ".full1.ppm" : ".full2.ppm"));
                static_cast<NanoVG*>(this)->beginFrame(getWidth(), getHeight());
                ++fDumps;
            }
        }
    }

    bool onMouse(const MouseEvent& ev) override
    {
        if (!ev.press)
            return false;
        const int b = buttonAt(ev.pos.getX(), ev.pos.getY());
        if (b < 0)
            return false;
        nudge(b, ev.button == 3 ? -1 : 1);  // right button goes back
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const int b = buttonAt(ev.pos.getX(), ev.pos.getY());
        if (b < 0 || ev.delta.getY() == 0)
            return false;
        nudge(b, ev.delta.getY() > 0 ? 1 : -1);
        return true;
    }

private:
    crt::Tube fTube;
    float fValues[kParamCount];
    uint32_t fRead = 0;
    std::chrono::steady_clock::time_point fLast;
    NanoImage fImage;
    uint fImageW = 0, fImageH = 0;
    std::string fDumpPath;
    int fDumps = 0;
    std::chrono::steady_clock::time_point fOpened = std::chrono::steady_clock::now();

    // testing hook: the frame just drawn, buttons included
    void dumpFrontBuffer(const std::string& path)
    {
        const int w = (int)getWidth(), h = (int)getHeight();
        std::vector<uint8_t> px((size_t)w * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6 %d %d 255\n", w, h);
            for (int y = h - 1; y >= 0; --y)  // OpenGL rows run bottom-up
                for (int x = 0; x < w; ++x)
                    std::fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
            std::fclose(f);
        }
    }

    void apply()
    {
        fTube.mode = (int)std::lround(fValues[kMode]);
        fTube.phosphor = (int)std::lround(fValues[kPhosphor]);
        fTube.afterglow = fValues[kAfterglow];
        fTube.intensity = fValues[kIntensity];
        fTube.gain = fValues[kGain];
        fTube.autoGain = fValues[kAutoGain] > 0.5f;
        fTube.graticule = fValues[kGraticule] > 0.5f;
        fTube.flaws = (int)std::lround(fValues[kFlaws]);
        fTube.sweepMs = fValues[kSweep];
    }

    // Change a setting by one step and tell the host (so FL records automation)
    void nudge(int b, int dir)
    {
        const uint32_t p = kButtons[b].param;
        float v = fValues[p];
        switch (p) {
        case kMode:      v = (float)(((int)v + dir + 3) % 3); break;
        case kPhosphor:  v = (float)(((int)v + dir + 19) % 19); break;
        case kFlaws:     v = (float)(((int)v + dir + 3) % 3); break;
        case kAutoGain:
        case kGraticule: v = v > 0.5f ? 0.0f : 1.0f; break;
        case kSweep:     v = std::clamp(v * (dir > 0 ? 1.5f : 1 / 1.5f), 1.0f, 200.0f); break;
        default:         v = std::clamp(v * (dir > 0 ? 1.25f : 0.8f), 0.05f, 20.0f); break;
        }
        fValues[p] = v;
        apply();
        editParameter(p, true);
        setParameterValue(p, v);
        editParameter(p, false);
        repaint();
    }

    void buttonRect(int b, float& x, float& w) const
    {
        float total = 0, before = 0;
        for (int i = 0; i < kNumButtons; ++i) {
            total += kWeights[i];
            if (i < b) before += kWeights[i];
        }
        x = getWidth() * before / total;
        w = getWidth() * kWeights[b] / total;
    }

    int buttonAt(double px, double py) const
    {
        const double stripH = kStrip * getScaleFactor();
        if (py < getHeight() - stripH)
            return -1;
        for (int b = 0; b < kNumButtons; ++b) {
            float x, w;
            buttonRect(b, x, w);
            if (px >= x && px < x + w)
                return b;
        }
        return -1;
    }

    void valueText(uint32_t p, char* out, size_t n) const
    {
        const float v = fValues[p];
        switch (p) {
        case kMode: {
            static const char* m[] = {"XY", "GONIO", "WAVE"};
            std::snprintf(out, n, "%s", m[std::clamp((int)std::lround(v), 0, 2)]);
            break;
        }
        case kPhosphor:
            std::snprintf(out, n, "%s", crt::phosphors()[std::clamp((int)std::lround(v), 0, 18)].name);
            break;
        case kFlaws: {
            static const char* f[] = {"OFF", "SUBTLE", "WORN"};
            std::snprintf(out, n, "%s", f[std::clamp((int)std::lround(v), 0, 2)]);
            break;
        }
        case kAutoGain:
        case kGraticule: std::snprintf(out, n, "%s", v > 0.5f ? "ON" : "OFF"); break;
        case kSweep:     std::snprintf(out, n, "%.1f ms", v); break;
        default:         std::snprintf(out, n, "%.2fx", v); break;
        }
    }

    void drawStrip(float y0, float h, float scale)
    {
        beginPath();
        rect(0, y0, getWidth(), h);
        fillColor(Color(22, 23, 26));
        fill();
        const crt::Phosphor& ph = crt::phosphors()[std::clamp((int)std::lround(fValues[kPhosphor]), 0, 18)];
        const Color accent((int)ph.glow[0], (int)ph.glow[1], (int)ph.glow[2]);
        fontFace(NANOVG_DEJAVU_SANS_TTF);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        char buf[48];
        for (int b = 0; b < kNumButtons; ++b) {
            float x, w;
            buttonRect(b, x, w);
            const bool dim = kButtons[b].param == kSweep && std::lround(fValues[kMode]) != crt::kWave;
            beginPath();
            roundedRect(x + 2 * scale, y0 + 4 * scale, w - 4 * scale, h - 8 * scale, 4 * scale);
            fillColor(Color(34, 36, 40));
            fill();
            strokeColor(Color(60, 62, 68));
            strokeWidth(1);
            stroke();
            fontSize(9 * scale);
            fillColor(Color(120, 128, 124));
            text(x + w / 2, y0 + 15 * scale, kButtons[b].caption, nullptr);
            valueText(kButtons[b].param, buf, sizeof(buf));
            fontSize(11.5f * scale);
            fillColor(dim ? Color(80, 84, 82) : accent);
            text(x + w / 2, y0 + 30 * scale, buf, nullptr);
        }
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrtScopeUI)
};

UI* createUI()
{
    return new CrtScopeUI();
}

END_NAMESPACE_DISTRHO
