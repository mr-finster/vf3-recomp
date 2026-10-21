# Virtua Fighter 3 (Model 3) recomp: headless tools for now (build/bin/vf3run).
# ROM-derived data lives in build/ and outside the repo, and is never committed.
CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wno-unused-function
CXXFLAGS += -DSLJIT_CONFIG_AUTO=1 -Isrc/core/shim_include $(EXTRA_CFLAGS)
# The PowerPC's floating point must round each operation on its own, on every platform (no fused multiply-adds)
PPC_FLAGS = -ffp-contract=off
LDLIBS ?= -lz

OBJ ?= build/obj
BIN ?= build/bin
objs = $(patsubst %.cpp,$(OBJ)/%.o,$(1))

all: $(BIN)/vf3run $(BIN)/vf3lock $(BIN)/vf3soak $(BIN)/nettest $(BIN)/vf3net $(BIN)/vf3

SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null || echo -I/opt/homebrew/include)
SDL_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null || echo -L/opt/homebrew/lib -lSDL3)
FT_CFLAGS ?= $(shell pkg-config --cflags freetype2 2>/dev/null || echo -I/opt/homebrew/include/freetype2)
FT_LIBS ?= $(shell pkg-config --libs freetype2 2>/dev/null || echo -L/opt/homebrew/lib -lfreetype)

# the version and date shown in the menu: the git tag (or tag-commits-hash between releases, or the commit) and
# the date of the last commit. Builds without git are handed them in the environment.
VF3_VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)
VF3_DATE ?= $(shell git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
$(OBJ)/version.h: FORCE
	@mkdir -p $(OBJ)
	@printf '#define VF3_VERSION "%s"\n#define VF3_DATE "%s"\n' "$(VF3_VERSION)" "$(VF3_DATE)" > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@
FORCE:

# RmlUi (MIT, third_party/rmlui): the in-game menu. Its core and the SDL backend, built with FreeType.
RML_SRC = $(wildcard third_party/rmlui/Source/Core/*.cpp third_party/rmlui/Source/Core/*/*.cpp) \
	third_party/rmlui/Backends/RmlUi_Platform_SDL.cpp third_party/rmlui/Backends/RmlUi_Renderer_SDL.cpp
RML_CFLAGS = -DRMLUI_STATIC_LIB -DRMLUI_FONT_ENGINE_FREETYPE -DRMLUI_SDL_VERSION_MAJOR=3 -Ithird_party/rmlui/Include -Ithird_party/rmlui/Backends
$(OBJ)/third_party/rmlui/%.o: CXXFLAGS += $(RML_CFLAGS) $(FT_CFLAGS) $(SDL_CFLAGS) -w
$(OBJ)/src/host/main.o $(OBJ)/src/host/menu.o $(OBJ)/src/host/online.o $(OBJ)/src/host/sysinfo.o $(OBJ)/src/host/d3d11_map.o $(OBJ)/src/gpu/r3d_gpu.o $(OBJ)/tools/menutest.o: CXXFLAGS += $(SDL_CFLAGS) $(RML_CFLAGS)
$(OBJ)/src/host/main.o: $(OBJ)/version.h
$(OBJ)/src/host/main.o: CXXFLAGS += -I$(OBJ)
# Netplay (src/net): rollback sessions and their links, nothing of the game's (shared with VF1 and VF2 Recomp)
NET = src/net/session.cpp src/net/link.cpp

HOST = src/host/main.cpp src/host/menu.cpp src/host/online.cpp src/host/vf3_netplay.cpp src/host/sysinfo.cpp src/host/d3d11_map.cpp src/gpu/r3d_gpu.cpp $(NET)

CORE = src/ppc/ppc.cpp src/machine/model3.cpp src/machine/romset.cpp src/host/input_script.cpp \
	src/rec/ppc_rec.cpp src/rec/vf3_map.cpp src/video/tilegen.cpp src/video/r3d_scene.cpp src/video/r3d_raster.cpp \
	src/sound/soundboard.cpp src/sound/scsp.cpp src/sound/scspdsp.cpp src/core/mame_shim.cpp \
	third_party/mame/m68000/m68000.cpp third_party/mame/m68000/m68000-decode.cpp \
	third_party/mame/m68000/m68000-sdf.cpp third_party/mame/m68000/m68000-sdp.cpp

# SLJIT (BSD-2): the native code made at start-up. C, built by the C++ compiler's driver (-x c)
SLJIT_OBJ = $(OBJ)/third_party/sljit/sljitLir.o
$(SLJIT_OBJ): third_party/sljit/sljitLir.c
	@mkdir -p $(dir $@)
	$(CXX) -x c -O2 -w -DSLJIT_CONFIG_AUTO=1 -c $< -o $@


$(OBJ)/src/ppc/ppc.o: CXXFLAGS += $(PPC_FLAGS)

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BIN)/vf3run: $(call objs,$(CORE) tools/vf3run.cpp) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/vf3lock: $(call objs,$(CORE) tools/vf3lock.cpp) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/vf3soak: $(call objs,$(CORE) tools/vf3soak.cpp) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/nettest: $(call objs,$(NET) tools/nettest.cpp)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BIN)/vf3net: $(call objs,$(CORE) $(NET) src/host/vf3_netplay.cpp tools/vf3net.cpp) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# netfuzz: the session against a peer who sends anything, with AddressSanitizer and UBSan (its own objects)
$(BIN)/netfuzz: $(NET) tools/netfuzz.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $^ -o $@

# menutest: the Online pages drawn offscreen to pictures (SDL_VIDEO_DRIVER=offscreen)
$(BIN)/menutest: $(call objs,src/host/menu.cpp src/host/online.cpp src/host/vf3_netplay.cpp src/machine/model3.cpp src/machine/romset.cpp src/ppc/ppc.cpp $(NET) tools/menutest.cpp $(RML_SRC))
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SDL_LIBS) $(FT_LIBS) $(LDLIBS)

$(BIN)/vf3: $(call objs,$(CORE) $(HOST) $(RML_SRC)) $(SLJIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(SDL_LIBS) $(FT_LIBS) $(LDLIBS) -lpthread

clean:
	rm -rf build

.PHONY: all clean FORCE
-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
