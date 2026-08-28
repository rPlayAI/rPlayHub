# rplay-view — the portable thin client (ffmpeg software decode + SDL2).
#
# Decode links the RVRA-PATCHED ffmpeg in ../deps/ffmpeg when it has been built
# (scripts/build-ffmpeg-rvra.sh) — required for a correct picture: this stream needs RVRA
# reference resampling, which stock ffmpeg lacks (doc/RVRA-AND-PORTABILITY.md). Without it the
# build falls back to the system ffmpeg via pkg-config and prints a warning: everything works,
# but pictures garble from the first resolution downshift under motion.
#
# Needs: pkg-config, SDL2 (and, for the fallback, system libavcodec/libavutil).
#   macOS:  brew install ffmpeg sdl2
#   Debian: apt install libavcodec-dev libavutil-dev libsdl2-dev

FFMPEG_DIR ?= ../deps/ffmpeg

CFLAGS += -Wall -Wextra -O2
LIBS   := $(shell pkg-config --libs sdl2)

ifneq ($(wildcard $(FFMPEG_DIR)/libavcodec/libavcodec.a),)
  # The patched tree FIRST, before SDL's pkg-config flags drag in /opt/homebrew/include --
  # the system ffmpeg headers live there too, and silently shadowing the patched ones with
  # them only works while the two versions happen to match.
  CFLAGS += -I$(FFMPEG_DIR)
  LIBS   += $(FFMPEG_DIR)/libavcodec/libavcodec.a $(FFMPEG_DIR)/libswresample/libswresample.a \
            $(FFMPEG_DIR)/libavutil/libavutil.a -lm -lpthread
else
  $(warning $(FFMPEG_DIR) not built -- linking SYSTEM ffmpeg; RVRA streams will garble under motion. Run scripts/build-ffmpeg-rvra.sh.)
  CFLAGS += $(shell pkg-config --cflags libavcodec libavutil)
  LIBS   += $(shell pkg-config --libs libavcodec libavutil)
endif
CFLAGS += $(shell pkg-config --cflags sdl2)

rplay-view: rplay-view.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# The full GUI (mirror pane + inspector tabs) — a SEPARATE app: Dear ImGui over the same SDL2
# renderer. rplay-view stays the untouched minimal reference viewer; stream-core.c is the GUI's
# verbatim copy of its stream engine (kept in sync by hand — the reference captures verify both).
# Needs deps/imgui + deps/json (scripts/fetch-gui-deps.sh) and a C++ compiler; `make` alone still
# builds only the minimal viewer.
IMGUI_DIR ?= ../deps/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer2.cpp
CXXFLAGS += $(CFLAGS) -std=c++17 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I../deps/json

rplay-gui: rplay-gui.cpp stream-core.c stream-core.h
	$(CC) $(CFLAGS) -c stream-core.c -o stream-core.o
	$(CXX) $(CXXFLAGS) -o $@ rplay-gui.cpp stream-core.o $(IMGUI_SRC) $(LIBS)

clean:
	rm -f rplay-view rplay-gui stream-core.o

.PHONY: clean
