package io.github.jqssun.airplay.renderer

// mirrors PTS_SANITY_NS in clock_convert.h: a pts this far from now comes from a stale
// sender clock mapping, not a real frame time, so it is treated as unsynced (0)
internal const val PTS_SANITY_NS = 2_000_000_000L

internal fun sanePtsNs(ptsNs: Long, nowNs: Long): Long {
    val d = nowNs - ptsNs
    return if (ptsNs != 0L && (d > PTS_SANITY_NS || d < -PTS_SANITY_NS)) 0L else ptsNs
}
