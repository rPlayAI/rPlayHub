# cdhost — standalone Makefile for the C port.
#
# Support code comes from the AccessorySDK vendored at ../deps/AccessorySDK (see its
# PROVENANCE.md). Everything the build needs lives inside this repo.
#
#   macOS (fast on-device verify): link system CoreFoundation — its plist API is identical to
#                                  the SDK's CFLite, so the same source builds either way.
#   Linux/Windows:                 compile against the vendored CFLite + crypto sources.

UNAME := $(shell uname)

# TLS backend: openssl (default) or mbedtls.
#
#   make TLS=mbedtls
#
# mbedtls is what ~/rplay and ~/carplay-dev use, vendored at ../deps/mbedtls, and it is what makes
# the Linux and Windows ports tractable -- built from source, no system dependency to find. It is
# also the swap to make if the transport is ever suspected of corrupting data, which is a
# measurement rather than an argument.
TLS ?= openssl
MBEDTLS_ROOT ?= ../deps/mbedtls

SDK_ROOT     ?= ../deps/AccessorySDK
SDK_SUPPORT  := $(SDK_ROOT)/Support
SDK_EXTERNAL := $(SDK_ROOT)/External

CFLAGS  += -Wall -Wextra -O2
ifeq ($(UNAME),Darwin)
  LDFLAGS += -framework CoreFoundation
else
  CFLAGS  += -I$(SDK_SUPPORT) -I$(SDK_EXTERNAL)
  LDFLAGS += -lpthread
endif

ifeq ($(TLS),mbedtls)
  # Compiled from the vendored source rather than linked against a system library, so the same
  # command works on every platform.
  MBEDTLS_SRC := $(wildcard $(MBEDTLS_ROOT)/library/*.c)
  CFLAGS  += -I$(MBEDTLS_ROOT)/include -I$(MBEDTLS_ROOT)/library
  TLS_SRC := tls_mbedtls.c $(MBEDTLS_SRC)
else
  OPENSSL ?= /opt/homebrew/opt/openssl@3
  CFLAGS  += -I$(OPENSSL)/include
  LDFLAGS += -L$(OPENSSL)/lib -lssl -lcrypto
  TLS_SRC := tls_openssl.c
endif

# The protocol core is shared with the app and the ports, so it is compiled from ../core rather
# than duplicated here. Those files are platform-neutral by design -- no sockets, no allocation,
# no Apple headers -- which is what lets the same objects build on Linux and Windows.
CORE_DIR := ../core
CORE_SRC := $(CORE_DIR)/rp_xpc.c $(CORE_DIR)/rp_http2.c $(CORE_DIR)/rp_remotexpc.c \
            $(CORE_DIR)/rp_coredevice.c $(CORE_DIR)/rp_media_offer.c \
            $(CORE_DIR)/rp_rtp.c $(CORE_DIR)/rp_rtp_assembler.c $(CORE_DIR)/rp_rtcp.c
CFLAGS   += -I$(CORE_DIR)

all: cdhost

cdhost: cdhost.c api_server.c media.c ddi.c $(CORE_SRC) $(TLS_SRC)
	@echo "  TLS backend: $(TLS)"
	$(CC) $(CFLAGS) -o $@ cdhost.c api_server.c media.c ddi.c $(CORE_SRC) $(TLS_SRC) $(LDFLAGS) -lz

clean:
	rm -f cdhost

.PHONY: all clean
