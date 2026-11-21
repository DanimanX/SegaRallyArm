/* Portable Linux pthread core-pinning helper for 4-core ARM64 handhelds
 * (Allwinner H700: Core 0 main/GL, Core 1 geometry, Core 2 sound, Core 3 2D).
 */
#ifndef PLATFORM_ARM64_THREAD_AFFINITY_H
#define PLATFORM_ARM64_THREAD_AFFINITY_H

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int arm64_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static inline int arm64_pin_current_thread(int core)
{
    if (core < 0)
        return -1;
    int n = arm64_cpu_count();
    if (n <= 1 || core >= n)
        return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static inline void arm64_set_thread_name(const char *name)
{
    if (name)
        pthread_setname_np(pthread_self(), name);
}

#ifdef __cplusplus
}
#endif

#else /* !__linux__ */

static inline int arm64_cpu_count(void) { return 1; }
static inline int arm64_pin_current_thread(int core) { (void)core; return -1; }
static inline void arm64_set_thread_name(const char *name) { (void)name; }

#endif /* __linux__ */

#endif /* PLATFORM_ARM64_THREAD_AFFINITY_H */
