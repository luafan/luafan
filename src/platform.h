#ifndef FAN_PLATFORM_H
#define FAN_PLATFORM_H

/*
 * Centralized platform detection and compatibility shims.
 * Include this header instead of scattering platform #ifdefs across source files.
 */

/* ---- Platform detection ---- */

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define FAN_PLATFORM_IOS 1
#  else
#    define FAN_PLATFORM_MACOS 1
#  endif
#elif defined(__ANDROID__)
#  define FAN_PLATFORM_ANDROID 1
#elif defined(__linux__)
#  define FAN_PLATFORM_LINUX 1
#elif defined(_WIN32)
#  define FAN_PLATFORM_WINDOWS 1
#endif

/* ---- Endianness conversion (be64toh / htobe64) ---- */

#if !defined(be64toh) && !defined(htobe64)
#  if defined(FAN_PLATFORM_LINUX)
#    include <endian.h>
#  elif defined(FAN_PLATFORM_IOS)
#    include <libkern/OSByteOrder.h>
#    define be64toh(x) OSSwapBigToHostInt64(x)
#    define htobe64(x) OSSwapHostToBigInt64(x)
#  elif defined(FAN_PLATFORM_MACOS)
#    include <sys/endian.h>
#  elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#    include <sys/endian.h>
#  else
#    include <netinet/in.h>
#    if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#      define be64toh(x) __builtin_bswap64(x)
#      define htobe64(x) __builtin_bswap64(x)
#    else
#      define be64toh(x) (x)
#      define htobe64(x) (x)
#    endif
#  endif
#endif

/* ---- CPU affinity ---- */

#if defined(FAN_PLATFORM_LINUX)
#  include <sched.h>
#elif defined(FAN_PLATFORM_MACOS)
#  include <sys/sysctl.h>

#  define SYSCTL_CORE_COUNT "machdep.cpu.core_count"

typedef struct cpu_set {
    uint32_t count;
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t *cs) {
    cs->count = 0;
}

static inline void CPU_SET(int num, cpu_set_t *cs) {
    cs->count |= (1 << num);
}

static inline int CPU_ISSET(int num, cpu_set_t *cs) {
    return (cs->count & (1 << num));
}

static inline int sched_getaffinity(pid_t pid, size_t cpu_size, cpu_set_t *cpu_set) {
    (void)pid;
    (void)cpu_size;
    int32_t core_count = 0;
    size_t len = sizeof(core_count);
    int ret = sysctlbyname(SYSCTL_CORE_COUNT, &core_count, &len, 0, 0);
    if (ret) {
        return -1;
    }
    cpu_set->count = 0;
    for (int i = 0; i < core_count; i++) {
        cpu_set->count |= (1 << i);
    }
    return 0;
}

#endif /* CPU affinity */

/* ---- getdtablesize compatibility (Android lacks it) ---- */

#if defined(FAN_PLATFORM_ANDROID)
#  include <unistd.h>
#  define FAN_GETDTABLESIZE() sysconf(_SC_OPEN_MAX)
#else
#  include <unistd.h>
#  define FAN_GETDTABLESIZE() getdtablesize()
#endif

/* ---- ifaddrs.h include path (Android bundles its own) ---- */

#if defined(FAN_PLATFORM_ANDROID)
#  include "ifaddrs.h"
#else
#  include <ifaddrs.h>
#endif

#endif /* FAN_PLATFORM_H */
