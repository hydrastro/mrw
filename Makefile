# morse-deluxe - portable Makefile (Linux / macOS / Windows-MinGW)
#
# Quick start:
#   make            build the core library, CLI, and tests
#   make check      build and run the unit tests
#   make gui        build the Dear ImGui desktop application
#   make everything build all of the above
#   make deps       fetch Dear ImGui / miniaudio into third_party/
#   make install    install libs, headers and binaries under PREFIX
#   make clean      remove build artifacts
#   make help       list targets and options
#
# Common options (pass on the command line, e.g. `make gui DSA_ROOT=/path/dsa`):
#   DSA_ROOT=...       built DSA checkout or install prefix (default pkg-config)
#   IMGUI_DIR=...      Dear ImGui source tree                (default third_party/imgui)
#   MINIAUDIO_DIR=...  directory containing miniaudio.h      (default third_party/miniaudio)
#   PREFIX=...         install prefix                        (default /usr/local)
#   GLFW_CFLAGS/GLFW_LIBS    override GLFW pkg-config flags
#
# The core library and CLI depend only on DSA + libm. The
# GUI additionally needs OpenGL, GLFW and (on Linux) the usual X11 dev packages.

# ---- toolchain -----------------------------------------------------------
CC      ?= cc
CXX     ?= c++
AR      ?= ar
PKGCONF ?= pkg-config
GIT     ?= git

# ---- platform detection --------------------------------------------------
UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
ifeq ($(OS),Windows_NT)
  PLATFORM := windows
else ifneq (,$(findstring MINGW,$(UNAME_S)))
  PLATFORM := windows
else ifneq (,$(findstring MSYS,$(UNAME_S)))
  PLATFORM := windows
else ifeq ($(UNAME_S),Darwin)
  PLATFORM := macos
else
  PLATFORM := linux
endif

ifeq ($(PLATFORM),windows)
  EXE := .exe
else
  EXE :=
endif

# ---- directories & dependencies -----------------------------------------
BUILD  ?= build
OBJ    := $(BUILD)/obj
THIRD  := third_party

# DS_ROOT remains a legacy alias.
DSA_ROOT      ?= $(DS_ROOT)
IMGUI_DIR     ?= $(THIRD)/imgui
MINIAUDIO_DIR ?= $(THIRD)/miniaudio
PREFIX        ?= /usr/local

# Pinned versions for `make deps`
IMGUI_REF     ?= v1.91.5
MINIAUDIO_REF ?= master

# ---- external DSA --------------------------------------------------------
# Build DSA separately with its own Makefile (including generated sources).
# A checkout uses build/libdsa.a; an installed prefix uses lib/libdsa.a.
ifneq ($(strip $(DSA_ROOT)),)
  DSA_CFLAGS ?= -I$(DSA_ROOT)/include
  DSA_LIBS ?= $(firstword $(wildcard $(DSA_ROOT)/build/libdsa.a $(DSA_ROOT)/lib/libdsa.a $(DSA_ROOT)/lib64/libdsa.a))
  ifeq ($(strip $(DSA_LIBS)),)
    DSA_LIBS = -L$(DSA_ROOT)/lib -L$(DSA_ROOT)/lib64 -ldsa
  endif
else
  DSA_CFLAGS ?= $(shell $(PKGCONF) --cflags dsa 2>/dev/null)
  DSA_LIBS ?= $(shell $(PKGCONF) --libs dsa 2>/dev/null || echo -ldsa)
endif
DSA_DEPS := $(filter %.a %.so %.dylib,$(DSA_LIBS))

# ---- core library --------------------------------------------------------
MORSE_SRC := $(wildcard src/*.c)
MORSE_OBJ := $(patsubst src/%.c,$(OBJ)/morse_%.o,$(MORSE_SRC))

LIBMORSE := $(BUILD)/libmorse.a
CLI      := $(BUILD)/morsec$(EXE)
GUI      := $(BUILD)/morse-deluxe-gui$(EXE)

# ---- flags ---------------------------------------------------------------
OPT       ?= -O2
CFLAGS    ?= $(OPT)
CXXFLAGS  ?= $(OPT)
MORSE_CPPFLAGS := -Iinclude $(DSA_CFLAGS)
MORSE_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -MMD -MP
LDLIBS_CORE := $(DSA_LIBS) -lm $(LDLIBS)

# ---- unit tests ----------------------------------------------------------
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%$(EXE),$(TEST_SRC))

# ==========================================================================
# default + phony targets
# ==========================================================================
.PHONY: all everything lib cli tests check gui deps install uninstall \
        clean distclean help format print-config

all: lib cli tests
	@echo "Built core library, CLI and tests for $(PLATFORM)."

everything: all gui

lib: $(LIBMORSE)
cli: $(CLI)
tests: $(TEST_BIN)

# ==========================================================================
# core library
# ==========================================================================
$(OBJ):
	@mkdir -p $(OBJ)

$(OBJ)/morse_%.o: src/%.c | $(OBJ)
	$(CC) $(CPPFLAGS) $(MORSE_CPPFLAGS) $(CFLAGS) $(MORSE_CFLAGS) -Isrc -c $< -o $@

$(LIBMORSE): $(MORSE_OBJ)
	@mkdir -p $(BUILD)
	rm -f $@
	$(AR) rcs $@ $^
	@echo "  AR    $@"

# ==========================================================================
# command-line tool
# ==========================================================================
$(CLI): app/cli/main.c $(LIBMORSE) $(DSA_DEPS)
	$(CC) $(CPPFLAGS) $(MORSE_CPPFLAGS) $(CFLAGS) $(MORSE_CFLAGS) $(LDFLAGS) -Isrc $< $(LIBMORSE) $(LDLIBS_CORE) -o $@
	@echo "  LINK  $@"

# ==========================================================================
# unit tests
# ==========================================================================
$(BUILD)/%$(EXE): tests/%.c $(LIBMORSE) $(DSA_DEPS)
	$(CC) $(CPPFLAGS) $(MORSE_CPPFLAGS) $(CFLAGS) $(MORSE_CFLAGS) $(LDFLAGS) -Itests -Isrc $< $(LIBMORSE) $(LDLIBS_CORE) -o $@

check: tests
	@set -e; for t in $(TEST_BIN); do "$$t"; done

# ==========================================================================
# GUI (Dear ImGui + GLFW + OpenGL + miniaudio)
# ==========================================================================
GUI_CXX_SRC := app/gui/main.cpp app/gui/app.cpp app/gui/audio.cpp \
               app/gui/media.cpp app/gui/cwio_cpp.cpp
GUI_C_SRC   := app/gui/miniaudio_impl.c
IMGUI_SRC   := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp \
               $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp \
               $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
               $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

# Explicit, unique, flat object names. The previous wildcard form let the core
# pattern also match the backends (because % spans '/'), which both nested the
# objects in an uncreated directory and compiled the backends twice.
IMGUI_CORE_OBJ := $(addprefix $(OBJ)/imgui_,imgui.o imgui_draw.o imgui_tables.o imgui_widgets.o)
IMGUI_BE_OBJ   := $(addprefix $(OBJ)/imgui_,imgui_impl_glfw.o imgui_impl_opengl3.o)

GUI_OBJ := $(patsubst app/gui/%.cpp,$(OBJ)/gui_%.o,$(GUI_CXX_SRC)) \
           $(patsubst app/gui/%.c,$(OBJ)/gui_%.o,$(GUI_C_SRC)) \
           $(IMGUI_CORE_OBJ) $(IMGUI_BE_OBJ)

# GLFW discovery
GUI_GLFW ?= system
ifeq ($(GUI_GLFW),system)
  GLFW_CFLAGS ?= $(shell $(PKGCONF) --cflags glfw3 2>/dev/null)
  GLFW_LIBS   ?= $(shell $(PKGCONF) --libs glfw3 2>/dev/null)
endif
ifeq ($(strip $(GLFW_LIBS)),)
  # pkg-config found nothing: fall back to a plain link flag per platform
  ifeq ($(PLATFORM),windows)
    GLFW_LIBS := -lglfw3
  else
    GLFW_LIBS := -lglfw
  endif
endif

CXXFLAGS_GUI := -std=c++17 $(CXXFLAGS) -Iapp/gui $(MORSE_CPPFLAGS) \
                -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(MINIAUDIO_DIR) \
                $(GLFW_CFLAGS) -MMD -MP

# AUDIO_DIRECT=1 links the Linux audio backends (ALSA + PulseAudio) directly
# instead of letting miniaudio dlopen() them at runtime. This bakes the library
# paths into the binary (rpath), so a binary built with plain `make` finds its
# audio backend without LD_LIBRARY_PATH - which is what you want on NixOS. It
# needs the alsa-lib and libpulseaudio *headers* at build time (present in the
# Nix dev shell). Off by default so the portable build stays dependency-light.
MA_DEFINES :=
MA_AUDIO_LIBS :=
ifeq ($(PLATFORM),linux)
  ifneq ($(AUDIO_DIRECT),0)
  ifneq ($(AUDIO_DIRECT),)
    MA_DEFINES := -DMA_NO_RUNTIME_LINKING -DMA_NO_JACK -DMA_NO_SNDIO \
                  -DMA_NO_OSS -DMA_NO_AAUDIO -DMA_NO_OPENSL -DMA_NO_AUDIO4
    MA_AUDIO_LIBS := -lasound -lpulse
  endif
  endif
endif

CFLAGS_GUI_C := $(CFLAGS) -Iapp/gui -I$(MINIAUDIO_DIR) $(MA_DEFINES) -MMD -MP

ifeq ($(PLATFORM),linux)
  GUI_PLATFORM_LIBS := -lGL -lpthread -ldl -lm $(MA_AUDIO_LIBS)
endif
ifeq ($(PLATFORM),macos)
  GUI_PLATFORM_LIBS := -framework Cocoa -framework OpenGL -framework IOKit \
                       -framework CoreVideo -framework CoreFoundation \
                       -framework CoreAudio -framework AudioToolbox \
                       -framework AudioUnit
endif
ifeq ($(PLATFORM),windows)
  # GLFW (Win32) + OpenGL + miniaudio (WASAPI/WinMM) link requirements
  GUI_PLATFORM_LIBS := -lopengl32 -lgdi32 -limm32 -lole32 -loleaut32 \
                       -lwinmm -lksuser -luuid
endif

gui: $(GUI)

$(GUI): $(GUI_OBJ) $(LIBMORSE) $(DSA_DEPS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(GUI_OBJ) $(LIBMORSE) $(LDLIBS_CORE) $(GLFW_LIBS) $(GUI_PLATFORM_LIBS) -o $@
	@echo "  LINK  $@"

$(OBJ)/gui_%.o: app/gui/%.cpp | $(OBJ) gui-check
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_GUI) -c $< -o $@

$(OBJ)/gui_%.o: app/gui/%.c | $(OBJ) gui-check
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS_GUI_C) -c $< -o $@

# Dear ImGui core and backend objects, mapped explicitly to their two source
# directories so object names stay flat and unique.
$(IMGUI_CORE_OBJ): $(OBJ)/imgui_%.o: $(IMGUI_DIR)/%.cpp | $(OBJ) gui-check
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_GUI) -c $< -o $@

$(IMGUI_BE_OBJ): $(OBJ)/imgui_%.o: $(IMGUI_DIR)/backends/%.cpp | $(OBJ) gui-check
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_GUI) -c $< -o $@

# ==========================================================================
# dependency checks & fetching
# ==========================================================================
.PHONY: gui-check
gui-check:
	@test -f "$(IMGUI_DIR)/imgui.cpp" || { \
	  echo "ERROR: Dear ImGui not found at IMGUI_DIR=$(IMGUI_DIR). Run 'make deps'."; exit 1; }
	@test -f "$(MINIAUDIO_DIR)/miniaudio.h" || { \
	  echo "ERROR: miniaudio.h not found at MINIAUDIO_DIR=$(MINIAUDIO_DIR). Run 'make deps'."; exit 1; }

deps:
	@mkdir -p $(THIRD)
	@test -d "$(IMGUI_DIR)/.git" || $(GIT) clone --depth 1 -b $(IMGUI_REF) https://github.com/ocornut/imgui.git $(IMGUI_DIR)
	@test -d "$(MINIAUDIO_DIR)/.git" || $(GIT) clone --depth 1 -b $(MINIAUDIO_REF) https://github.com/mackron/miniaudio.git $(MINIAUDIO_DIR)
	@echo "Dependencies ready under $(THIRD)/."

# ==========================================================================
# install
# ==========================================================================
install: lib cli
	@mkdir -p $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/morse $(DESTDIR)$(PREFIX)/bin
	cp $(LIBMORSE) $(DESTDIR)$(PREFIX)/lib/
	cp include/morse/*.h $(DESTDIR)$(PREFIX)/include/morse/
	cp $(CLI) $(DESTDIR)$(PREFIX)/bin/
	@test -f $(GUI) && cp $(GUI) $(DESTDIR)$(PREFIX)/bin/ || true
	@echo "Installed to $(DESTDIR)$(PREFIX)."

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libmorse.a
	rm -rf $(DESTDIR)$(PREFIX)/include/morse
	rm -f $(DESTDIR)$(PREFIX)/bin/morsec$(EXE) $(DESTDIR)$(PREFIX)/bin/morse-deluxe-gui$(EXE)

# ==========================================================================
# misc
# ==========================================================================
format:
	@command -v clang-format >/dev/null 2>&1 && \
	  clang-format -i src/*.c src/*.h include/morse/*.h app/cli/*.c app/gui/*.cpp app/gui/*.hpp app/gui/*.c tests/*.c tests/*.h && \
	  echo "Formatted." || echo "clang-format not found."

print-config:
	@echo "PLATFORM      = $(PLATFORM)"
	@echo "CC            = $(CC)"
	@echo "CXX           = $(CXX)"
	@echo "DSA_ROOT      = $(DSA_ROOT)"
	@echo "IMGUI_DIR     = $(IMGUI_DIR)"
	@echo "MINIAUDIO_DIR = $(MINIAUDIO_DIR)"
	@echo "GLFW (libs)   = $(GLFW_LIBS)"
	@echo "PREFIX        = $(PREFIX)"

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(THIRD)

help:
	@echo "morse-deluxe make targets:"
	@echo "  make            core library + CLI + tests"
	@echo "  make check      build & run unit tests"
	@echo "  make gui        Dear ImGui desktop app (needs OpenGL/GLFW, run 'make deps' first)"
	@echo "  make everything core + CLI + tests + GUI"
	@echo "  make deps       fetch imgui/miniaudio into third_party/"
	@echo "  make install    install under PREFIX=$(PREFIX)"
	@echo "  make clean      remove build/"
	@echo "  make print-config   show detected settings"

# ---- auto dependency tracking -------------------------------------------
-include $(MORSE_OBJ:.o=.d)
-include $(GUI_OBJ:.o=.d)
-include $(TEST_BIN:%$(EXE)=%.d)
-include $(CLI:%$(EXE)=%.d)
