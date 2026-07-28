# cdhost — standalone Makefile for the C port.
#
# Support code comes from the AccessorySDK vendored at ../deps/AccessorySDK (see its
# PROVENANCE.md). Everything the build needs lives inside this repo.
#
#   macOS (fast on-device verify): link system CoreFoundation — its plist API is identical to
#                                  the SDK's CFLite, so the same source builds either way.
#   Linux/Windows:                 compile against the vendored CFLite + crypto sources.

UNAME := $(shell uname)

SDK_ROOT     ?= ../deps/AccessorySDK
SDK_SUPPORT  := $(SDK_ROOT)/Support
SDK_EXTERNAL := $(SDK_ROOT)/External

CFLAGS  += -Wall -Wextra -O2
ifeq ($(UNAME),Darwin)
  OPENSSL ?= /opt/homebrew/opt/openssl@3
  CFLAGS  += -I$(OPENSSL)/include
  LDFLAGS += -framework CoreFoundation -L$(OPENSSL)/lib -lssl -lcrypto
else
  CFLAGS  += -I$(SDK_SUPPORT) -I$(SDK_EXTERNAL)
  LDFLAGS += -lssl -lcrypto -lpthread
endif

all: cdhost

cdhost: cdhost.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f cdhost

.PHONY: all clean
