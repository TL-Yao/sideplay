package io.github.jqssun.airplay.renderer

/**
 * mirrors native AudioConfig struct; defaults must match preference defaults so a
 * freshly-constructed AudioRenderer is sane
 */
data class AudioConfig(
    val cushionMs: Int = 0,           // no longer used by the engine (playout = pts + target latency)
    val percentilePct: Int = 95,      // no longer used by the engine (adaptive tuner removed)
    val oboeBufferFrames: Int = 0,    // 0 = auto (two bursts)
    val forceSwAlac: Boolean = false,
    val realtimePriority: Boolean = true,
    val lowLatency: Boolean = true,
    val benchmarkLog: Boolean = false,
)
