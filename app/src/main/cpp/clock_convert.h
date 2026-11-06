#ifndef CLOCK_CONVERT_H
#define CLOCK_CONVERT_H

#include <stdint.h>

/* CLOCK_REALTIME ns -> CLOCK_MONOTONIC ns, given (mono - realtime) read back to back;
 * 0 means "not NTP-synced yet" and stays 0 */
static inline uint64_t mono_from_realtime_ns(uint64_t realtime_ns, int64_t mono_minus_realtime_ns) {
    return realtime_ns ? (uint64_t)((int64_t)realtime_ns + mono_minus_realtime_ns) : 0;
}

/* a stale rtp->ntp mapping (UxPlay keeps the previous stream's sync across an audio
 * TEARDOWN, so the first packets of the next stream are hours off) yields a pts nowhere
 * near now; such a pts would blow up the arrival-delay window, so treat it as unsynced */
#define PTS_SANITY_NS 2000000000LL

static inline int64_t sane_pts_ns(int64_t pts, int64_t now) {
    const int64_t d = now - pts;
    return pts && (d > PTS_SANITY_NS || d < -PTS_SANITY_NS) ? 0 : pts;
}

#endif  /* CLOCK_CONVERT_H */
