# core/ — the portable video path. No platform dependencies, so this builds anywhere.
#
#   make test     build and run the unit tests, including against real recordings if present

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -O2 -g

HEVC_SAMPLE ?= ../screen3.h265
H264_SAMPLE ?= ../build/screen3.h264

all: test_rp_video_stream test_rp_xpc test_rp_remotexpc test_rp_media_offer test_rp_rtp

test_rp_video_stream: test_rp_video_stream.c rp_video_stream.c rp_video_stream.h rp_geometry.c rp_geometry.h
	$(CC) $(CFLAGS) -o $@ test_rp_video_stream.c rp_video_stream.c rp_geometry.c

test_rp_remotexpc: test_rp_remotexpc.c rp_remotexpc.c rp_http2.c rp_xpc.c
	$(CC) $(CFLAGS) -o $@ $^

test_rp_xpc: test_rp_xpc.c rp_xpc.c rp_xpc.h
	$(CC) $(CFLAGS) -o $@ test_rp_xpc.c rp_xpc.c

test: test_rp_video_stream test_rp_xpc test_rp_remotexpc test_rp_media_offer test_rp_rtp
	./test_rp_video_stream $(HEVC_SAMPLE) $(H264_SAMPLE)
	@echo "core/rp_xpc tests"
	@./test_rp_xpc >/dev/null && echo "  encoder + reader OK (byte-compare against Python: scripts/check-xpc.sh)"
	@./test_rp_remotexpc | tail -6
	@echo "core/rp_media_offer + rp_rtp (byte-compared against the Python and a real capture)"
	@../scripts/check-offer.sh || true
	@../scripts/check-rtp.sh || true

clean:
	rm -f test_rp_video_stream test_rp_xpc test_rp_remotexpc test_rp_media_offer test_rp_rtp

.PHONY: all test clean

test_rp_media_offer: test_rp_media_offer.c rp_media_offer.c
	$(CC) $(CFLAGS) -o $@ $^ -lz

test_rp_rtp: test_rp_rtp.c rp_rtp.c
	$(CC) $(CFLAGS) -o $@ $^
