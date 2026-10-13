// The plugin window: the tube on top, a strip of buttons along the bottom.
// Left-click / scroll up = next or more, right-click / scroll down = back or less.
// Drop a GIF or picture on the tube (or click the tube in IMAGE mode) to trace it.
#include "DistrhoUI.hpp"
#include "CrtScopeShared.hpp"
#include "CrtTube.hpp"
#include "ImageTrace.hpp"
#include "OpenGL.hpp"
#include <memory>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <ole2.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

START_NAMESPACE_DISTRHO

// Traces a file on its own thread (a long GIF takes a few seconds)
struct Loader {
    std::string path;
    crt::Picture pic;
    std::string error;
    std::atomic<bool> cancel{false}, done{false};
    std::atomic<int> percent{0};
    std::thread thread;

    explicit Loader(const std::string& p) : path(p)
    {
        thread = std::thread([this] {
            error = crt::loadPicture(path, pic, cancel, percent);
            done.store(true);
        });
    }
    ~Loader()
    {
        cancel.store(true);
        if (thread.joinable())
            thread.join();
    }
};

#ifdef _WIN32
// Files dragged from Explorer (or FL's browser) onto the window
class DropTarget : public IDropTarget {
public:
    explicit DropTarget(std::function<void(const std::string&)> cb) : fCb(std::move(cb)) {}
    virtual ~DropTarget() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++fRefs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --fRefs;
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        fOk = data && data->QueryGetData(&fmt) == S_OK;
        *effect = fOk ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = fOk ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_NONE;
        FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg = {};
        if (!data || data->GetData(&fmt, &stg) != S_OK)
            return S_OK;
        std::string path;
        if (HDROP drop = (HDROP)GlobalLock(stg.hGlobal)) {
            wchar_t w[MAX_PATH * 4];
            if (DragQueryFileW(drop, 0, w, MAX_PATH * 4) > 0) {
                char u[MAX_PATH * 12];
                if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), nullptr, nullptr) > 0)
                    path = u;
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        if (!path.empty()) {
            *effect = DROPEFFECT_COPY;
            fCb(path);
        }
        return S_OK;
    }

private:
    std::function<void(const std::string&)> fCb;
    ULONG fRefs = 1;
    bool fOk = false;
};
#endif

static constexpr uint kTubeSize = 640;
static constexpr uint kStrip = 84;

// selectors step through choices, knobs turn (drag up/down), switches flip
enum Kind { kSelect, kKnob, kSwitch };
struct Button {
    uint32_t param;
    const char* caption;
    Kind kind;
};
static const Button kButtons[] = {
    {kMode, "MODE", kSelect},     {kPhosphor, "PHOSPHOR", kSelect}, {kAfterglow, "GLOW", kKnob},
    {kIntensity, "BEAM", kKnob},  {kGain, "GAIN", kKnob},           {kAutoGain, "AUTO", kSwitch},
    {kGraticule, "GRID", kSwitch}, {kFlaws, "FLAWS", kSelect},      {kWet, "WET", kKnob},
    {kSweep, "SWEEP", kKnob},
};
static constexpr int kNumButtons = sizeof(kButtons) / sizeof(kButtons[0]);
// how wide each button is, relative (phosphor names are long)
static const float kWeights[kNumButtons] = {1.0f, 1.9f, 0.9f, 0.9f, 0.9f, 0.8f, 0.8f, 1.0f, 0.8f, 1.0f};

class CrtScopeUI : public UI {
public:
    CrtScopeUI()
        : UI(kTubeSize, kTubeSize + kStrip)
    {
        loadSharedResources();
        for (int i = 0; i < kParamCount; ++i)
            fValues[i] = 0;
        fValues[kAfterglow] = fValues[kIntensity] = fValues[kGain] = 1;
        fValues[kAutoGain] = fValues[kGraticule] = 1;
        fValues[kSweep] = 20;
        fValues[kWet] = 1;
        apply();
        fLast = std::chrono::steady_clock::now();
        if (const char* p = std::getenv("CRTSCOPE_DUMP"))  // testing: save a frame to this file
            fDumpPath = p;
    }

    ~CrtScopeUI() override
    {
#ifdef _WIN32
        if (fDropHwnd)
            RevokeDragDrop(fDropHwnd);
        if (fDrop)
            fDrop->Release();
        if (fOleInit)
            OleUninitialize();
#endif
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

    void stateChanged(const char* key, const char* value) override
    {
        if (std::strcmp(key, "image") == 0 && value && *value)
            loadImage(value);
    }

    void uiFileBrowserSelected(const char* filename) override
    {
        if (filename)
            pickFile(filename);
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
        registerDrop();
        if (fLoader && fLoader->done.load()) {
            std::unique_ptr<Loader> l = std::move(fLoader);
            l->thread.join();
            if (l->error.empty()) {
                fPic.reset(new crt::Picture(std::move(l->pic)));
                fPicTime = 0;
            } else {
                fPicError = l->error;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - fLast).count();
        fLast = now;
        fTube.shapeN = 0;
        if (fTube.mode == crt::kImage && fPic) {
            fPicTime += std::clamp(dt, 0.0f, 0.25f);
            if (const crt::Frame* fr = fPic->at(fPicTime)) {
                fTube.shapeX = fr->x.data();
                fTube.shapeY = fr->y.data();
                fTube.shapeBlank = fr->blank.data();
                fTube.shapeN = (int)fr->x.size();
            }
            fTube.shapeMoving = fPic->animated();
        }
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

        if (fTube.mode == crt::kImage)
            drawImageHint((float)w, (float)h, (float)scale);
        drawStrip((float)h, (float)stripH, (float)scale);

        // testing hook: snapshot the whole window (buttons included) 3s, 7s and 13s after opening
        if (!fDumpPath.empty() && fDumps < 3) {
            static const float when[3] = {3.0f, 7.0f, 13.0f};
            const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - fOpened).count();
            if (age > when[fDumps]) {
                static_cast<NanoVG*>(this)->endFrame();
                dumpFrontBuffer(fDumpPath + ".full" + std::to_string(fDumps + 1) + ".ppm");
                static_cast<NanoVG*>(this)->beginFrame(getWidth(), getHeight());
                ++fDumps;
            }
        }
    }

    bool onMouse(const MouseEvent& ev) override
    {
        if (!ev.press) {
            if (fDrag < 0)
                return false;
            editParameter(kButtons[fDrag].param, false);
            fDrag = -1;
            return true;
        }
        const int b = buttonAt(ev.pos.getX(), ev.pos.getY());
        if (b < 0) {
            if (fTube.mode != crt::kImage || ev.button != 1)
                return false;
            FileBrowserOptions opts;  // clicking the tube in image mode picks a file
            opts.title = "Pick a GIF or picture for the CRT";
            openFileBrowser(opts);
            return true;
        }
        if (kButtons[b].kind == kKnob) {
            const uint32_t p = kButtons[b].param;
            if (ev.button == 3) {  // right-click: back to the default
                editParameter(p, true);
                setValue(p, knobDefault(p));
                editParameter(p, false);
                return true;
            }
            if (ev.button != 1)
                return false;
            fDrag = b;  // left: drag up/down to turn
            fDragY = ev.pos.getY();
            fDragNorm = toNorm(p, fValues[p]);
            editParameter(p, true);
            return true;
        }
        nudge(b, ev.button == 3 ? -1 : 1);  // right button goes back
        return true;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        if (fDrag < 0)
            return false;
        const uint32_t p = kButtons[fDrag].param;
        const float n = std::clamp(fDragNorm + (float)(fDragY - ev.pos.getY()) / (200.0f * (float)getScaleFactor()), 0.0f, 1.0f);
        setValue(p, fromNorm(p, n));
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
    std::unique_ptr<Loader> fLoader;
    std::unique_ptr<crt::Picture> fPic;
    std::string fPicPath, fPicError;
    double fPicTime = 0;
#ifdef _WIN32
    DropTarget* fDrop = nullptr;
    HWND fDropHwnd = nullptr;
    bool fOleInit = false, fDropTried = false;
#endif

    void registerDrop()
    {
#ifdef _WIN32
        if (fDropTried)
            return;
        fDropTried = true;
        HWND hwnd = (HWND)getWindow().getNativeWindowHandle();
        if (!hwnd)
            return;
        fOleInit = SUCCEEDED(OleInitialize(nullptr));  // the host has normally done this already
        fDrop = new DropTarget([this](const std::string& p) { pickFile(p); });
        if (RegisterDragDrop(hwnd, fDrop) == S_OK)
            fDropHwnd = hwnd;
        else
            std::fprintf(stderr, "CRT Scope: drag and drop unavailable\n");
#endif
    }

    // a file the user chose: show it, switch to image mode, save it with the project
    void pickFile(const std::string& path)
    {
        fPicPath.clear();  // reload even if it's the same file (it may have changed)
        loadImage(path);
        setState("image", path.c_str());
        if (std::lround(fValues[kMode]) != crt::kImage) {
            fValues[kMode] = crt::kImage;
            apply();
            editParameter(kMode, true);
            setParameterValue(kMode, crt::kImage);
            editParameter(kMode, false);
        }
    }

    void loadImage(const std::string& path)
    {
        if (path == fPicPath)
            return;
        fPicPath = path;
        fPicError.clear();
        fLoader.reset(new Loader(path));  // cancels and waits for any earlier one
        repaint();
    }

    void drawImageHint(float w, float h, float scale)
    {
        char buf[160];
        const char* line2 = nullptr;
        if (fLoader) {
            std::snprintf(buf, sizeof(buf), "TRACING  %d%%", fLoader->percent.load());
        } else if (!fPicError.empty()) {
            std::snprintf(buf, sizeof(buf), "%s", fPicError.c_str());
            line2 = "drop another GIF or picture here";
        } else if (!fPic) {
            std::snprintf(buf, sizeof(buf), "DROP A GIF OR PICTURE HERE");
            line2 = "or click to choose one";
        } else {
            return;
        }
        const crt::Phosphor& ph = crt::phosphors()[std::clamp((int)std::lround(fValues[kPhosphor]), 0, 18)];
        fontFace(NANOVG_DEJAVU_SANS_TTF);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        fontSize(16 * scale);
        fillColor(Color((int)ph.glow[0], (int)ph.glow[1], (int)ph.glow[2], 220));
        text(w / 2, h / 2 - (line2 ? 10 * scale : 0), buf, nullptr);
        if (line2) {
            fontSize(11.5f * scale);
            fillColor(Color(120, 128, 124));
            text(w / 2, h / 2 + 14 * scale, line2, nullptr);
        }
    }

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

    int fDrag = -1;
    double fDragY = 0;
    float fDragNorm = 0;

    // knobs: where the value sits on the dial, 0..1 (multipliers turn on a log scale)
    static float toNorm(uint32_t p, float v)
    {
        if (p == kWet) return std::clamp(v, 0.0f, 1.0f);
        const float lo = p == kSweep ? 1.0f : 0.05f, hi = p == kSweep ? 200.0f : 20.0f;
        return std::clamp(std::log(std::max(v, lo) / lo) / std::log(hi / lo), 0.0f, 1.0f);
    }
    static float fromNorm(uint32_t p, float n)
    {
        if (p == kWet) return n;
        const float lo = p == kSweep ? 1.0f : 0.05f, hi = p == kSweep ? 200.0f : 20.0f;
        return lo * std::pow(hi / lo, n);
    }
    static float knobDefault(uint32_t p) { return p == kSweep ? 20.0f : 1.0f; }

    void setValue(uint32_t p, float v)
    {
        fValues[p] = v;
        apply();
        setParameterValue(p, v);
        repaint();
    }

    // Change a setting by one step and tell the host (so FL records automation)
    void nudge(int b, int dir)
    {
        const uint32_t p = kButtons[b].param;
        float v = fValues[p];
        switch (p) {
        case kMode:      v = (float)(((int)v + dir + 4) % 4); break;
        case kPhosphor:  v = (float)(((int)v + dir + 19) % 19); break;
        case kFlaws:     v = (float)(((int)v + dir + 3) % 3); break;
        case kAutoGain:
        case kGraticule: v = v > 0.5f ? 0.0f : 1.0f; break;
        case kSweep:     v = std::clamp(v * (dir > 0 ? 1.5f : 1 / 1.5f), 1.0f, 200.0f); break;
        case kWet:       v = std::clamp(std::round(v * 20 + dir) / 20, 0.0f, 1.0f); break;
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
            static const char* m[] = {"XY", "GONIO", "WAVE", "IMAGE"};
            std::snprintf(out, n, "%s", m[std::clamp((int)std::lround(v), 0, 3)]);
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
        case kWet:       std::snprintf(out, n, "%d%%", (int)std::lround(v * 100)); break;
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
        const Color label(120, 128, 124), off(80, 84, 82);
        fontFace(NANOVG_DEJAVU_SANS_TTF);
        textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
        char buf[48];
        const bool picture = std::lround(fValues[kMode]) == crt::kImage && fPic;
        for (int b = 0; b < kNumButtons; ++b) {
            float x, w;
            buttonRect(b, x, w);
            const uint32_t p = kButtons[b].param;
            const bool dim = (p == kSweep && std::lround(fValues[kMode]) != crt::kWave)
                          || (p == kWet && std::lround(fValues[kFlaws]) == 0 && !picture);
            const Color col = dim ? off : accent;
            const float cx = x + w / 2, cy = y0 + 44 * scale;
            beginPath();
            roundedRect(x + 2 * scale, y0 + 4 * scale, w - 4 * scale, h - 8 * scale, 4 * scale);
            fillColor(Color(30, 32, 36));
            fill();
            strokeColor(Color(52, 54, 60));
            strokeWidth(1);
            stroke();
            fontSize(9 * scale);
            fillColor(label);
            text(cx, y0 + 15 * scale, kButtons[b].caption, nullptr);

            if (kButtons[b].kind == kKnob) {
                drawKnob(cx, cy, std::min(w * 0.27f, 15 * scale), toNorm(p, fValues[p]), col, scale);
                valueText(p, buf, sizeof(buf));
                fontSize(8.5f * scale);
                fillColor(dim ? off : label);
                text(cx, y0 + 70 * scale, buf, nullptr);
            } else if (kButtons[b].kind == kSwitch) {
                drawSwitch(cx, cy, fValues[p] > 0.5f, accent, scale);
            } else {
                // a little lit display window
                const float dw = w - 14 * scale, dh = 22 * scale;
                beginPath();
                roundedRect(cx - dw / 2, cy - dh / 2, dw, dh, 3 * scale);
                fillColor(Color(10, 12, 11));
                fill();
                strokeColor(Color(60, 62, 68));
                stroke();
                valueText(p, buf, sizeof(buf));
                fontSize(11 * scale);
                fillColor(col);
                text(cx, cy, buf, nullptr);
                fontSize(8 * scale);
                fillColor(off);
                text(cx, y0 + 70 * scale, "< click >", nullptr);
            }
        }
    }

    // a dial: dark knob, a lit arc showing the setting, a pointer
    void drawKnob(float cx, float cy, float r, float n, const Color& col, float scale)
    {
        const float a0 = 0.75f * 3.14159265f, a1 = 2.25f * 3.14159265f;
        const float a = a0 + (a1 - a0) * n;
        lineCap(ROUND);
        beginPath();
        arc(cx, cy, r + 4 * scale, a0, a1, CW);
        strokeColor(Color(50, 53, 58));
        strokeWidth(3 * scale);
        stroke();
        if (n > 0.002f) {
            beginPath();
            arc(cx, cy, r + 4 * scale, a0, a, CW);
            strokeColor(col);
            strokeWidth(3 * scale);
            stroke();
        }
        beginPath();
        circle(cx, cy, r);
        fillPaint(radialGradient(cx - r * 0.3f, cy - r * 0.4f, r * 0.1f, r * 1.2f, Color(78, 80, 86), Color(26, 27, 30)));
        fill();
        strokeColor(Color(12, 12, 14));
        strokeWidth(1);
        stroke();
        beginPath();
        moveTo(cx + std::cos(a) * r * 0.25f, cy + std::sin(a) * r * 0.25f);
        lineTo(cx + std::cos(a) * r * 0.85f, cy + std::sin(a) * r * 0.85f);
        strokeColor(Color(225, 228, 222));
        strokeWidth(2 * scale);
        stroke();
        lineCap(BUTT);
    }

    // a slide switch: the lever sits right and lights up when on
    void drawSwitch(float cx, float cy, bool on, const Color& col, float scale)
    {
        const float w = 34 * scale, h = 16 * scale;
        beginPath();
        roundedRect(cx - w / 2, cy - h / 2, w, h, h / 2);
        fillColor(on ? Color(col.red, col.green, col.blue, 0.35f) : Color(12, 13, 15));
        fill();
        strokeColor(on ? col : Color(60, 62, 68));
        strokeWidth(1);
        stroke();
        const float kx = on ? cx + w / 2 - h / 2 : cx - w / 2 + h / 2;
        beginPath();
        circle(kx, cy, h / 2 - 2 * scale);
        fillPaint(radialGradient(kx - 2 * scale, cy - 2 * scale, 1, h / 2, Color(235, 236, 232), Color(150, 152, 150)));
        fill();
        // little indicator lamp under the switch
        beginPath();
        circle(cx, cy + 16 * scale, 2.5f * scale);
        fillColor(on ? col : Color(40, 42, 44));
        fill();
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CrtScopeUI)
};

UI* createUI()
{
    return new CrtScopeUI();
}

END_NAMESPACE_DISTRHO
