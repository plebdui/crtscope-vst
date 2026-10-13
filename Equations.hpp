// EQUATION mode: built-in parametric curves for the beam. Static ones are
// fixed shapes; animated ones change with time u (seconds). Every curve is
// scaled to fit -0.8..0.8 like a traced picture. Plain C++, shared by the
// window (drawing) and the plugin (shaping the sound).
#pragma once

#include <algorithm>
#include <cmath>

namespace crt {

static constexpr int kNumEquations = 11;

struct EquationInfo {
    const char* name;
    bool animated;
    float turns;  // t runs 0 .. turns * 2pi to close the curve
};

static const EquationInfo kEquationInfo[kNumEquations] = {
    {"ROSE", false, 1},           {"HEART", false, 1},          {"BUTTERFLY", false, 6},
    {"SPIROGRAPH", false, 3},     {"LISSAJOUS KNOT", false, 1}, {"INFINITY", false, 1},
    {"SPINNING LISSAJOUS", true, 1}, {"MORPHING ROSE", true, 1}, {"BREATHING SPIROGRAPH", true, 3},
    {"BEATING HEART", true, 1},   {"ROTATING CUBE", true, 1},
};

// t01 in [0,1): position along the curve. u: animation time in seconds.
// Unscaled; equation() below fits it to the tube.
static void equationRaw(int id, double t01, double u, double& x, double& y)
{
    const double pi = 3.14159265358979;
    const double t = t01 * 2 * pi * kEquationInfo[id].turns;
    switch (id) {
    case 0: {  // rose r = cos(4t): eight petals
        const double r = std::cos(4 * t);
        x = r * std::cos(t); y = r * std::sin(t);
        break;
    }
    case 1:  // heart
        x = 16 * std::pow(std::sin(t), 3);
        y = 13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t) + 2.5;
        break;
    case 2: {  // Fay's butterfly
        const double r = std::exp(std::sin(t)) - 2 * std::cos(4 * t) + std::pow(std::sin((2 * t - pi) / 24), 5);
        x = r * std::sin(t); y = r * std::cos(t) - 0.6;
        break;
    }
    case 3: {  // spirograph (hypotrochoid R=5, r=3, d=5)
        x = 2 * std::cos(t) + 5 * std::cos(2.0 / 3 * t);
        y = 2 * std::sin(t) - 5 * std::sin(2.0 / 3 * t);
        break;
    }
    case 4:  // Lissajous 3:2
        x = std::sin(3 * t + pi / 2); y = std::sin(2 * t);
        break;
    case 5: {  // lemniscate of Bernoulli
        const double d = 1 + std::sin(t) * std::sin(t);
        x = std::cos(t) / d; y = std::sin(t) * std::cos(t) / d;
        break;
    }
    case 6:  // Lissajous 3:2 with drifting phase: looks like it spins in 3D
        x = std::sin(3 * t + u * 0.9); y = std::sin(2 * t);
        break;
    case 7: {  // rose that morphs 3 -> 4 -> 5 -> 2 petals
        const double k = std::fmod(u * 0.25, 4.0);
        const int a = (int)k, b = (a + 1) % 4;
        const double m = 0.5 - 0.5 * std::cos(pi * (k - a));
        static const int petals[4] = {3, 4, 5, 2};
        const double r = (1 - m) * std::cos(petals[a] * t) + m * std::cos(petals[b] * t);
        x = r * std::cos(t); y = r * std::sin(t);
        break;
    }
    case 8: {  // spirograph whose pen arm grows and shrinks
        const double d = 3 + 2.5 * std::sin(u * 0.7);
        x = 2 * std::cos(t) + d * std::cos(2.0 / 3 * t);
        y = 2 * std::sin(t) - d * std::sin(2.0 / 3 * t);
        break;
    }
    case 9: {  // heart with a lub-dub beat, about 70 bpm
        const double ph = std::fmod(u * 1.17, 1.0);
        const double beat = std::exp(-std::pow((ph - 0.1) / 0.05, 2)) + 0.6 * std::exp(-std::pow((ph - 0.3) / 0.05, 2));
        const double hx = 16 * std::pow(std::sin(t), 3);
        const double hy = 13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t) + 2.5;
        const double s = 0.82 + 0.18 * beat;
        x = hx * s; y = hy * s;
        break;
    }
    case 10: {  // wireframe cube spinning on two axes, perspective
        // one beam path over all 12 edges (three are drawn twice)
        static const int path[16] = {0, 1, 2, 3, 0, 4, 5, 6, 7, 4, 5, 1, 2, 6, 7, 3};
        static const double v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                       {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        const double p = t01 * 15;
        const int i = std::min((int)p, 14);
        const double f = p - i;
        double q[3];
        for (int k = 0; k < 3; ++k)
            q[k] = v[path[i]][k] + (v[path[i + 1]][k] - v[path[i]][k]) * f;
        const double a = u * 0.8, b = u * 0.53;
        const double x1 = q[0] * std::cos(a) - q[2] * std::sin(a), z1 = q[0] * std::sin(a) + q[2] * std::cos(a);
        const double y1 = q[1] * std::cos(b) - z1 * std::sin(b), z2 = q[1] * std::sin(b) + z1 * std::cos(b);
        const double persp = 4 / (4 + z2);
        x = x1 * persp; y = y1 * persp;
        break;
    }
    default:
        x = y = 0;
        break;
    }
}

// The curve fitted to the tube (-0.8..0.8), same scale for every frame of an animation
static void equation(int id, double t01, double u, float& x, float& y)
{
    static const auto scales = [] {
        struct S { double v[kNumEquations]; } s;
        for (int id = 0; id < kNumEquations; ++id) {
            double m = 1e-9;
            const int frames = kEquationInfo[id].animated ? 48 : 1;
            for (int f = 0; f < frames; ++f)
                for (int i = 0; i < 2000; ++i) {
                    double px, py;
                    equationRaw(id, i / 2000.0, f * 0.37, px, py);
                    m = std::max({m, std::fabs(px), std::fabs(py)});
                }
            s.v[id] = 0.8 / m;
        }
        return s;
    }();
    id = std::clamp(id, 0, kNumEquations - 1);
    double px, py;
    equationRaw(id, t01, u, px, py);
    x = (float)(px * scales.v[id]);
    y = (float)(py * scales.v[id]);
}

} // namespace crt
