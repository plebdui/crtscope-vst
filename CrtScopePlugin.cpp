#include "CrtScopeShared.hpp"
#include <cstring>

START_NAMESPACE_DISTRHO

static const float kDefaults[kParamCount] = {0, 0, 1, 1, 1, 1, 1, 1, 20};

CrtScopePlugin::CrtScopePlugin()
    : Plugin(kParamCount, 0, 1)
{
    std::memcpy(fParams, kDefaults, sizeof(fParams));
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
        p.ranges.min = 0; p.ranges.max = 3;
        static ParameterEnumerationValue v[4] = {{0, "XY"}, {1, "Goniometer"}, {2, "Waveform"}, {3, "Image"}};
        p.enumValues.count = 4; p.enumValues.restrictedMode = true; p.enumValues.values = v;
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
    ring.write(inputs[0], inputs[1], frames);  // before the copy, in case the host reuses buffers
    // a scope listens, it doesn't change the sound
    if (outputs[0] != inputs[0])
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
    if (outputs[1] != inputs[1])
        std::memcpy(outputs[1], inputs[1], sizeof(float) * frames);
}

Plugin* createPlugin()
{
    return new CrtScopePlugin();
}

END_NAMESPACE_DISTRHO
