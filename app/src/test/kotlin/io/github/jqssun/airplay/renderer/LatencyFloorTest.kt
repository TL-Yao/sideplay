package io.github.jqssun.airplay.renderer

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LatencyFloorTest {
    private var now = 0L
    private val floor = LatencyFloor(90) { now }

    private fun feed(delayMs: Int, seconds: Int, fps: Int = 60) {
        repeat(seconds * fps) {
            now += 1_000_000_000L / fps
            floor.observeReadyDelayNs(delayMs * 1_000_000L)
        }
    }

    @Test fun neverBelowBase() {
        feed(70, 2)
        assertEquals(90, floor.current())
    }

    @Test fun slowDecodeRaisesImmediately() {
        feed(130, 1)
        assertEquals(140, floor.current())
    }

    @Test fun lowersOnlyAfterTenSecondsBelow() {
        feed(130, 1)
        feed(70, 5) // the 130 ms samples have aged out of the 5 s window
        assertEquals(70_000_000L, floor.readyPercentileNs(95))
        feed(70, 5)
        assertEquals(140, floor.current())
        feed(70, 5)
        assertEquals(90, floor.current())
    }

    @Test fun cappedAt500() {
        feed(800, 1)
        assertEquals(500, floor.current())
    }

    @Test fun warmupFramesIgnoredThenFirstSampleRaisesAtOnce() {
        repeat(5) { now += 16_666_667; floor.observeReadyDelayNs(300_000_000L) }
        assertEquals(90, floor.current())
        now += 16_666_667
        floor.observeReadyDelayNs(130_000_000L)
        assertEquals(140, floor.current())
    }

    @Test fun warmupRestartsAfterReset() {
        feed(40, 1)
        floor.reset(90)
        repeat(5) { now += 16_666_667; floor.observeReadyDelayNs(300_000_000L) }
        assertEquals(90, floor.current())
    }

    // reviewer probe: once decoding recovers to a true 40 ms, L_eff must come back down to base
    @Test fun recoversToBaseAfterDecodeSpeedsUp() {
        feed(130, 1)
        assertEquals(140, floor.current())
        feed(40, 15)
        assertEquals(90, floor.current())
    }

    @Test fun lateMeansMoreThanOneFrameBehind() {
        val interval = 16_666_666L
        val t = 1_000_000_000L
        assertTrue(floor.isLate(t, t + interval + 1, interval))
        assertFalse(floor.isLate(t, t + interval, interval))
        assertFalse(floor.isLate(t, t - 5_000_000L, interval))
    }

    @Test fun consecutiveLateFramesRaiseInsteadOfDropping() {
        val interval = 16_666_666L
        val t = 1_000_000_000L
        repeat(15) { assertTrue(floor.isLate(t, t + 50_000_000L, interval)) }
        assertFalse(floor.isLate(t, t + 53_000_000L, interval))
        assertEquals(160, floor.current()) // 90 + 53 + 10, rounded up to 10 ms
        assertTrue(floor.isLate(t, t + 50_000_000L, interval)) // streak restarts
    }

    @Test fun resetReturnsToBase() {
        feed(200, 1)
        floor.reset(90)
        assertEquals(90, floor.current())
    }
}
