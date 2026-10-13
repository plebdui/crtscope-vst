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
        if (!ev.press)
            return false;
        const int b = buttonAt(ev.pos.getX(), ev.pos.getY());
        if (b < 0) {
            if (fTube.mode != crt::kImage || ev.button != 1)
                return false;
            FileBrowserOptions opts;  // clicking the tube in image mode picks a file
            opts.title = "Pick a GIF or picture for the CRT";
            openFileBrowser(opts);
            return true;
        }
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
