// A tiny VST3 host for testing CRT Scope the way FL Studio uses it: load the
// plugin, open its window inside a host window, stream stereo audio through it
// on a separate thread, click a button in the window, then shut everything down.
// Windows only; built with mingw and run under Wine:
//   x86_64-w64-mingw32-g++ -std=c++17 -O1 -I../dpf/distrho/src minihost.cpp -o minihost.exe -static -lgdi32 -lole32 -lshell32 -luuid
//   wine minihost.exe 'C:\path\CRT-Scope.vst3\Contents\x86_64-win\CRT-Scope.vst3' ['C:\path\picture.gif']
// With a picture path it also drags that file onto the window (the way Explorer
// does) and checks the plugin switched to IMAGE mode and saved the path.
#define _USE_MATH_DEFINES
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <algorithm>
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
#include "travesty/bstream.h"
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

// MIDI notes for the plugin, one block at a time
struct EventList {
    struct VT { v3_funknown u; v3_event_list l; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    std::vector<v3_event> events;
    static EventList* me(void* s) { return static_cast<EventList*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_event_list_iid)) {
            *obj = self; return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static uint32_t V3_API count(void* s) { return (uint32_t)me(s)->events.size(); }
    static v3_result V3_API get(void* s, int32_t idx, v3_event* e)
    {
        if (idx < 0 || idx >= (int32_t)me(s)->events.size()) return V3_INVALID_ARG;
        *e = me(s)->events[idx];
        return V3_OK;
    }
    static v3_result V3_API add(void*, v3_event*) { return V3_NOT_IMPLEMENTED; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {count, get, add}};
        return &vt;
    }
    EventList() : vtbl(table()) {}
};
std::atomic<int> gNoteOn{-1}, gNoteOff{-1};  // set by the test, sent with the next block
std::atomic<float> gPeak{0};                  // loudest output sample since the test reset it

// in-memory stream, for saving the plugin's state
struct Stream {
    struct VT { v3_funknown u; v3_bstream s; };
    const VT* vtbl;
    std::atomic<uint32_t> refs{1};
    std::vector<uint8_t> data;
    int64_t pos = 0;
    static Stream* me(void* s) { return static_cast<Stream*>(s); }
    static v3_result V3_API qi(void* self, const v3_tuid iid, void** obj)
    {
        if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_bstream_iid)) {
            *obj = self; addRef(self); return V3_OK;
        }
        *obj = nullptr; return V3_NO_INTERFACE;
    }
    static v3_result V3_API read(void* s, void* buf, int32_t n, int32_t* got)
    {
        const int32_t k = (int32_t)std::max<int64_t>(0, std::min<int64_t>(n, (int64_t)me(s)->data.size() - me(s)->pos));
        std::memcpy(buf, me(s)->data.data() + me(s)->pos, k); me(s)->pos += k;
        if (got) *got = k;
        return V3_OK;
    }
    static v3_result V3_API write(void* s, void* buf, int32_t n, int32_t* put)
    {
        auto& d = me(s)->data;
        if ((int64_t)d.size() < me(s)->pos + n) d.resize(me(s)->pos + n);
        std::memcpy(d.data() + me(s)->pos, buf, n); me(s)->pos += n;
        if (put) *put = n;
        return V3_OK;
    }
    static v3_result V3_API seek(void* s, int64_t p, int32_t mode, int64_t* res)
    {
        me(s)->pos = mode == V3_SEEK_SET ? p : mode == V3_SEEK_CUR ? me(s)->pos + p : (int64_t)me(s)->data.size() + p;
        if (res) *res = me(s)->pos;
        return V3_OK;
    }
    static v3_result V3_API tell(void* s, int64_t* p) { *p = me(s)->pos; return V3_OK; }
    static const VT* table()
    {
        static const VT vt = {{qi, addRef, noRelease}, {read, write, seek, tell}};
        return &vt;
    }
    Stream() : vtbl(table()) {}
};

// ---- a file being dragged, as Explorer offers it (CF_HDROP)
struct DragFile : IDataObject {
    std::wstring path;
    LONG refs = 1;
    explicit DragFile(const std::wstring& p) : path(p) {}
    virtual ~DragFile() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDataObject) { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }  // lives on the stack
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        if (QueryGetData(f) != S_OK) return DV_E_FORMATETC;
        const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GHND, bytes);
        DROPFILES* df = (DROPFILES*)GlobalLock(h);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        std::memcpy((char*)df + sizeof(DROPFILES), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        m->tymed = TYMED_HGLOBAL; m->hGlobal = h; m->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override
    {
        return (f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* o) override { o->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** e) override
    {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &f, e);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

// ---- the "mouse": lets go of the file after a few checks
struct DragHand : IDropSource {
    int polls = 0;
    virtual ~DragHand() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropSource) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD) override
    {
        if (esc) return DRAGDROP_S_CANCEL;
        Sleep(50);
        return ++polls >= 6 ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
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
    // a dropped picture shapes the sound (and so does one in a reopened project)
    std::atomic<bool> expectClean{true}, changed{false};
    bool reloaded = false;
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
            EventList midi;
            for (int k = 0; k < 2; ++k) {
                const int note = (k == 0 ? gNoteOn : gNoteOff).exchange(-1);
                if (note < 0) continue;
                v3_event e = {};
                e.type = k == 0 ? V3_EVENT_NOTE_ON : V3_EVENT_NOTE_OFF;
                if (k == 0) { e.note_on.pitch = (int16_t)note; e.note_on.velocity = 1.0f; e.note_on.note_id = -1; }
                else { e.note_off.pitch = (int16_t)note; e.note_off.note_id = -1; }
                midi.events.push_back(e);
            }
            v3_process_data d = {};
            d.input_params = (v3_param_changes**)&changes;
            d.input_events = (v3_event_list**)&midi;
            d.process_mode = 0; d.symbolic_sample_size = 0; d.nframes = 512;
            d.num_input_buses = 1; d.num_output_buses = 1; d.inputs = &in; d.outputs = &out;
            if (v3_cpp_obj(proc)->process(proc, &d) != V3_OK) audioOk = false;
            for (int i = 0; i < 512; ++i)
                if (outL[i] != inL[i] || outR[i] != inR[i]) {
                    if (expectClean) audioOk = false;  // must pass audio through untouched
                    else changed = true;
                }
            float pk = gPeak;
            for (int i = 0; i < 512; ++i) pk = std::max({pk, std::fabs(outL[i]), std::fabs(outR[i])});
            gPeak = pk;
            ++blocks;
            const double wait = start + blocks * 512 / 48000.0 - now();
            if (wait > 0) Sleep((DWORD)(wait * 1000));
        }
    });

    // ---- reopening a saved project: load the state before the window opens
    if (const char* load = std::getenv("MINIHOST_LOAD")) {
        Stream st;
        if (FILE* f = std::fopen(load, "rb")) {
            int c;
            while ((c = std::fgetc(f)) != EOF) st.data.push_back((uint8_t)c);
            std::fclose(f);
        }
        CHECK(!st.data.empty(), "saved state file");
        CHECK(v3_cpp_obj(comp)->set_state(comp, (v3_bstream**)&st) == V3_OK, "set_state");
        st.pos = 0;
        v3_cpp_obj(ctrl)->set_component_state(ctrl, (v3_bstream**)&st);
        std::printf("loaded saved state (%zu bytes)\n", st.data.size());
        reloaded = true;
        expectClean = false;
    }

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
    const double opened = now();
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
    const bool modeClicked = gEdits > 0 && gLastParam == 0;

    // ---- turn the GLOW knob: press on it, drag up, let go
    const int knobX = (int)(cr.right * (2.9 + 0.45) / 11.7), knobY = cr.bottom - 84 + 44;  // strip column weights add up to 11.7
    PostMessageA(child, WM_MOUSEMOVE, 0, MAKELPARAM(knobX, knobY));
    PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(knobX, knobY));
    for (int i = 1; i <= 10; ++i) {
        PostMessageA(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(knobX, knobY - i * 6));
        pump(0.05);
    }
    PostMessageA(child, WM_LBUTTONUP, 0, MAKELPARAM(knobX, knobY - 60));
    pump(0.5);
    std::printf("after dragging GLOW up: param %d -> %.3f (default 1.0x = 0.048)\n", gLastParam.load(), gLastValue.load());
    const bool knobTurned = gLastParam == 2 && gLastValue > 0.1;  // host value is linear: (v - 0.05) / 19.95

    // ---- drag a picture onto the window, like dropping a file from Explorer
    bool dropped = true, saved = true;
    if (argc > 2) {
        OleInitialize(nullptr);
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wpath, MAX_PATH);
        DragFile file(wpath);
        DragHand hand;
        RECT sr;
        GetWindowRect(child, &sr);
        SetCursorPos((sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2 - 40);
        pump(0.3);
        DWORD effect = 0;
        expectClean = false;
        const HRESULT hr = DoDragDrop(&file, &hand, DROPEFFECT_COPY, &effect);
        std::printf("drag and drop: hr=0x%lx effect=%lu, param %d -> %.3f\n", (unsigned long)hr, effect,
                    gLastParam.load(), gLastValue.load());
        dropped = hr == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY && gLastParam == 0 && gLastValue == 0.75;  // IMAGE = mode 3 of 0..4
        pump(6.0);  // trace it and show it
        std::printf("sound after the drop: %s\n", changed ? "shaped by the picture" : "UNCHANGED");
        dropped = dropped && changed;
        if (const char* shot = std::getenv("MINIHOST_SHOT")) saveWindow(win, (std::string(shot) + "3.bmp").c_str());

        // the project should now remember the file
        Stream st;
        v3_cpp_obj(comp)->get_state(comp, (v3_bstream**)&st);
        const std::string blob(st.data.begin(), st.data.end());
        const std::string name = std::string(argv[2]).substr(std::string(argv[2]).find_last_of("\\/") + 1);
        saved = blob.find(name) != std::string::npos;
        if (const char* keep = std::getenv("MINIHOST_SAVE"))
            if (FILE* f = std::fopen(keep, "wb")) { std::fwrite(blob.data(), 1, blob.size(), f); std::fclose(f); }
        std::printf("saved state: %zu bytes, contains the file name: %s\n", blob.size(), saved ? "yes" : "NO");

        // ---- next mode is EQUATION; click the tube's right half for the next curve
        PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        PostMessageA(child, WM_LBUTTONUP, 0, pos);
        pump(1.5);
        const bool eqMode = gLastParam == 0 && gLastValue == 1.0;
        const LPARAM right = MAKELPARAM(cr.right * 3 / 4, (cr.bottom - 84) / 2);
        PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, right);
        PostMessageA(child, WM_LBUTTONUP, 0, right);
        pump(3.5);
        std::printf("EQUATION mode: %s, clicking the tube -> param %d = %.3f\n", eqMode ? "on" : "NOT ON",
                    gLastParam.load(), gLastValue.load());
        dropped = dropped && eqMode && gLastParam == 10 && std::fabs(gLastValue - 0.1) < 1e-6;

        // ---- turn SHAPE (last knob) all the way up: the curve itself becomes the sound
        const int shapeX = (int)(cr.right * (10.0 + 0.45) / 11.7);
        PostMessageA(child, WM_MOUSEMOVE, 0, MAKELPARAM(shapeX, knobY));
        PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(shapeX, knobY));
        for (int i = 1; i <= 10; ++i) {
            PostMessageA(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(shapeX, knobY - i * 25));
            pump(0.05);
        }
        PostMessageA(child, WM_LBUTTONUP, 0, MAKELPARAM(shapeX, knobY - 250));
        pump(1.0);
        const bool shapeUp = gLastParam == 11 && gLastValue == 1.0;
        std::printf("after dragging SHAPE up: param %d -> %.3f\n", gLastParam.load(), gLastValue.load());
        dropped = dropped && shapeUp;

        // ---- MIDI switch on: the curve plays only while a note is held
        const LPARAM midiSw = MAKELPARAM((int)(cr.right * (10.9 + 0.4) / 11.7), knobY);
        PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, midiSw);
        PostMessageA(child, WM_LBUTTONUP, 0, midiSw);
        pump(1.0);
        const bool midiOn = gLastParam == 12 && gLastValue == 1.0;
        std::printf("(%.1f s after the window opened: MIDI on, no note yet)\n", now() - opened);
        gPeak = 0;
        pump(0.5);
        const float quiet = gPeak;
        gNoteOn = 57;
        pump(0.1);
        gPeak = 0;
        pump(0.5);
        const float playing = gPeak;
        std::printf("(%.1f s after the window opened: note A3 held)\n", now() - opened);
        gNoteOff = 57;
        pump(1.0);
        gPeak = 0;
        pump(0.5);
        const float released = gPeak;
        std::printf("MIDI switch: param %d -> %.3f; output peak no note %.3f, note held %.3f, released %.3f\n",
                    gLastParam.load(), gLastValue.load(), quiet, playing, released);
        dropped = dropped && midiOn && quiet < 0.01f && playing > 0.8f && playing <= 0.95f && released < 0.01f;  // full velocity peaks at about -1 dB

        // ---- drop a second picture (argv[3], or the same one again): it should replace the first
        const char* second = argc > 3 ? argv[3] : argv[2];
        MultiByteToWideChar(CP_UTF8, 0, second, -1, wpath, MAX_PATH);
        DragFile file2(wpath);
        DragHand hand2;
        SetCursorPos((sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2 - 40);
        pump(0.3);
        effect = 0;
        const HRESULT hr2 = DoDragDrop(&file2, &hand2, DROPEFFECT_COPY, &effect);
        pump(6.0);
        if (const char* shot = std::getenv("MINIHOST_SHOT")) saveWindow(win, (std::string(shot) + "4.bmp").c_str());
        Stream st2;
        v3_cpp_obj(comp)->get_state(comp, (v3_bstream**)&st2);
        const std::string blob2(st2.data.begin(), st2.data.end());
        const std::string name2 = std::string(second).substr(std::string(second).find_last_of("\\/") + 1);
        const bool again = hr2 == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY && gLastParam == 0 && gLastValue == 0.75
                        && blob2.find(name2) != std::string::npos;
        std::printf("second drop: hr=0x%lx effect=%lu, param %d -> %.3f, saved name: %s\n", (unsigned long)hr2, effect,
                    gLastParam.load(), gLastValue.load(), blob2.find(name2) != std::string::npos ? "yes" : "NO");
        dropped = dropped && again;

        // ---- click the tube (IMAGE mode) twice: each time the file picker opens, pick a file
        bool picked = true;
        for (int k = 0; k < 2; ++k) {
            const char* file = k == 0 ? (argc > 4 ? argv[4] : argv[2]) : second;  // argv[4]: a big GIF, still tracing when the second is picked
            const LPARAM mid = MAKELPARAM(cr.right / 2, (cr.bottom - 84) / 2);
            PostMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, mid);
            PostMessageA(child, WM_LBUTTONUP, 0, mid);
            HWND dlg = nullptr;
            for (int i = 0; i < 50 && !dlg; ++i) {
                pump(0.1);
                dlg = FindWindowA(nullptr, "Pick a GIF or picture for the CRT");
            }
            if (!dlg) {
                std::printf("file picker %d: DID NOT OPEN\n", k + 1);
                picked = false;
                break;
            }
            SetDlgItemTextA(dlg, 0x47c, file);  // file name box
            SetDlgItemTextA(dlg, 0x480, file);
            PostMessageA(dlg, WM_COMMAND, IDOK, 0);
            pump(k == 0 && argc > 4 ? 0.5 : 4.0);
            Stream stk;
            v3_cpp_obj(comp)->get_state(comp, (v3_bstream**)&stk);
            const std::string b(stk.data.begin(), stk.data.end());
            const std::string nm = std::string(file).substr(std::string(file).find_last_of("\\/") + 1);
            const bool ok = b.find(nm) != std::string::npos;
            std::printf("file picker %d: picked %s, saved: %s\n", k + 1, nm.c_str(), ok ? "yes" : "NO");
            picked = picked && ok;
        }
        dropped = dropped && picked;
    }

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
    if (reloaded) std::printf("sound after reopening: %s\n", changed ? "shaped by the picture" : "UNCHANGED");
    CHECK(!reloaded || changed, "a reopened project's picture should shape the sound");
    CHECK(modeClicked, "clicking MODE should change parameter 0");
    CHECK(knobTurned, "dragging the GLOW knob up should raise parameter 2");
    CHECK(dropped, "dropping a picture should switch to IMAGE mode, then EQUATION mode should step curves");
    CHECK(saved, "the dropped picture should be saved with the project");
    std::printf("ALL OK\n");
    return 0;
}
