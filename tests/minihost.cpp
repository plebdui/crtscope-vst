// A tiny VST3 host for testing CRT Scope the way FL Studio uses it: load the
// plugin, open its window inside a host window, stream stereo audio through it
// on a separate thread, click a button in the window, then shut everything down.
// Windows only; built with mingw and run under Wine:
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 -I../dpf/distrho/src minihost.cpp -o minihost.exe -static -lgdi32
//   wine minihost.exe 'C:\path\CRT-Scope.vst3\Contents\x86_64-win\CRT-Scope.vst3'
#define _USE_MATH_DEFINES
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "travesty/audio_processor.h"
#include "travesty/base.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"
#include "travesty/host.h"
#include "travesty/message.h"
#include "travesty/view.h"

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); std::fflush(stdout); return 1; } } while (0)

static std::atomic<int> gEdits{0};
static std::mutex gEditLock;
static std::vector<std::pair<v3_param_id, double>> gPendingEdits;
static std::atomic<int> gLastParam{-1};
static std::atomic<double> gLastValue{-1};

// ---- minimal COM-style objects the plugin expects from a host ------------------------

template <class VT>
struct Obj {
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
};

static uint32_t V3_API addRef(void* self) { return ++static_cast<Obj<void>*>(self)->refs; }
static uint32_t V3_API noRelease(void*) { return 1; }  // host objects live for the whole test

// attribute list: a bag of named values
struct AttrList {
    struct VT { v3_funknown u; v3_attribute_list a; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::vector<int16_t>> strings;
    std::map<std::string, std::vector<uint8_t>> blobs;
    static AttrList* me(void* s) { return static_cast<AttrList*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_attribute_list_iid)) {
            *obj = self; addRef(self); return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static uint32_t V3_API unref(void* self)
    {
        const uint32_t r = --me(self)->refs;
        if (r == 0) delete me(self);
        return r;
    }
    static v3_result V3_API setInt(void* s, const char* id, int64_t v) { me(s)->ints[id] = v; return V3_OK; }
    static v3_result V3_API getInt(void* s, const char* id, int64_t* v)
    {
        auto it = me(s)->ints.find(id); if (it == me(s)->ints.end()) return V3_INVALID_ARG; *v = it->second; return V3_OK;
    }
    static v3_result V3_API setFloat(void* s, const char* id, double v) { me(s)->floats[id] = v; return V3_OK; }
    static v3_result V3_API getFloat(void* s, const char* id, double* v)
    {
        auto it = me(s)->floats.find(id); if (it == me(s)->floats.end()) return V3_INVALID_ARG; *v = it->second; return V3_OK;
    }
    static v3_result V3_API setString(void* s, const char* id, const int16_t* str)
    {
        std::vector<int16_t> v; while (*str) v.push_back(*str++); v.push_back(0); me(s)->strings[id] = v; return V3_OK;
    }
    static v3_result V3_API getString(void* s, const char* id, int16_t* out, uint32_t size)
    {
        auto it = me(s)->strings.find(id); if (it == me(s)->strings.end() || size == 0) return V3_INVALID_ARG;
        const uint32_t n = std::min<uint32_t>(size / sizeof(int16_t), (uint32_t)it->second.size());
        std::memcpy(out, it->second.data(), n * sizeof(int16_t)); out[n - 1] = 0; return V3_OK;
    }
    static v3_result V3_API setBinary(void* s, const char* id, const void* data, uint32_t size)
    {
        me(s)->blobs[id].assign((const uint8_t*)data, (const uint8_t*)data + size); return V3_OK;
    }
    static v3_result V3_API getBinary(void* s, const char* id, const void** data, uint32_t* size)
    {
        auto it = me(s)->blobs.find(id); if (it == me(s)->blobs.end()) return V3_INVALID_ARG;
        *data = it->second.data(); *size = (uint32_t)it->second.size(); return V3_OK;
    }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, unref}, {setInt, getInt, setFloat, getFloat, setString, getString, setBinary, getBinary}};
        return &vt;
    }
    AttrList() : vtbl(table()) {}
};

struct Message {
    struct VT { v3_funknown u; v3_message m; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    std::string id;
    AttrList* attrs = new AttrList();
    static Message* me(void* s) { return static_cast<Message*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_message_iid)) {
            *obj = self; addRef(self); return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static uint32_t V3_API unref(void* self)
    {
        const uint32_t r = --me(self)->refs;
        if (r == 0) { AttrList::unref(me(self)->attrs); delete me(self); }
        return r;
    }
    static const char* V3_API getId(void* s) { return me(s)->id.c_str(); }
    static void V3_API setId(void* s, const char* id) { me(s)->id = id ? id : ""; }
    static v3_attribute_list** V3_API getAttrs(void* s) { return (v3_attribute_list**)me(s)->attrs; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, unref}, {getId, setId, getAttrs}};
        return &vt;
    }
    Message() : vtbl(table()) {}
};

struct HostApp {
    struct VT { v3_funknown u; v3_host_application h; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_host_application_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static v3_result V3_API getName(void*, v3_str_128 name)
    {
        const char* n = "CRT minihost"; int i = 0; for (; n[i]; ++i) name[i] = n[i]; name[i] = 0; return V3_OK;
    }
    static v3_result V3_API createInstance(void*, v3_tuid cid, v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(cid, v3_message_iid) && v3_tuid_match(iid, v3_message_iid)) {
            *obj = new Message(); return V3_OK;
        }
        if (v3_tuid_match(cid, v3_attribute_list_iid) && v3_tuid_match(iid, v3_attribute_list_iid)) {
            *obj = new AttrList(); return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {getName, createInstance}};
        return &vt;
    }
    HostApp() : vtbl(table()) {}
};

// receives "the user changed a knob in the plugin window" (FL records automation from these)
struct Handler {
    struct VT { v3_funknown u; v3_component_handler h; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_component_handler_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static v3_result V3_API begin(void*, v3_param_id) { return V3_OK; }
    static v3_result V3_API perform(void*, v3_param_id id, double v)
    {
        ++gEdits; gLastParam = (int)id; gLastValue = v;
        std::lock_guard<std::mutex> g(gEditLock);
        gPendingEdits.push_back({id, v});
        return V3_OK;
    }
    static v3_result V3_API end(void*, v3_param_id) { return V3_OK; }
    static v3_result V3_API restart(void*, int32_t) { return V3_OK; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {begin, perform, end, restart}};
        return &vt;
    }
    Handler() : vtbl(table()) {}
};

struct Frame {
    struct VT { v3_funknown u; v3_plugin_frame f; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_frame_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static v3_result V3_API resize(void*, v3_plugin_view**, v3_view_rect*) { return V3_OK; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {resize}};
        return &vt;
    }
    Frame() : vtbl(table()) {}
};

// parameter changes handed to process(): one value per parameter, like a host
// passing on what the user just did in the plugin window
struct ParamQueue {
    struct VT { v3_funknown u; v3_param_value_queue q; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    v3_param_id id = 0;
    double value = 0;
    static ParamQueue* me(void* s) { return static_cast<ParamQueue*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_value_queue_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static v3_param_id V3_API getId(void* s) { return me(s)->id; }
    static int32_t V3_API count(void*) { return 1; }
    static v3_result V3_API getPoint(void* s, int32_t idx, int32_t* offset, double* v)
    {
        if (idx != 0) return V3_INVALID_ARG;
        *offset = 0; *v = me(s)->value; return V3_OK;
    }
    static v3_result V3_API addPoint(void*, int32_t, double, int32_t*) { return V3_NOT_IMPLEMENTED; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {getId, count, getPoint, addPoint}};
        return &vt;
    }
    ParamQueue() : vtbl(table()) {}
};

struct ParamChanges {
    struct VT { v3_funknown u; v3_param_changes c; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    std::vector<ParamQueue*> queues;
    static ParamChanges* me(void* s) { return static_cast<ParamChanges*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_param_changes_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static int32_t V3_API count(void* s) { return (int32_t)me(s)->queues.size(); }
    static v3_param_value_queue** V3_API get(void* s, int32_t idx)
    {
        return idx >= 0 && idx < (int32_t)me(s)->queues.size() ? (v3_param_value_queue**)me(s)->queues[idx] : nullptr;
    }
    static v3_param_value_queue** V3_API add(void*, const v3_param_id*, int32_t*) { return nullptr; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {count, get, add}};
        return &vt;
    }
    ParamChanges() : vtbl(table()) {}
};

// ---- the test ---------------------------------------------------------------------------

// (GetTickCount runs slow under some Wine builds, so time everything with steady_clock)
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void pump(double seconds)
{
    const double end = now() + seconds;
    while (now() < end) {
        MSG msg;
        while (now() < end && PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(5);
    }
}

// screenshot of a window's client area, as a BMP (to check what the plugin drew, buttons included)
static void saveWindow(HWND w, const char* path)
{
    RECT rc;
    GetClientRect(w, &rc);
    const int W = rc.right, H = rc.bottom;
    HDC src = GetDC(w), mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, W, H);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, W, H, src, 0, 0, SRCCOPY);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    const int stride = (W * 3 + 3) & ~3;
    std::vector<uint8_t> px(stride * H);
    GetDIBits(mem, bmp, 0, H, px.data(), &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(&fh, sizeof(fh), 1, f); std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        std::fwrite(px.data(), 1, px.size(), f); std::fclose(f);
    }
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(w, src);
}

static BOOL CALLBACK findChild(HWND h, LPARAM out)
{
    *(HWND*)out = h;
    return FALSE;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(argc > 1, "usage: minihost path\\to\\plugin.vst3");
    HMODULE lib = LoadLibraryA(argv[1]);
    CHECK(lib, "LoadLibrary");
    if (auto init = (bool (*)())GetProcAddress(lib, "InitDll")) init();
    auto getFactory = (v3_plugin_factory** (*)())GetProcAddress(lib, "GetPluginFactory");
    CHECK(getFactory, "GetPluginFactory export");
    v3_plugin_factory** factory = getFactory();
    CHECK(factory, "factory");

    static HostApp host;
    v3_tuid componentCid = {};
    bool found = false;
    const int32_t nClasses = v3_cpp_obj(factory)->num_classes(factory);
    for (int32_t i = 0; i < nClasses; ++i) {
        v3_class_info info;
        v3_cpp_obj(factory)->get_class_info(factory, i, &info);
        std::printf("class %d: %s (%s)\n", i, info.name, info.category);
        if (!std::strcmp(info.category, "Audio Module Class")) { std::memcpy(componentCid, info.class_id, 16); found = true; }
    }
    CHECK(found, "audio module class");

    v3_component** comp = nullptr;
    CHECK(v3_cpp_obj(factory)->create_instance(factory, componentCid, v3_component_iid, (void**)&comp) == V3_OK && comp, "create component");
    CHECK(v3_cpp_obj_initialize(comp, (v3_funknown**)&host) == V3_OK, "component initialize");

    // the edit controller: either the component itself, or a separate class
    v3_edit_controller** ctrl = nullptr;
    bool separate = false;
    if (v3_cpp_obj_query_interface(comp, v3_edit_controller_iid, &ctrl) != V3_OK || !ctrl) {
        v3_tuid ctrlCid;
        CHECK(v3_cpp_obj(comp)->get_controller_class_id(comp, ctrlCid) == V3_OK, "controller class id");
        CHECK(v3_cpp_obj(factory)->create_instance(factory, ctrlCid, v3_edit_controller_iid, (void**)&ctrl) == V3_OK && ctrl, "create controller");
        CHECK(v3_cpp_obj_initialize(ctrl, (v3_funknown**)&host) == V3_OK, "controller initialize");
        separate = true;
        v3_connection_point **a = nullptr, **b = nullptr;
        if (v3_cpp_obj_query_interface(comp, v3_connection_point_iid, &a) == V3_OK &&
            v3_cpp_obj_query_interface(ctrl, v3_connection_point_iid, &b) == V3_OK) {
            v3_cpp_obj(a)->connect(a, b);
            v3_cpp_obj(b)->connect(b, a);
        }
    }
    std::printf("controller: %s\n", separate ? "separate class" : "same object as the component");
    static Handler handler;
    v3_cpp_obj(ctrl)->set_component_handler(ctrl, (v3_component_handler**)&handler);
    std::printf("parameters: %d\n", v3_cpp_obj(ctrl)->get_parameter_count(ctrl));

    v3_audio_processor** proc = nullptr;
    CHECK(v3_cpp_obj_query_interface(comp, v3_audio_processor_iid, &proc) == V3_OK && proc, "audio processor");
    v3_process_setup setup = {0 /*realtime*/, 0 /*32-bit*/, 512, 48000.0};
    CHECK(v3_cpp_obj(proc)->setup_processing(proc, &setup) == V3_OK, "setup_processing");
    CHECK(v3_cpp_obj(comp)->set_active(comp, true) == V3_OK, "set_active");
    v3_cpp_obj(proc)->set_processing(proc, true);

    // ---- audio thread: a 3:2 stereo test tone in 512-frame blocks, in real time
    std::atomic<bool> running{true};
    std::atomic<int> blocks{0};
    std::atomic<bool> audioOk{true};
    std::thread audio([&] {
        std::vector<float> inL(512), inR(512), outL(512), outR(512);
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        v3_audio_bus_buffers in = {}, out = {};
        in.num_channels = 2; in.channel_buffers_32 = ins;
        out.num_channels = 2; out.channel_buffers_32 = outs;
        double t = 0;
        const double start = now();
        while (running) {
            for (int i = 0; i < 512; ++i, t += 1 / 48000.0) {
                inL[i] = 0.6f * (float)std::sin(2 * M_PI * 220 * t);
                inR[i] = 0.6f * (float)std::sin(2 * M_PI * 330.4 * t + 0.5);
            }
            ParamChanges changes;
            std::vector<std::unique_ptr<ParamQueue>> queues;
            {
                std::lock_guard<std::mutex> g(gEditLock);
                for (const auto& e : gPendingEdits) {
                    queues.emplace_back(new ParamQueue());
                    queues.back()->id = e.first;
                    queues.back()->value = e.second;
                    changes.queues.push_back(queues.back().get());
                }
                gPendingEdits.clear();
            }
            v3_process_data d = {};
            d.input_params = (v3_param_changes**)&changes;
            d.process_mode = 0; d.symbolic_sample_size = 0; d.nframes = 512;
            d.num_input_buses = 1; d.num_output_buses = 1; d.inputs = &in; d.outputs = &out;
            if (v3_cpp_obj(proc)->process(proc, &d) != V3_OK) audioOk = false;
            for (int i = 0; i < 512; ++i)
                if (outL[i] != inL[i] || outR[i] != inR[i]) audioOk = false;  // must pass audio through untouched
            ++blocks;
            const double wait = start + blocks * 512 / 48000.0 - now();
            if (wait > 0) Sleep((DWORD)(wait * 1000));
        }
    });

    // ---- open the plugin window inside a host window
    v3_plugin_view** view = v3_cpp_obj(ctrl)->create_view(ctrl, "editor");
    CHECK(view, "create_view");
    CHECK(v3_cpp_obj(view)->is_platform_type_supported(view, "HWND") == V3_OK, "HWND supported");
    v3_view_rect r = {};
    v3_cpp_obj(view)->get_size(view, &r);
    std::printf("editor size %dx%d\n", r.right - r.left, r.bottom - r.top);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "minihost";
    RegisterClassA(&wc);
    RECT wr = {0, 0, r.right - r.left, r.bottom - r.top};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND win = CreateWindowA("minihost", "CRT Scope test host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(win, "host window");
    static Frame frame;
    v3_cpp_obj(view)->set_frame(view, (v3_plugin_frame**)&frame);
    CHECK(v3_cpp_obj(view)->attached(view, win, "HWND") == V3_OK, "attached");
    std::printf("window open, streaming audio...\n");
    pump(4.0);
    if (const char* shot = std::getenv("MINIHOST_SHOT")) saveWindow(win, (std::string(shot) + "1.bmp").c_str());

    // ---- click the MODE button (bottom-left of the window) like a user would
    HWND child = nullptr;
    EnumChildWindows(win, findChild, (LPARAM)&child);
    CHECK(child, "plugin child window");
    RECT cr;
    GetClientRect(child, &cr);
    const LPARAM pos = MAKELPARAM(20, cr.bottom - 20);
    PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    PostMessageA(child, WM_LBUTTONUP, 0, pos);
    pump(4.0);
    if (const char* shot = std::getenv("MINIHOST_SHOT")) saveWindow(win, (std::string(shot) + "2.bmp").c_str());
    std::printf("after clicking MODE: %d edit(s), param %d -> %.3f\n", gEdits.load(), gLastParam.load(), gLastValue.load());

    // ---- close the window, stop audio, unload (what FL does when you remove the plugin)
    v3_cpp_obj(view)->removed(view);
    v3_cpp_obj_unref(view);
    DestroyWindow(win);
    running = false;
    audio.join();
    std::printf("audio blocks processed: %d, passthrough %s\n", blocks.load(), audioOk ? "exact" : "WRONG");
    v3_cpp_obj(proc)->set_processing(proc, false);
    v3_cpp_obj(comp)->set_active(comp, false);
    v3_cpp_obj_unref(proc);
    if (separate) {
        v3_cpp_obj_terminate(ctrl);
        v3_cpp_obj_unref(ctrl);
    } else {
        v3_cpp_obj_unref(ctrl);
    }
    v3_cpp_obj_terminate(comp);
    v3_cpp_obj_unref(comp);
    v3_cpp_obj_unref(factory);
    if (auto exitDll = (bool (*)())GetProcAddress(lib, "ExitDll")) exitDll();
    FreeLibrary(lib);
    CHECK(audioOk, "audio passthrough");
    CHECK(gEdits > 0 && gLastParam == 0, "clicking MODE should change parameter 0");
    std::printf("ALL OK\n");
    return 0;
}
