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

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_CXX_FLAGS += -std=gnu++17
ifeq ($(WINDOWS),true)
LINK_FLAGS += -lole32 -lshell32 -luuid   # drag and drop
endif

TARGETS += vst3 clap
ifeq ($(MACOS),true)
TARGETS += au   # GarageBand / Logic
LINK_FLAGS += -framework Cocoa
endif
all: $(TARGETS)
