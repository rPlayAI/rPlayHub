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

# libimobiledevice stack (Stage-1 migration: device_info, and more to come). Homebrew prefix on
# macOS; a Linux/Windows build points these at the system or vendored copies.
IMD_PREFIX ?= /opt/homebrew
CFLAGS  += -I$(IMD_PREFIX)/include

# STATIC=1 links the libimobiledevice stack from its .a archives, so the shipped binary needs no
# installed libimobiledevice/libplist (only OpenSSL, which the DMG bundles, and system libs). The
# order matters -- dependents before dependencies -- and libplist is C++, hence -lc++. Homebrew
# ships these .a files; a Linux/Windows build points the paths at its own static build.
ifeq ($(STATIC),1)
  # Strip unused code so a whole-archive static link does not bloat the binary. macOS's linker
  # dead-strips at the symbol/atom level; -ffunction-sections helps it split further.
  CFLAGS  += -ffunction-sections -fdata-sections
  # -dead_strip removes unused functions; -S/-x strip debug and local symbols (this is a shipped
  # binary, not one you debug). Together they cut the whole-archive static link by ~1.2 MB.
  LDFLAGS += -Wl,-dead_strip -Wl,-S -Wl,-x
  IMD_LIBS := $(IMD_PREFIX)/opt/libimobiledevice/lib/libimobiledevice-1.0.a \
              $(IMD_PREFIX)/opt/libimobiledevice-glue/lib/libimobiledevice-glue-1.0.a \
              $(IMD_PREFIX)/opt/libusbmuxd/lib/libusbmuxd-2.0.a \
              $(IMD_PREFIX)/opt/libplist/lib/libplist-2.0.a -lc++
else
  IMD_LIBS := -L$(IMD_PREFIX)/lib -limobiledevice-1.0 -lplist-2.0
endif
LDFLAGS += $(IMD_LIBS)

# CoreFoundation is gone: the engine's plists are libplist and its usbmux/lockdown is
# libimobiledevice, so no macOS framework is needed. (The CFLite include for non-Darwin is no
# longer needed either now that nothing uses CFLite.)
ifneq ($(UNAME),Darwin)
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
  ifeq ($(STATIC),1)
    # Link OpenSSL from its .a too, so the shipped binary needs no OpenSSL dylib -- only system
    # libs remain. libimobiledevice.a's OpenSSL symbols resolve against these same archives.
    LDFLAGS += $(OPENSSL)/lib/libssl.a $(OPENSSL)/lib/libcrypto.a
  else
    LDFLAGS += -L$(OPENSSL)/lib -lssl -lcrypto
  endif
  TLS_SRC := tls_openssl.c
endif

# The protocol core is shared with the app and the ports, so it is compiled from ../core rather
# than duplicated here. Those files are platform-neutral by design -- no sockets, no allocation,
# no Apple headers -- which is what lets the same objects build on Linux and Windows.
# Userspace TCP/IP over the tunnel (lwIP) -- removes the root utun. Compiled in always; the
# engine uses it only when RPLAY_USERSPACE_NET=1 (default stays the kernel utun). This is what
# makes the engine run unprivileged and portable (no TUN device on Linux).
LWIP_DIR  := ../deps/lwip
LWIP_PORT := $(LWIP_DIR)/contrib/ports/unix/port
# HAVE_USERNET compiles media.c's userspace (lwIP) networking path. The macOS app also builds
# media.c, but without usernet/lwIP, so the guard keeps it on kernel sockets there.
CFLAGS    += -DHAVE_USERNET -I lwip-engine -I $(LWIP_DIR)/src/include -I $(LWIP_PORT)/include
LWIP_SRC  := $(LWIP_DIR)/src/core/init.c $(LWIP_DIR)/src/core/def.c $(LWIP_DIR)/src/core/inet_chksum.c \
             $(LWIP_DIR)/src/core/ip.c $(LWIP_DIR)/src/core/mem.c $(LWIP_DIR)/src/core/memp.c \
             $(LWIP_DIR)/src/core/netif.c $(LWIP_DIR)/src/core/pbuf.c $(LWIP_DIR)/src/core/tcp.c \
             $(LWIP_DIR)/src/core/tcp_in.c $(LWIP_DIR)/src/core/tcp_out.c $(LWIP_DIR)/src/core/timeouts.c \
             $(LWIP_DIR)/src/core/udp.c $(LWIP_DIR)/src/core/sys.c \
             $(LWIP_DIR)/src/core/ipv6/ip6.c $(LWIP_DIR)/src/core/ipv6/ip6_addr.c \
             $(LWIP_DIR)/src/core/ipv6/icmp6.c $(LWIP_DIR)/src/core/ipv6/nd6.c \
             $(LWIP_DIR)/src/core/ipv6/ip6_frag.c $(LWIP_DIR)/src/core/ipv6/inet6.c \
             $(LWIP_DIR)/src/core/ipv6/mld6.c \
             $(LWIP_DIR)/src/api/api_lib.c $(LWIP_DIR)/src/api/api_msg.c $(LWIP_DIR)/src/api/netbuf.c \
             $(LWIP_DIR)/src/api/sockets.c $(LWIP_DIR)/src/api/tcpip.c $(LWIP_DIR)/src/api/err.c \
             $(LWIP_PORT)/sys_arch.c

CORE_DIR := ../core
CORE_SRC := $(CORE_DIR)/rp_xpc.c $(CORE_DIR)/rp_http2.c $(CORE_DIR)/rp_remotexpc.c \
            $(CORE_DIR)/rp_coredevice.c $(CORE_DIR)/rp_media_offer.c \
            $(CORE_DIR)/rp_rtp.c $(CORE_DIR)/rp_rtp_assembler.c $(CORE_DIR)/rp_rtcp.c
CFLAGS   += -I$(CORE_DIR)

all: cdhost

cdhost: cdhost.c api_server.c media.c ddi.c imd.c usernet.c tunio.c $(CORE_SRC) $(LWIP_SRC) $(TLS_SRC)
	@echo "  TLS backend: $(TLS)"
	$(CC) $(CFLAGS) -o $@ cdhost.c api_server.c media.c ddi.c imd.c usernet.c tunio.c $(CORE_SRC) $(LWIP_SRC) $(TLS_SRC) $(LDFLAGS) -lz
	$(if $(filter 1,$(STATIC)),strip $@,)   # full symbol strip on the shipped static build

clean:
	rm -f cdhost

.PHONY: all clean
