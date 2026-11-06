package io.github.jqssun.airplay.renderer

// Effective presentation delay L_eff (ms after sender pts) shared by video and audio:
// never below base, raised at once when video decoding or audio arrival gets slower (the larger of
// the two wins), lowered only after both stay faster.
class LatencyFloor(private var baseMs: Int, private val clock: () -> Long = System::nanoTime) {

    private val samples = ArrayDeque<Pair<Long, Long>>() // (observedAtNs, readyDelayNs)
    private var currentMs = baseMs
    private var videoCandidateMs = baseMs
    private var audioCandidateMs = baseMs
    private var belowSinceNs = NONE
    private var consecutiveLate = 0
    private var warmupLeft = WARMUP_FRAMES

    @Synchronized
    fun reset(baseMs: Int) {
        this.baseMs = baseMs
        samples.clear()
        currentMs = baseMs
        videoCandidateMs = baseMs
        audioCandidateMs = baseMs
        belowSinceNs = NONE
        consecutiveLate = 0
        warmupLeft = WARMUP_FRAMES
    }

    @Synchronized
    fun current(): Int = currentMs

    @Synchronized
    fun observeReadyDelayNs(ns: Long) {
        if (warmupLeft > 0) { warmupLeft--; return }
        val now = clock()
        samples.addLast(now to ns)
        while (samples.first().first < now - WINDOW_NS) samples.removeFirst()

        videoCandidateMs = _candidateMs(_percentile(95))
        _update(now)
    }

    // p95 of audio packet arrival - pts; 0 = no audio samples
    @Synchronized
    fun observeAudioP95Ns(ns: Long) {
        audioCandidateMs = if (ns == 0L) baseMs else _candidateMs(ns)
        _update(clock())
    }

    private fun _candidateMs(p95Ns: Long): Int = minOf(maxOf(baseMs, _ceil10Ms(p95Ns + MARGIN_NS)), MAX_MS)

    private fun _update(now: Long) {
        val candidate = maxOf(videoCandidateMs, audioCandidateMs)
        when {
            candidate > currentMs -> _raise(candidate)
            candidate < currentMs - LOWER_HYSTERESIS_MS -> {
                if (belowSinceNs == NONE) belowSinceNs = now
                else if (now - belowSinceNs >= LOWER_AFTER_NS) { currentMs = candidate; belowSinceNs = NONE }
            }
            else -> belowSinceNs = NONE
        }
    }

    // true = drop the frame; a long run of drops is broken by drawing and raising L_eff past the lateness
    @Synchronized
    fun isLate(targetNs: Long, nowNs: Long, frameIntervalNs: Long): Boolean {
        val lateNs = nowNs - targetNs
        if (lateNs <= frameIntervalNs) { consecutiveLate = 0; return false }
        if (consecutiveLate < RESCUE_AFTER_DROPS) { consecutiveLate++; return true }
        consecutiveLate = 0
        _raise(_ceil10Ms(currentMs * 1_000_000L + lateNs + MARGIN_NS))
        return false
    }

    @Synchronized
    fun readyPercentileNs(pct: Int): Long = if (samples.isEmpty()) 0 else _percentile(pct)

    private fun _raise(ms: Int) {
        currentMs = maxOf(currentMs, minOf(ms, MAX_MS))
        belowSinceNs = NONE
    }

    private fun _percentile(pct: Int): Long {
        val delays = LongArray(samples.size) { samples[it].second }.apply { sort() }
        return delays[((delays.size * pct + 99) / 100 - 1).coerceIn(0, delays.size - 1)]
    }

    private fun _ceil10Ms(ns: Long): Int = (Math.floorDiv(ns + STEP_NS - 1, STEP_NS) * 10).toInt()

    companion object {
        private const val NONE = Long.MIN_VALUE
        private const val WINDOW_NS = 5_000_000_000L
        private const val LOWER_AFTER_NS = 10_000_000_000L
        private const val LOWER_HYSTERESIS_MS = 20
        private const val MARGIN_NS = 10_000_000L
        private const val STEP_NS = 10_000_000L
        private const val MAX_MS = 500
        private const val RESCUE_AFTER_DROPS = 15
        // the first frames after a (re)started codec decode far slower than steady state; not counted
        private const val WARMUP_FRAMES = 5
    }
}
