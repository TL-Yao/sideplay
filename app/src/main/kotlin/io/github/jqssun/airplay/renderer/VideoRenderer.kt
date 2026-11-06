package io.github.jqssun.airplay.renderer

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Process
import android.util.Log
import android.view.Surface
import io.github.jqssun.airplay.renderer.DecoderSelector.Companion.videoCaps
import java.util.Locale
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.LockSupport
import kotlin.concurrent.thread

class VideoRenderer(ctx: Context) {

    private val lock = Object()
    private val pipeline = VideoPipeline()
    val selector = DecoderSelector(ctx)
    private var avcDecoder: MediaCodecInfo? = null
    private var hevcDecoder: MediaCodecInfo? = null
    private var maxFps = 0
    private var codec: MediaCodec? = null
    private var displaySurface: Surface? = null
    private var currentH265 = false
    private var videoWidth = 0
    private var videoHeight = 0
    private var firstFrameQueued = false

    // stats
    @Volatile var fps = 0; private set
    @Volatile var bitrateBps = 0L; private set
    @Volatile var frameCount = 0L; private set
    @Volatile var codecName = ""; private set
    @Volatile var droppedFrames = 0L; private set
    @Volatile var framePacingJitterUs = 0L; private set
    @Volatile var lateFrames = 0L; private set

    // frames are shown at sender pts + L_eff; L_eff starts at this base and rises with decode delay
    @Volatile var targetLatencyMs = BASE_LATENCY_MS
    @Volatile var effectiveLatencyMs = BASE_LATENCY_MS; private set
    // called on the output thread (or the caller of a reset) with each new L_eff
    var onEffectiveLatencyChanged: ((Int) -> Unit)? = null
    // p95 of audio packet arrival - pts (ns, 0 = none); polled once a second by the output thread
    var audioLatencySource: (() -> Long)? = null
    private val floor = LatencyFloor(BASE_LATENCY_MS)

    var enforceSdr = true
    var keyAllowFrameDrop = true
    var benchmarkLog = false
    var benchmarkLogCallback: ((String) -> Unit)? = null
    private var _framesThisSec = 0
    private var _bytesThisSec = 0L
    private var _lastStatReset = 0L
    private val _frameIntervalsNs = LongArray(120)
    private var _frameIntervalIdx = 0
    private var _frameIntervalCount = 0
    private var _lastOutputFrameNs = 0L

    // fetch thread dequeues and timestamps decoded frames; present thread waits for their time and releases
    private var _outputThreads = emptyList<Thread>()
    private val _readyFrames = LinkedBlockingQueue<ReadyFrame>()
    @Volatile private var _outputRunning = false
    @Volatile private var _outputFailed = false
    // owned by the present thread
    private var _syncWindowStartNs = 0L
    private var _offsetSumNs = 0L
    private var _offsetMaxNs = 0L
    private var _offsetCount = 0
    private var _nextAudioPollNs = 0L
    private var _audioP95Ns = 0L
    @Volatile private var _ptsRejected = 0  // cumulative, written under lock

    fun setResolution(w: Int, h: Int) {
        if (w != videoWidth || h != videoHeight) _resetLatency()
        videoWidth = w
        videoHeight = h
        pipeline.setVideoSize(w, h)
    }

    // doesn't restart codec; decoder renders into pipeline's own persistent surface
    fun setSurface(surface: Surface) = synchronized(lock) {
        displaySurface = surface
        pipeline.setDisplaySurface(surface)
    }

    fun clearSurface(surface: Surface) = synchronized(lock) {
        if (displaySurface !== surface) return@synchronized
        displaySurface = null
        pipeline.setDisplaySurface(null)
    }

    fun selectDecoders(w: Int, h: Int, fps: Int, h265: Boolean): Boolean = synchronized(lock) {
        avcDecoder = selector.avc()
        hevcDecoder = if (h265) selector.hevc(avcDecoder, w, h, fps) else null
        maxFps = fps
        Log.i(TAG, "decoders: avc=${avcDecoder?.name} hevc=${hevcDecoder?.name}")
        hevcDecoder != null
    }

    // unknown limits default to 1080p
    fun maxResolution(): Pair<Int, Int> =
        listOfNotNull(avcDecoder?.let { it to DecoderSelector.AVC }, hevcDecoder?.let { it to DecoderSelector.HEVC })
            .map { (info, mime) ->
                runCatching { info.videoCaps(mime).let { it.supportedWidths.upper to it.supportedHeights.upper } }
                    .getOrDefault(1920 to 1080)
            }
            .reduceOrNull { (w1, h1), (w2, h2) -> minOf(w1, w2) to minOf(h1, h2) } ?: (1920 to 1080)

    // codec per mirror session; pipeline persists across sessions
    fun startSession() = synchronized(lock) { _resetStats(); _resetLatency() }

    fun stopSession() = synchronized(lock) { stopCodec(); _resetLatency() }

    private fun _resetLatency() {
        floor.reset(targetLatencyMs)
        _publishLatency(force = true)
    }

    private fun _publishLatency(force: Boolean = false) = synchronized(floor) {
        val ms = floor.current()
        if (!force && ms == effectiveLatencyMs) return@synchronized
        effectiveLatencyMs = ms
        onEffectiveLatencyChanged?.invoke(ms)
    }

    private fun _resetStats() {
        fps = 0; bitrateBps = 0; frameCount = 0; codecName = ""
        droppedFrames = 0; framePacingJitterUs = 0; lateFrames = 0
        _framesThisSec = 0; _bytesThisSec = 0
    }

    private fun _updateStats(size: Int) {
        val now = System.currentTimeMillis()
        if (now - _lastStatReset >= 1000) {
            fps = _framesThisSec
            bitrateBps = _bytesThisSec * 8
            framePacingJitterUs = _computeFramePacingJitterUs()
            _framesThisSec = 0
            _bytesThisSec = 0
            _lastStatReset = now
            if (benchmarkLog) _emitBenchmarkLine()
        }
        _framesThisSec++
        _bytesThisSec += size
        frameCount++
    }

    private fun _emitBenchmarkLine() {
        val msg = "fps=$fps bitrate=${bitrateBps / 1000}kbps " +
            "jitter=${framePacingJitterUs}us frames=$frameCount " +
            "dropped=$droppedFrames codec=$codecName " +
            "res=${videoWidth}x${videoHeight}"
        Log.i(BENCH_TAG, msg)
        benchmarkLogCallback?.invoke(msg)
    }

    fun feedFrame(data: ByteArray, rawPtsNs: Long, isH265: Boolean) {
        synchronized(lock) {
            // a rejected pts takes the unsynced path: the frame is drawn immediately
            val ntpTimeNs = sanePtsNs(rawPtsNs, System.nanoTime())
            if (ntpTimeNs != rawPtsNs) _ptsRejected++
            if (_outputFailed) stopCodec()
            _updateStats(data.size)
            if (videoWidth == 0 || videoHeight == 0) return

            if (codec == null || isH265 != currentH265) {
                // a stale reference frame decodes to corruption, so wait for a keyframe to (re)start
                if (!_isKeyframe(data, isH265)) {
                    if (codec != null) stopCodec()
                    return
                }
                stopCodec()
            }

            try {
                if (codec == null) startCodec(isH265)
                _feedToCodec(data, ntpTimeNs)
            } catch (e: Exception) {
                Log.w(TAG, "Codec error, resetting", e)
                stopCodec()
            }
        }
    }

    private fun _feedToCodec(data: ByteArray, ntpTimeNs: Long) {
        val c = codec ?: return
        // dropping a frame desyncs decoder until the next keyframe, but source would only send one on (re)connect
        val retries = if (firstFrameQueued) FEED_RETRIES else FIRST_FEED_RETRIES
        repeat(retries) {
            val idx = c.dequeueInputBuffer(FEED_WAIT_US)
            if (idx >= 0) {
                val buf = c.getInputBuffer(idx) ?: return
                buf.clear()
                buf.put(data)
                c.queueInputBuffer(idx, 0, data.size, ntpTimeNs / 1000, 0)
                firstFrameQueued = true
                return
            }
        }
        // also the visible symptom of the output thread holding buffers while it waits for their time
        droppedFrames++
        Log.w(TAG, "Decoder input queue full; dropping frame. drops=$droppedFrames")
    }

    private fun _isKeyframe(data: ByteArray, isH265: Boolean): Boolean {
        if (data.size < 5) return false
        var i = 0
        while (i <= data.size - 5) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()) {
                val key = if (isH265) {
                    val type = (data[i + 4].toInt() shr 1) and 0x3F
                    type == 19 || type == 20 || type == 21 || type == 32 || type == 33
                } else {
                    val type = data[i + 4].toInt() and 0x1F
                    type == 5 || type == 7
                }
                if (key) return true
            }
            i++
        }
        return false
    }

    private fun startCodec(h265: Boolean) {
        pipeline.start()
        pipeline.setVideoSize(videoWidth, videoHeight)
        val s = pipeline.inputSurface ?: return
        currentH265 = h265
        val mime = if (h265) DecoderSelector.HEVC else DecoderSelector.AVC
        val info = (if (h265) hevcDecoder else avcDecoder) ?: error("no decoder selected for $mime")

        firstFrameQueued = false
        try {
            _startWithLadder(info, mime, s, h265)
        } catch (e: Exception) {
            // strict hw decoders reject configs beyond their real limits
            val sw = selector.software(mime, videoWidth, videoHeight) ?: throw e
            Log.w(TAG, "Hardware decoder failed, trying software fallback", e)
            _startWithLadder(sw, mime, s, h265)
        }
        Log.i(TAG, "Video codec started: $mime ${videoWidth}x${videoHeight} ($codecName)")
        _resetLatency()
        codec?.let { _startOutput(it) }
    }

    private fun _startWithLadder(info: MediaCodecInfo, mime: String, s: Surface, h265: Boolean) {
        var tryNum = 0
        while (true) {
            val format = _format(mime, info)
            val more = selector.lowLatencyOptions(format, info, mime, tryNum)
            try {
                _startDecoder(MediaCodec.createByCodecName(info.name), format, s, h265)
                return
            } catch (e: Exception) {
                if (!more) throw e
                Log.w(TAG, "configure try $tryNum failed: $format", e)
                tryNum++
            }
        }
    }

    private fun _format(mime: String, info: MediaCodecInfo) = MediaFormat.createVideoFormat(mime, videoWidth, videoHeight).apply {
        setInteger(MediaFormat.KEY_FRAME_RATE, maxFps)
        if (selector.adaptive(info, mime)) {
            setInteger(MediaFormat.KEY_MAX_WIDTH, videoWidth)
            setInteger(MediaFormat.KEY_MAX_HEIGHT, videoHeight)
        }
        setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxOf(videoWidth * videoHeight * 3 / 4, 1024 * 1024))
        if (enforceSdr) {
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        }
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, if (keyAllowFrameDrop) 1 else 0)
        }
    }

    private fun _startDecoder(c: MediaCodec, format: MediaFormat, surface: Surface, h265: Boolean) {
        try {
            c.configure(format, surface, null, 0)
            c.start()
        } catch (e: Exception) {
            try { c.release() } catch (_: Exception) {}
            throw e
        }
        codec = c
        codecName = (if (h265) "H.265" else "H.264") + " (${c.name})"
    }

    private fun stopCodec() {
        _stopOutput()
        _frameIntervalIdx = 0
        _frameIntervalCount = 0
        _lastOutputFrameNs = 0L
        codec?.let {
            try {
                it.stop()
                it.release()
            } catch (_: Exception) {}
        }
        codec = null
    }

    // neither thread takes `lock`, so stopCodec can join them while holding the lock
    private fun _startOutput(c: MediaCodec) {
        val intervalNs = 1_000_000_000L / (if (maxFps > 0) maxFps else 60)
        _outputFailed = false
        _outputRunning = true
        _readyFrames.clear()
        _syncWindowStartNs = System.nanoTime()
        _outputThreads = listOf(
            thread(name = "VideoOutputFetch") { _guardOutput { _fetchLoop(c) } },
            thread(name = "VideoOutput") { _guardOutput { _presentLoop(c, intervalNs) } },
        )
    }

    private fun _stopOutput() {
        if (_outputThreads.isEmpty()) return
        _outputRunning = false
        _outputThreads.forEach { LockSupport.unpark(it) }
        _outputThreads.forEach { it.join() }
        _outputThreads = emptyList()
        _readyFrames.clear() // their buffers go back with codec.stop()
        _outputFailed = false
    }

    private inline fun _guardOutput(loop: () -> Unit) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY)
        try {
            loop()
        } catch (e: Exception) {
            // the next feedFrame sees the flag and runs the usual stopCodec-and-wait-for-keyframe reset
            Log.w(TAG, "Codec error on output thread, resetting", e)
            _outputFailed = true
        }
    }

    // never waits on presentation, so readyNs is when the decoder actually produced the frame
    private fun _fetchLoop(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (_outputRunning) {
            val idx = c.dequeueOutputBuffer(info, OUTPUT_WAIT_US)
            if (idx < 0) continue
            val readyNs = System.nanoTime()
            val ptsNs = info.presentationTimeUs * 1000
            if (ptsNs != 0L) {
                floor.observeReadyDelayNs(readyNs - ptsNs)
                _publishLatency()
            }
            _readyFrames.put(ReadyFrame(idx, ptsNs))
        }
    }

    private fun _presentLoop(c: MediaCodec, intervalNs: Long) {
        while (_outputRunning) {
            _readyFrames.poll(OUTPUT_WAIT_US, TimeUnit.MICROSECONDS)?.let { _present(c, it.idx, it.ptsNs, intervalNs) }
            _maybePollAudio()
            _maybeLogSync()
        }
    }

    private fun _present(c: MediaCodec, idx: Int, ptsNs: Long, intervalNs: Long) {
        if (ptsNs == 0L) { // sender clock not synced yet
            c.releaseOutputBuffer(idx, true)
            _recordOutputFrameTime()
            return
        }
        val target = ptsNs + floor.current() * 1_000_000L - DISPLAY_LAG_NS
        var now = System.nanoTime()
        while (now < target && _outputRunning) {
            LockSupport.parkNanos(target - now)
            now = System.nanoTime()
        }
        val late = floor.isLate(target, now, intervalNs)
        _publishLatency()
        if (late || !_outputRunning) {
            c.releaseOutputBuffer(idx, false)
            if (late) lateFrames++
            return
        }
        c.releaseOutputBuffer(idx, true)
        _recordOutputFrameTime()
        val offset = now - target
        _offsetSumNs += offset
        _offsetMaxNs = maxOf(_offsetMaxNs, offset)
        _offsetCount++
    }

    private fun _maybePollAudio() {
        val source = audioLatencySource ?: return
        val now = System.nanoTime()
        if (now < _nextAudioPollNs) return
        _nextAudioPollNs = now + AUDIO_POLL_INTERVAL_NS
        _audioP95Ns = source()
        floor.observeAudioP95Ns(_audioP95Ns)
        _publishLatency()
    }

    private fun _maybeLogSync() {
        val now = System.nanoTime()
        if (now - _syncWindowStartNs < SYNC_LOG_INTERVAL_NS) return
        val meanMs = if (_offsetCount > 0) _offsetSumNs / _offsetCount / 1e6 else 0.0
        Log.i(TAG, String.format(Locale.US,
            "video sync: L=%dms(base %d) ready p50=%.1fms p95=%.1fms offset mean=%.1fms max=%.1fms late=%d dropped=%d audioP95=%.1fms ptsRejected=%d",
            effectiveLatencyMs, targetLatencyMs, floor.readyPercentileNs(50) / 1e6, floor.readyPercentileNs(95) / 1e6,
            meanMs, _offsetMaxNs / 1e6, lateFrames, droppedFrames, _audioP95Ns / 1e6, _ptsRejected))
        _syncWindowStartNs = now
        _offsetSumNs = 0; _offsetMaxNs = 0; _offsetCount = 0
    }

    fun release() = synchronized(lock) {
        stopCodec()
        pipeline.release()
        _resetStats()
    }

    private fun _recordOutputFrameTime() {
        val now = System.nanoTime()
        if (_lastOutputFrameNs > 0) {
            _frameIntervalsNs[_frameIntervalIdx % _frameIntervalsNs.size] = now - _lastOutputFrameNs
            _frameIntervalIdx++
            _frameIntervalCount++
        }
        _lastOutputFrameNs = now
    }

    private fun _computeFramePacingJitterUs(): Long {
        val count = _frameIntervalCount.coerceAtMost(_frameIntervalsNs.size)
        if (count < 2) return 0

        var sum = 0.0
        var sumSq = 0.0
        for (i in 0 until count) {
            val interval = _frameIntervalsNs[i].toDouble()
            sum += interval
            sumSq += interval * interval
        }
        val mean = sum / count
        val variance = (sumSq / count) - (mean * mean)
        return (kotlin.math.sqrt(variance.coerceAtLeast(0.0)) / 1000.0).toLong()
    }

    private class ReadyFrame(val idx: Int, val ptsNs: Long)

    companion object {
        private const val TAG = "VideoRenderer"
        private const val BENCH_TAG = "BENCHMARK"
        private const val FEED_WAIT_US = 20_000L
        private const val FEED_RETRIES = 10
        private const val FIRST_FEED_RETRIES = 50
        const val BASE_LATENCY_MS = 90
        private const val OUTPUT_WAIT_US = 10_000L
        // a released buffer reaches the screen about one vsync later (SurfaceTexture -> GL blit -> compositor)
        private const val DISPLAY_LAG_NS = 16_000_000L
        private const val SYNC_LOG_INTERVAL_NS = 5_000_000_000L
        private const val AUDIO_POLL_INTERVAL_NS = 1_000_000_000L
    }
}
