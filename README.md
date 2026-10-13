# CRT Scope (VST3 / CLAP)

The [crtscope](https://github.com/plebdui/crtscope) tube as a plugin: put it on a
mixer track and its window draws that track's sound as a 1960s CRT oscilloscope.
The audio passes through untouched.

- **Modes:** XY (Lissajous), goniometer, and a triggered waveform
- **Phosphors:** P7 radar, P1 green, P12 orange, P31, plus 15 Apple II colours
- **Controls** along the bottom: left-click or scroll up = next / more,
  right-click or scroll down = back / less. All of them are automatable.

Image/video tracing and the clock from the terminal version aren't in the plugin.

## Build (Windows plugin, from Linux)

```sh
git clone --recursive https://github.com/DISTRHO/DPF.git dpf
git -C dpf apply ../plugin/patches/dpf-wine-pixel-format.patch   # see below
cd plugin
make -C ../dpf/dgl WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ opengl
make WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
# -> bin/CRT-Scope.vst3 and bin/CRT-Scope.clap
```

The patch fixes the plugin window failing to open under Wine: Pugl asked for a
pixel format with wglChoosePixelFormatARB, Wine answered "success, 0 formats",
and Pugl used format 0. It now falls back to the basic format that works.

## Tests

- `tests/render_test.cpp`: draws the tube with test tones to PPM images (native, no host)
- `tests/minihost.cpp`: a tiny VST3 host (mingw, run under Wine) that opens the
  window, streams audio, clicks MODE and checks the result. Set `CRTSCOPE_DUMP`
  to a file path and the window saves snapshots of itself.
- pluginval 1.0.4, strictness 10, under Wine: all tests pass except its editor
  test, which times out under Wine for DPF's own example plugins too (the
  stuck threads are all inside pluginval).
