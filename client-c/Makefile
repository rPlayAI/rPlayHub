# rplay-view — the portable thin client (ffmpeg software decode + SDL2).
# Needs: pkg-config, libavcodec/libavutil (ffmpeg), SDL2.
#   macOS:  brew install ffmpeg sdl2
#   Debian: apt install libavcodec-dev libavutil-dev libsdl2-dev

PKGS   := libavcodec libavutil sdl2
CFLAGS += -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LIBS   := $(shell pkg-config --libs $(PKGS))

rplay-view: rplay-view.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f rplay-view

.PHONY: clean
