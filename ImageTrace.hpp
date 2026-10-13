// Picture mode: load a GIF/PNG/JPG/BMP and turn every frame into a beam path
// (the outlines), the same way the terminal crtscope traces images and video.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_GIF
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "vendor/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace crt {

struct Frame {
    std::vector<float> x, y;      // -1..1, y up
    std::vector<uint8_t> blank;   // 1 = beam off on the way to this point
};

struct Picture {
    std::vector<Frame> frames;
    std::vector<float> starts;    // when each frame starts (seconds)
    float length = 0;             // whole loop (seconds)
    bool animated() const { return frames.size() > 1; }

    const Frame* at(double t) const
    {
        if (frames.empty())
            return nullptr;
        if (frames.size() == 1 || length <= 0)
            return &frames[0];
        const float tt = (float)std::fmod(t, (double)length);
        const size_t i = std::upper_bound(starts.begin(), starts.end(), tt) - starts.begin();
        return &frames[i == 0 ? 0 : i - 1];
    }
};

static bool readFile(const std::string& utf8path, std::vector<uint8_t>& out)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8path.c_str(), -1, &w[0], n);
    FILE* f = _wfopen(w.c_str(), L"rb");
#else
    FILE* f = std::fopen(utf8path.c_str(), "rb");
#endif
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// Box-filter shrink so the long side is at most `side` pixels.
static void shrink(const uint8_t* src, int w, int h, int side, std::vector<float>& lum,
                   std::vector<float>& alpha, int& ow, int& oh)
{
    const float s = std::min(1.0f, (float)side / std::max(w, h));
    ow = std::max(2, (int)std::lround(w * s));
    oh = std::max(2, (int)std::lround(h * s));
    lum.assign((size_t)ow * oh, 0);
    alpha.assign((size_t)ow * oh, 0);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            float l = 0, a = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = src + ((size_t)yy * w + xx) * 4;
                    const float pa = p[3] / 255.0f;
                    l += (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f * pa;
                    a += pa;
                }
            const float n = (float)((y1 - y0) * (x1 - x0));
            lum[(size_t)y * ow + x] = l / n;
            alpha[(size_t)y * ow + x] = a / n;
        }
    }
}

// Soften 3x3, then Sobel
static void edges(const std::vector<float>& a, int w, int h, std::vector<float>& gx, std::vector<float>& gy)
{
    auto at = [&](const std::vector<float>& v, int x, int y) {
        return v[(size_t)std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)];
    };
    std::vector<float> s((size_t)w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float t = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    t += at(a, x + dx, y + dy);
            s[(size_t)y * w + x] = t / 9;
        }
    gx.assign(s.size(), 0);
    gy.assign(s.size(), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            gx[i] = (at(s, x + 1, y) - at(s, x - 1, y)) * 2 + at(s, x + 1, y - 1) - at(s, x - 1, y - 1)
                  + at(s, x + 1, y + 1) - at(s, x - 1, y + 1);
            gy[i] = (at(s, x, y + 1) - at(s, x, y - 1)) * 2 + at(s, x - 1, y + 1) - at(s, x - 1, y - 1)
                  + at(s, x + 1, y + 1) - at(s, x + 1, y - 1);
        }
}

// One frame -> outline path. wholeFrame (animations) keeps the frame's own
// position and size so things don't jump around between frames.
static bool traceFrame(const uint8_t* rgba, int w0, int h0, bool wholeFrame, Frame& out)
{
    int w, h;
    std::vector<float> lum, alpha;
    shrink(rgba, w0, h0, 240, lum, alpha, w, h);
    std::vector<float> gx1, gy1, gx2, gy2;
    edges(lum, w, h, gx1, gy1);
    edges(alpha, w, h, gx2, gy2);
    const size_t N = (size_t)w * h;
    std::vector<float> gx(N), gy(N), mag(N);
    for (size_t i = 0; i < N; ++i) {
        const float m1 = std::hypot(gx1[i], gy1[i]), m2 = std::hypot(gx2[i], gy2[i]);
        const bool use2 = m2 > m1;
        gx[i] = use2 ? gx2[i] : gx1[i];
        gy[i] = use2 ? gy2[i] : gy1[i];
        mag[i] = use2 ? m2 : m1;
    }

    // keep only the crest of each edge so lines come out one pixel thin
    std::vector<float> nz;
    for (float m : mag)
        if (m > 0) nz.push_back(m);
    if (nz.empty())
        return false;
    const size_t k99 = std::min(nz.size() - 1, (size_t)(0.99 * (nz.size() - 1)));
    std::nth_element(nz.begin(), nz.begin() + k99, nz.end());
    const float rel = wholeFrame ? 0.1f : 0.25f;
    const float thresh = std::max(rel * nz[k99], wholeFrame ? 0.1f : 0.05f);
    static const int nbr[4][2] = {{0, 1}, {1, 1}, {1, 0}, {1, -1}};  // (dy, dx)
    auto magAt = [&](int x, int y) {
        return (x < 0 || y < 0 || x >= w || y >= h) ? 0.0f : mag[(size_t)y * w + x];
    };
    std::vector<int> xs, ys;
    std::vector<int> grid(N, -1);  // pixel -> point index
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const float m = mag[i];
            if (m <= thresh)
                continue;
            int a = (int)std::lround(std::atan2(gy[i], gx[i]) / (3.14159265f / 4));
            a = ((a % 4) + 4) % 4;
            const int dy = nbr[a][0], dx = nbr[a][1];
            if (m >= magAt(x + dx, y + dy) && m >= magAt(x - dx, y - dy)) {
                grid[i] = (int)xs.size();
                xs.push_back(x);
                ys.push_back(y);
            }
        }
    const int n = (int)xs.size();
    if (n < 2)
        return false;

    // walk the edge pixels: follow neighbours, jump (beam off) to the nearest
    // unvisited pixel when a line ends
    std::vector<uint8_t> visited(n, 0);
    std::vector<int> order;
    std::vector<uint8_t> jump;
    order.reserve(n);
    jump.reserve(n);
    static const int steps8[8][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    auto free_at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return -1;
        const int j = grid[(size_t)y * w + x];
        return (j >= 0 && !visited[j]) ? j : -1;
    };
    int cur = 0;
    bool jumped = false;
    for (int c = 0; c < n; ++c) {
        visited[cur] = 1;
        order.push_back(cur);
        jump.push_back(jumped ? 1 : 0);
        const int x = xs[cur], y = ys[cur];
        int nxt = -1;
        for (const auto& s : steps8)
            if ((nxt = free_at(x + s[1], y + s[0])) >= 0)
                break;
        jumped = false;
        if (nxt < 0) {
            // nearest unvisited: search square rings outward, then one ring past
            // the first hit so the pick is (nearly) the true nearest
            float best = 1e30f;
            int stopAt = std::max(w, h);
            for (int r = 2; r <= stopAt; ++r) {
                for (int dy = -r; dy <= r; ++dy) {
                    const int step = (dy == -r || dy == r) ? 1 : 2 * r;
                    for (int dx = -r; dx <= r; dx += step) {
                        const int j = free_at(x + dx, y + dy);
                        if (j >= 0) {
                            const float d = (float)(dx * dx + dy * dy);
                            if (d < best) { best = d; nxt = j; }
                        }
                    }
                }
                if (nxt >= 0 && stopAt == std::max(w, h))
                    stopAt = std::min(stopAt, (int)std::ceil(r * 1.415f));
            }
            if (nxt < 0)
                break;
            jumped = true;
        }
        cur = nxt;
    }

    // drop specks (strokes shorter than 6 pixels)
    std::vector<int> o2;
    std::vector<uint8_t> b2;
    for (size_t a = 0; a < order.size();) {
        size_t b = a + 1;
        while (b < order.size() && !jump[b]) ++b;
        if (b - a >= 6)
            for (size_t i = a; i < b; ++i) {
                o2.push_back(order[i]);
                b2.push_back(i == a ? 1 : 0);
            }
        a = b;
    }
    if (o2.size() >= 2) {
        order.swap(o2);
        jump.swap(b2);
    }
    jump[0] = 0;

    // thin out very busy pictures so the beam can redraw them every frame
    const size_t maxPoints = 2500;
    if (order.size() > maxPoints) {
        const size_t k = (order.size() + maxPoints - 1) / maxPoints;
        std::vector<int> o3;
        std::vector<uint8_t> b3;
        for (size_t i = 0; i < order.size(); i += k) {
            uint8_t any = 0;
            for (size_t j = (i == 0 ? 0 : i - k + 1); j <= i; ++j)
                any |= jump[j];
            o3.push_back(order[i]);
            b3.push_back(i == 0 ? 0 : any);
        }
        order.swap(o3);
        jump.swap(b3);
    }

    out.x.resize(order.size());
    out.y.resize(order.size());
    out.blank = jump;
    float cxo, cyo, r;
    if (wholeFrame) {
        cxo = (w - 1) / 2.0f;
        cyo = (h - 1) / 2.0f;
        r = std::hypot((float)w, (float)h) / 2 / 1.05f;  // frame corners just past the rim
    } else {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i : order) {
            x0 = std::min(x0, (float)xs[i]); x1 = std::max(x1, (float)xs[i]);
            y0 = std::min(y0, (float)ys[i]); y1 = std::max(y1, (float)ys[i]);
        }
        cxo = (x0 + x1) / 2;
        cyo = (y0 + y1) / 2;
        r = 0;
        for (int i : order)
            r = std::max(r, std::hypot(xs[i] - cxo, ys[i] - cyo));
        r = std::max(r, 1.0f);
    }
    for (size_t i = 0; i < order.size(); ++i) {
        out.x[i] = (xs[order[i]] - cxo) / r * 0.8f;
        out.y[i] = (cyo - ys[order[i]]) / r * 0.8f;  // leaves room for the music to push it about
    }
    return true;
}

// Load and trace a whole file. Returns an error message, or "" when it worked.
static std::string loadPicture(const std::string& path, Picture& pic, const std::atomic<bool>& cancel,
                               std::atomic<int>& percent)
{
    std::vector<uint8_t> data;
    if (!readFile(path, data))
        return "can't open the file";
    int w = 0, h = 0, frames = 1, comp = 0;
    int* delays = nullptr;
    uint8_t* px;
    const bool gif = data.size() > 3 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F';
    if (gif)
        px = stbi_load_gif_from_memory(data.data(), (int)data.size(), &delays, &w, &h, &frames, &comp, 4);
    else
        px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &comp, 4);
    if (!px)
        return "not a GIF, PNG, JPG or BMP";
    const bool anim = frames > 1;
    float t = 0;
    for (int f = 0; f < frames && !cancel.load(); ++f) {
        const float dur = std::max(delays ? delays[f] : 100, 20) / 1000.0f;
        Frame fr;
        if (traceFrame(px + (size_t)f * w * h * 4, w, h, anim, fr)) {
            pic.frames.push_back(std::move(fr));
            pic.starts.push_back(t);
        } else if (!pic.frames.empty()) {
            // blank frame: keep it as an empty one so the timing stays right
            pic.frames.emplace_back();
            pic.starts.push_back(t);
        }
        t += dur;
        percent.store((f + 1) * 100 / frames);
    }
    pic.length = t;
    stbi_image_free(px);
    if (delays)
        stbi_image_free(delays);
    if (pic.frames.empty())
        return "couldn't find any outlines in that picture";
    return "";
}

} // namespace crt
