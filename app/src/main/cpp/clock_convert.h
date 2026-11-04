#ifndef CLOCK_CONVERT_H
#define CLOCK_CONVERT_H

#include <stdint.h>

/* CLOCK_REALTIME ns -> CLOCK_MONOTONIC ns, given (mono - realtime) read back to back;
 * 0 means "not NTP-synced yet" and stays 0 */
static inline uint64_t mono_from_realtime_ns(uint64_t realtime_ns, int64_t mono_minus_realtime_ns) {
    return realtime_ns ? (uint64_t)((int64_t)realtime_ns + mono_minus_realtime_ns) : 0;
}

#endif  /* CLOCK_CONVERT_H */
