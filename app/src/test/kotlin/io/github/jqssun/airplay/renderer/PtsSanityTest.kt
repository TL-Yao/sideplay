package io.github.jqssun.airplay.renderer

import org.junit.Assert.assertEquals
import org.junit.Test

class PtsSanityTest {
    private val now = 29_044_413_000_000L  // an uptime from the field log
    private val sec = 1_000_000_000L

    @Test fun unsyncedStaysZero() = assertEquals(0L, sanePtsNs(0L, now))
    @Test fun recentPtsKept() = assertEquals(now - sec, sanePtsNs(now - sec, now))
    @Test fun hoursOldPtsRejected() = assertEquals(0L, sanePtsNs(now - 3 * 3600 * sec, now))
    @Test fun farFuturePtsRejected() = assertEquals(0L, sanePtsNs(now + 3 * sec, now))
}
