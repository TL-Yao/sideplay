// Host self-check for TimelineBuffer and the realtime->monotonic conversion.
// Build + run from the repo root (non-zero exit = failure):
//   clang++ -std=c++17 -Wall -Wextra -O1 -I app/src/main/cpp \
//     app/src/test/cpp/timeline_buffer_selfcheck.cpp -o /tmp/tb_selfcheck && /tmp/tb_selfcheck

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "clock_convert.h"
#include "timeline_buffer.h"

static int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); gFailures++; } } while (0)

static constexpr int SR = 48000;
static constexpr int CH = 2;
static constexpr int BLOCK = 480;                    // 10 ms
static constexpr int64_t BLOCK_NS = 10'000'000;
static constexpr int64_t MS = 1'000'000;
static constexpr int64_t L90 = 90 * MS;

// block i, frame k -> distinct non-zero sample, so silence and misplacement are both visible
static int16_t sampleOf(int block, int k) { return (int16_t)((block * BLOCK + k) % 32000 + 1); }

static std::vector<int16_t> makeBlock(int block) {
    std::vector<int16_t> v(BLOCK * CH);
    for (int k = 0; k < BLOCK; k++) { v[k * CH] = sampleOf(block, k); v[k * CH + 1] = (int16_t)-sampleOf(block, k); }
    return v;
}

static void writeBlock(TimelineBuffer &tb, int block, int64_t pts) {
    auto v = makeBlock(block);
    tb.write(v.data(), v.size(), pts);
}

static std::vector<int16_t> readBlock(TimelineBuffer &tb, int64_t dacNs) {
    std::vector<int16_t> out(BLOCK * CH, 0x5555);
    tb.read(out.data(), BLOCK, dacNs);
    return out;
}

static bool isBlock(const std::vector<int16_t> &out, int block) { return out == makeBlock(block); }

static bool isSilent(const std::vector<int16_t> &out, size_t from = 0, size_t to = BLOCK * CH) {
    for (size_t i = from; i < to; i++) if (out[i] != 0) return false;
    return true;
}

// base time near "now": the producer bounds unanchored writes against the real monotonic clock
static int64_t base() { return (monoNs() / NS_PER_SEC + 1) * NS_PER_SEC; }

static void caseClockConvert() {
    CHECK(mono_from_realtime_ns(0, -5) == 0);  // unsynced stays 0
    const uint64_t rt = 1'700'000'000'000'000'000ULL;
    const int64_t diff = -1'699'999'000'000'000'000LL;  // mono - realtime
    CHECK(mono_from_realtime_ns(rt, diff) == 1'000'000'000'000ULL);  // 1000 s of uptime
    CHECK(mono_from_realtime_ns(rt, 0) == rt);

    const int64_t now = 29'044'413LL * MS;  // an uptime from the field log
    CHECK(sane_pts_ns(0, now) == 0);
    CHECK(sane_pts_ns(now - 1000 * MS, now) == now - 1000 * MS);
    CHECK(sane_pts_ns(now - 3 * 3600'000LL * MS, now) == 0);  // stale sync, hours in the past
    CHECK(sane_pts_ns(now + 3000 * MS, now) == 0);
}

// (a) in-order writes read back on time: identical output, offset ~0
static void caseInOrder() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    const int N = 600;  // 6 s: covers one 5 s stats window
    int mismatches = 0;
    for (int i = 0; i < N; i++) {
        writeBlock(tb, i, b + i * BLOCK_NS);
        if (i >= 3 && !isBlock(readBlock(tb, b + (i - 3) * BLOCK_NS + L90), i - 3)) mismatches++;
    }
    CHECK(mismatches == 0);
    auto d = tb.debugInfo();
    CHECK(d.lateDrops == 0);
    CHECK(d.silenceFills == 0);
    CHECK(d.underruns == 0);
    CHECK(d.offsetMeanUs == 0);
    CHECK(d.offsetMaxUs == 0);
    CHECK(d.targetLatencyMs == 90);
}

// (a') after anchoring, DAC drifts 0.5 ms late: within 1 ms tolerance the read head stays
// put (no glitch) and the offset is reported
static void caseSubMsOffset() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    int mismatches = 0;
    for (int i = 0; i < 600; i++) {
        writeBlock(tb, i, b + i * BLOCK_NS);
        const int64_t drift = i > 3 ? 500'000 : 0;
        if (i >= 3 && !isBlock(readBlock(tb, b + (i - 3) * BLOCK_NS + L90 + drift), i - 3)) mismatches++;
    }
    CHECK(mismatches == 0);
    auto d = tb.debugInfo();
    CHECK(d.lateDrops == 0 && d.underruns == 0);
    CHECK(d.offsetMeanUs >= 495 && d.offsetMeanUs <= 500);
    CHECK(d.offsetMaxUs == 500);
}

// (b) block whose slot the read head already passed is dropped; later blocks play normally
static void caseLateDrop() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    writeBlock(tb, 0, b);
    writeBlock(tb, 1, b + BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    CHECK(isBlock(readBlock(tb, b + BLOCK_NS + L90), 1));
    CHECK(isSilent(readBlock(tb, b + 2 * BLOCK_NS + L90)));  // block 2 not here yet
    writeBlock(tb, 2, b + 2 * BLOCK_NS);                      // arrives too late
    CHECK(tb.debugInfo().lateDrops == 1);
    writeBlock(tb, 3, b + 3 * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + 3 * BLOCK_NS + L90), 3));
    CHECK(tb.debugInfo().lateDrops == 1);
    CHECK(tb.debugInfo().underruns == 0);
}

// (c) 50 ms gap between blocks plays as 50 ms of silence
static void caseGap() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    writeBlock(tb, 0, b);
    writeBlock(tb, 1, b + BLOCK_NS + 50 * MS);
    CHECK(tb.debugInfo().silenceFills == 1);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    for (int j = 1; j <= 5; j++) CHECK(isSilent(readBlock(tb, b + j * BLOCK_NS + L90)));
    CHECK(isBlock(readBlock(tb, b + 6 * BLOCK_NS + L90), 1));
    auto d = tb.debugInfo();
    CHECK(d.lateDrops == 0 && d.underruns == 0 && d.silenceFills == 1);
}

// (d) read head 300 ms behind (output stalled): next read jumps to where it should be
static void caseReadHeadBehind() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    for (int i = 0; i < 40; i++) writeBlock(tb, i, b + i * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    CHECK(isBlock(readBlock(tb, b + 31 * BLOCK_NS + L90), 31));  // 300 ms later than read head
    CHECK(isBlock(readBlock(tb, b + 32 * BLOCK_NS + L90), 32));
    auto d = tb.debugInfo();
    CHECK(d.lateDrops == 1);
    CHECK(d.underruns == 0);
}

// (e) target latency 90 -> 150 ms: new blocks play 60 ms later
static void caseLatencyChange() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    writeBlock(tb, 0, b);
    writeBlock(tb, 1, b + BLOCK_NS);
    tb.setTargetLatencyNs(150 * MS);
    CHECK(tb.debugInfo().targetLatencyMs == 150);
    writeBlock(tb, 2, b + 2 * BLOCK_NS);
    writeBlock(tb, 3, b + 3 * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    CHECK(isBlock(readBlock(tb, b + BLOCK_NS + L90), 1));
    for (int j = 0; j < 6; j++) CHECK(isSilent(readBlock(tb, b + (2 + j) * BLOCK_NS + L90)));
    CHECK(isBlock(readBlock(tb, b + 2 * BLOCK_NS + 150 * MS), 2));
    CHECK(isBlock(readBlock(tb, b + 3 * BLOCK_NS + 150 * MS), 3));
    auto d = tb.debugInfo();
    CHECK(d.silenceFills == 1 && d.lateDrops == 0 && d.underruns == 0);
}

// (f) pts 0 (not NTP-synced yet) appends right after the previous block
static void casePtsZero() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    writeBlock(tb, 0, b);
    writeBlock(tb, 1, 0);
    writeBlock(tb, 2, 0);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    CHECK(isBlock(readBlock(tb, b + BLOCK_NS + L90), 1));
    CHECK(isBlock(readBlock(tb, b + 2 * BLOCK_NS + L90), 2));
    auto d = tb.debugInfo();
    CHECK(d.silenceFills == 0 && d.lateDrops == 0 && d.underruns == 0);
}

// (g) read head > 1 ms ahead of DAC: silence, read head held, one underrun per callback
static void caseReadHeadAhead() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    for (int i = 0; i < 3; i++) writeBlock(tb, i, b + i * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    CHECK(isSilent(readBlock(tb, b + L90)));  // DAC didn't advance: read head 10 ms ahead
    CHECK(tb.debugInfo().underruns == 1);
    CHECK(isBlock(readBlock(tb, b + BLOCK_NS + L90), 1));
    CHECK(tb.debugInfo().underruns == 1);
}

// flush drops buffered audio; next writes start a fresh run
static void caseFlush() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    for (int i = 0; i < 5; i++) writeBlock(tb, i, b + i * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + L90), 0));
    tb.flush();
    CHECK(isSilent(readBlock(tb, b + BLOCK_NS + L90)));  // old block 1 gone
    writeBlock(tb, 10, b + 10 * BLOCK_NS);
    CHECK(isBlock(readBlock(tb, b + 10 * BLOCK_NS + L90), 10));
}

// DAC estimate stats: fallbacks counted; jitter = |DAC step - previous callback length|,
// published with the 5 s offset window; flush forgets the previous callback
static void caseDacEstimate() {
    TimelineBuffer tb(SR, CH);
    const int64_t b = base();
    tb.noteDacEstimate(b, BLOCK, true);
    tb.noteDacEstimate(b + BLOCK_NS, BLOCK, false);              // exact: 0
    tb.noteDacEstimate(b + 2 * BLOCK_NS - 3 * MS, BLOCK, false); // stepped back 3 ms
    tb.flush();
    tb.noteDacEstimate(b + 100 * BLOCK_NS, BLOCK, true);         // after restart: not compared
    CHECK(tb.debugInfo().dacFallbacks == 2);
    CHECK(tb.debugInfo().dacJitterMaxUs == 0);  // window not closed yet
    writeBlock(tb, 0, b);
    readBlock(tb, b + L90);                     // opens the window
    readBlock(tb, b + L90 + 6 * NS_PER_SEC);    // closes it
    CHECK(tb.debugInfo().dacJitterMaxUs == 3000);
}

int main() {
    caseClockConvert();
    caseInOrder();
    caseSubMsOffset();
    caseLateDrop();
    caseGap();
    caseReadHeadBehind();
    caseLatencyChange();
    casePtsZero();
    caseReadHeadAhead();
    caseFlush();
    caseDacEstimate();
    if (gFailures) { std::fprintf(stderr, "%d check(s) failed\n", gFailures); return 1; }
    std::printf("timeline_buffer selfcheck: all passed\n");
    return 0;
}
