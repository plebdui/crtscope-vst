#include "CrtScopeShared.hpp"
#include <cmath>
#include <cstring>

START_NAMESPACE_DISTRHO

// flaws default off: dropping the scope on a track shouldn't change the sound
static const float kDefaults[kParamCount] = {0, 0, 1, 1, 1, 1, 1, 0, 20, 1, 0, 0};

CrtScopePlugin::CrtScopePlugin()
    : Plugin(kParamCount, 0, 1)
{
    std::memcpy(fParams, kDefaults, sizeof(fParams));
    float x, y;
    crt::equation(0, 0, 0, x, y);  // works out the curves' sizes now, not on the audio thread
}

CrtScopePlugin::~CrtScopePlugin()
{
    stopLoading();
}

void CrtScopePlugin::stopLoading()
{
    fLoadCancel.store(true);
    if (fLoadThread.joinable())
        fLoadThread.join();
    fLoadCancel.store(false);
}

// A picture was dropped (or a project reopened): trace it for the sound
void CrtScopePlugin::setState(const char* key, const char* value)
{
    if (std::strcmp(key, "image") != 0 || fPicPath == value)
        return;
    fPicPath = value;
    stopLoading();
    std::lock_guard<std::mutex> lock(fPicLock);
    if (!*value) {
        fPic.store(nullptr, std::memory_order_release);
        fPicOld = std::move(fPicCur);
        return;
    }
    const std::string path = value;
    fLoadThread = std::thread([this, path] {
        crt::Picture pic;
        std::atomic<int> percent{0};
        if (!crt::loadPicture(path, pic, fLoadCancel, percent).empty() || fLoadCancel.load())
            return;
        std::unique_ptr<crt::PictureMod> mod(new crt::PictureMod(pic));
        std::lock_guard<std::mutex> lock(fPicLock);
        fPic.store(mod.get(), std::memory_order_release);
        fPicOld = std::move(fPicCur);
        fPicCur = std::move(mod);
    });
}

void CrtScopePlugin::initAudioPort(bool input, uint32_t index, AudioPort& port)
{
    port.groupId = kPortGroupStereo;
    Plugin::initAudioPort(input, index, port);
}

void CrtScopePlugin::initParameter(uint32_t index, Parameter& p)
{
    p.hints = kParameterIsAutomatable;
    p.ranges.def = kDefaults[index];
    switch (index) {
    case kMode: {
        p.hints |= kParameterIsInteger;
        p.name = "Mode"; p.symbol = "mode";
        p.ranges.min = 0; p.ranges.max = 4;
        static ParameterEnumerationValue v[5] = {{0, "XY"}, {1, "Goniometer"}, {2, "Waveform"}, {3, "Image"}, {4, "Equation"}};
        p.enumValues.count = 5; p.enumValues.restrictedMode = true; p.enumValues.values = v;
        p.enumValues.deleteLater = false;
        break;
    }
    case kPhosphor:
        p.hints |= kParameterIsInteger;
        p.name = "Phosphor"; p.symbol = "phosphor";
        p.ranges.min = 0; p.ranges.max = 18;
        break;
    case kAfterglow:
        p.hints |= kParameterIsLogarithmic;
        p.name = "Afterglow"; p.symbol = "afterglow"; p.unit = "x";
        p.ranges.min = 0.05f; p.ranges.max = 20;
        break;
    case kIntensity:
        p.hints |= kParameterIsLogarithmic;
        p.name = "Beam"; p.symbol = "intensity"; p.unit = "x";
        p.ranges.min = 0.05f; p.ranges.max = 20;
        break;
    case kGain:
        p.hints |= kParameterIsLogarithmic;
        p.name = "Gain"; p.symbol = "gain"; p.unit = "x";
        p.ranges.min = 0.05f; p.ranges.max = 20;
        break;
    case kAutoGain:
        p.hints |= kParameterIsBoolean | kParameterIsInteger;
        p.name = "Auto gain"; p.symbol = "autogain";
        p.ranges.min = 0; p.ranges.max = 1;
        break;
    case kGraticule:
        p.hints |= kParameterIsBoolean | kParameterIsInteger;
        p.name = "Graticule"; p.symbol = "graticule";
        p.ranges.min = 0; p.ranges.max = 1;
        break;
    case kFlaws: {
        p.hints |= kParameterIsInteger;
        p.name = "Tube flaws"; p.symbol = "flaws";
        p.ranges.min = 0; p.ranges.max = 2;
        static ParameterEnumerationValue v[3] = {{0, "Off"}, {1, "Subtle"}, {2, "Worn"}};
        p.enumValues.count = 3; p.enumValues.restrictedMode = true; p.enumValues.values = v;
        p.enumValues.deleteLater = false;
        break;
    }
    case kSweep:
        p.hints |= kParameterIsLogarithmic;
        p.name = "Sweep"; p.symbol = "sweep"; p.unit = "ms";
        p.ranges.min = 1; p.ranges.max = 200;
        break;
    case kEquation: {
        p.hints |= kParameterIsInteger;
        p.name = "Equation"; p.symbol = "equation";
        p.ranges.min = 0; p.ranges.max = crt::kNumEquations - 1;
        static ParameterEnumerationValue v[crt::kNumEquations];
        for (int i = 0; i < crt::kNumEquations; ++i) {
            v[i].value = (float)i;
            v[i].label = crt::kEquationInfo[i].name;
        }
        p.enumValues.count = crt::kNumEquations; p.enumValues.restrictedMode = true; p.enumValues.values = v;
        p.enumValues.deleteLater = false;
        break;
    }
    case kWet:
        p.name = "Wet"; p.symbol = "wet"; p.unit = "%";
        p.ranges.min = 0; p.ranges.max = 1;
        break;
    case kShape:
        p.name = "Shape"; p.symbol = "shape"; p.unit = "%";
        p.ranges.min = 0; p.ranges.max = 1;
        break;
    }
}

float CrtScopePlugin::getParameterValue(uint32_t index) const
{
    return index < kParamCount ? fParams[index] : 0.0f;
}

void CrtScopePlugin::setParameterValue(uint32_t index, float value)
{
    if (index < kParamCount)
        fParams[index] = value;
}

// the picture shown in image mode, saved with the project
void CrtScopePlugin::initState(uint32_t, State& state)
{
    state.key = "image";
    state.label = "Image file";
    state.defaultValue = "";
}

void CrtScopePlugin::run(const float** inputs, float** outputs, uint32_t frames)
{
    ring.write(inputs[0], inputs[1], frames);  // the scope shows the clean input
    // tube flaws (hum, drift, flicker) on the sound too; OFF = untouched
    fAmp.process(inputs[0], inputs[1], outputs[0], outputs[1], frames,
                 (int)std::lround(fParams[kFlaws]), fParams[kWet], (float)getSampleRate());
    // in IMAGE and EQUATION modes the shape on the tube shapes the sound as well,
    // and SHAPE blends in the shape itself played as sound (oscilloscope music)
    const int mode = (int)std::lround(fParams[kMode]);
    const float sr = (float)getSampleRate();
    if (mode == kModeEquation) {
        const int eq = (int)std::lround(fParams[kEquation]);
        float len = 0;
        for (int k = 0; k < crt::PictureMod::kN; ++k) {
            crt::equation(eq, k / (double)crt::PictureMod::kN, fEqTime, fEqX[k], fEqY[k]);
            if (k) len += std::hypot(fEqX[k] - fEqX[k - 1], fEqY[k] - fEqY[k - 1]);
        }
        fEqTime = std::fmod(fEqTime + frames / (double)sr, 3600.0);
        fPicFx.process(outputs[0], outputs[1], frames, fEqX, fEqY, std::min(1.0f, len / 16.0f), 0.25f,
                       fParams[kWet], sr);
        fSynth.process(outputs[0], outputs[1], frames, fEqX, fEqY, fParams[kShape], sr);
    } else if (mode == kModeImage) {
        const crt::PictureMod* pic = fPic.load(std::memory_order_acquire);
        fPicFx.process(outputs[0], outputs[1], frames, pic, fParams[kWet], sr);
        fSynth.process(outputs[0], outputs[1], frames, pic, fParams[kShape], sr);
    } else {
        fPicFx.process(outputs[0], outputs[1], frames, nullptr, fParams[kWet], sr);
    }
}

Plugin* createPlugin()
{
    return new CrtScopePlugin();
}

END_NAMESPACE_DISTRHO
