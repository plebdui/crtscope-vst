// Renders the tube with test tones and writes PPM images, no plugin host needed.
//   g++ -O2 -std=c++17 render_test.cpp -o render_test && ./render_test out_dir
#include "../CrtTube.hpp"
#include <cstdio>
#include <string>

static void save(const crt::Tube& t, const uint8_t* rgba, const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6 %d %d 255\n", t.width(), t.height());
    for (int i = 0; i < t.width() * t.height(); ++i)
        std::fwrite(rgba + i * 4, 1, 3, f);
    std::fclose(f);
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const float sr = 48000;
    struct Case { const char* name; int mode, phosphor, flaws; float a2; };
    const Case cases[] = {{"xy", crt::kXY, 0, 1, 0}, {"gonio", crt::kGonio, 1, 1, 0.3f},
                          {"wave", crt::kWave, 3, 1, 0}, {"worn_p12", crt::kXY, 2, 2, 0},
                          {"apple_purple", crt::kXY, 9, 1, 0}, {"silence", crt::kXY, 0, 1, -1}};
    for (const Case& c : cases) {
        crt::Tube tube;
        tube.resize(640, 640);
        tube.mode = c.mode;
        tube.phosphor = c.phosphor;
        tube.flaws = c.flaws;
        double t = 0;
        const int per = (int)(sr / 60);
        std::vector<float> L(per), R(per);
        for (int frame = 0; frame < 120; ++frame) {
            for (int i = 0; i < per; ++i, t += 1 / sr) {
                const bool quiet = c.a2 < 0;
                L[i] = quiet ? 0 : 0.6f * std::sin(2 * M_PI * 220 * t);
                R[i] = quiet ? 0 : 0.6f * std::sin(2 * M_PI * 330.4 * t + 0.5) + c.a2 * L[i];
            }
            tube.feed(L.data(), R.data(), per, sr);
            tube.step(1.0f / 60);
        }
        save(tube, tube.render(), dir + "/" + c.name + ".ppm");
        std::printf("%s ok\n", c.name);
    }
}
