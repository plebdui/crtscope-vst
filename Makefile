#!/usr/bin/make -f
# CRT Scope plugin. Windows VST3 (for FL Studio), built from Linux:
#   make WINDOWS=true CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
DPF_PATH = ../dpf
DPF_TARGET_DIR = bin
DPF_BUILD_DIR = build
NAME = CRT-Scope
FILES_DSP = CrtScopePlugin.cpp
FILES_UI  = CrtScopeUI.cpp

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_CXX_FLAGS += -std=gnu++17

TARGETS += vst3 clap
all: $(TARGETS)
