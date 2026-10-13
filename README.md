# CRT Scope (VST3 / CLAP)

The [crtscope](https://github.com/plebdui/crtscope) tube as a plugin: put it on a
mixer track and its window draws that track's sound as a 1960s CRT oscilloscope.
The audio passes through untouched, unless you turn on FLAWS or load a picture.

<p align="center">
  <img src="screenshots/modes.gif" width="100%" alt="XY, goniometer and waveform modes moving on the tube">
</p>

<p align="center">
  <img src="screenshots/window-xy.png" width="45%" alt="The plugin window in XY mode, with its row of controls">
  <img src="screenshots/window-gonio.png" width="45%" alt="The plugin window in goniometer mode">
</p>

- **Modes:** XY (Lissajous), goniometer, a triggered waveform, and IMAGE
- **Pictures:** drag a GIF, PNG, JPG or BMP onto the window (or click the tube
  in IMAGE mode to pick one) and the beam traces its outlines; GIFs animate and
  the music shakes the picture. The file is saved with your FL project.
- **The picture shapes the sound** in IMAGE mode: the beam's path around the
  outline becomes a tremolo (left/right moves the left channel, up/down the
  right), and busy frames sound bright, sparse ones dark. GIFs change it every frame.
- **Phosphors:** P7 radar, P1 green, P12 orange, P31, plus 15 Apple II colours
- **FLAWS** (off / subtle / worn): an old tube's 60Hz mains hum, drift and
  flicker, on the picture and on the sound (hum ~41 dB / ~29 dB under the music)
- **WET** (0-100%): how much of the flaws and the picture reach the sound
- **Controls** along the bottom: left-click or scroll up = next / more,
  right-click or scroll down = back / less. All of them are automatable.

<p align="center">
  <img src="screenshots/window-image.png" width="45%" alt="A dropped GIF traced on the tube in IMAGE mode">
</p>

Video files and the clock from the terminal version aren't in the plugin.

![P7 radar, P1 green, P12 orange, P31, and Apple II blue, purple, red and white](screenshots/phosphors.png)

## Install in FL Studio

Download `CRT-Scope-vst3-windows.zip` from
[Releases](https://github.com/plebdui/crtscope-vst/releases/latest) and unzip it.
Copy the `CRT-Scope.vst3` folder into `C:\Program Files\Common Files\VST3` (on Linux
with Wine: `~/.wine/drive_c/Program Files/Common Files/VST3`), then in FL:
**Options → Manage plugins → Find more plugins**, and add **CRT Scope** to a
mixer track.

## Build (Windows plugin, from Linux)

Needs the mingw-w64 cross compiler (`x86_64-w64-mingw32-g++`).

```sh
git clone https://github.com/plebdui/crtscope-vst.git
cd crtscope-vst
git clone --recursive https://github.com/DISTRHO/DPF.git dpf
git -C dpf apply ../patches/dpf-wine-pixel-format.patch   # see below
make -C dpf/dgl WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ opengl
make WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
# -> bin/CRT-Scope.vst3 and bin/CRT-Scope.clap
```

The patch fixes the plugin window failing to open under Wine: Pugl asked for a
pixel format with wglChoosePixelFormatARB, Wine answered "success, 0 formats",
and Pugl used format 0. It now falls back to the basic format that works.

## Tests

- `tests/render_test.cpp`: draws the tube with test tones to PPM images (native, no host)
- `tests/minihost.cpp`: a tiny VST3 host (mingw, run under Wine) that opens the
  window, streams audio, clicks MODE and checks the result. Given a picture
  path, it also drags it onto the window through OLE (like Explorer), checks
  IMAGE mode turned on and the path is in the saved state; `MINIHOST_SAVE` /
  `MINIHOST_LOAD` save and reopen that state. Set `CRTSCOPE_DUMP`
  to a file path and the window saves snapshots of itself.
- pluginval 1.0.4, strictness 10, under Wine: all tests pass except its editor
  test, which times out under Wine for DPF's own example plugins too (the
  stuck threads are all inside pluginval).
