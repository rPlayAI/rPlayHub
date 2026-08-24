/* Minimal arch layer for macOS/Linux (a hosted C environment). */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <sys/time.h>
#define LWIP_NO_STDINT_H 0
#define LWIP_NO_INTTYPES_H 0
#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_PLATFORM_DIAG(x)   do { } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwip assert: %s\n", x); abort(); } while(0)
#include <stdio.h>
#include <stdlib.h>
#endif
