#!/usr/bin/make -f
# CRT Scope plugin. Windows VST3 (for FL Studio), built from Linux:
#   make WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
# macOS AU + VST3 + CLAP (GarageBand / Logic): just `make` on a Mac
# (.github/workflows/macos.yml builds a universal one)
# DPF goes inside this folder (see README); ../dpf also works
DPF_PATH ?= $(if $(wildcard dpf/Makefile.base.mk),dpf,../dpf)
DPF_TARGET_DIR = bin
DPF_BUILD_DIR = build
NAME = CRT-Scope
FILES_DSP = CrtScopePlugin.cpp
FILES_UI  = CrtScopeUI.cpp
ifeq ($(shell uname -s),Darwin)
FILES_UI += MacDrop.mm   # drag and drop from Finder
endif
# Mac: `make MIDI_AU=true` builds the second Audio Unit, "CRT Scope MIDI" (a
# MIDI-controlled effect for Logic). It needs its own window library build
# (make -C dpf/dgl opengl BUILD_DIR_SUFFIX=-midi DGL_NAMESPACE=CrtScopeMidiDGL)
# so its window classes don't clash with the plain one's.
ifeq ($(MIDI_AU),true)
NAME = CRT-Scope-MIDI
BUILD_DIR_SUFFIX = -midi
DGL_NAMESPACE = CrtScopeMidiDGL
endif

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_CXX_FLAGS += -std=gnu++17
ifeq ($(WINDOWS),true)
LINK_FLAGS += -lole32 -lshell32 -luuid   # drag and drop
endif

ifeq ($(MIDI_AU),true)
BUILD_C_FLAGS += -DCRT_MIDI_AU
BUILD_CXX_FLAGS += -DCRT_MIDI_AU
TARGETS += au
else
TARGETS += vst3 clap
ifeq ($(MACOS),true)
TARGETS += au   # GarageBand / Logic
endif
endif
ifeq ($(MACOS),true)
LINK_FLAGS += -framework Cocoa
endif
all: $(TARGETS)
