#ifndef TIMELINE_BUFFER_H
#define TIMELINE_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "audio_time.h"

/*
 * Playout buffer keyed by absolute output time. Position p counts frames on the
 * CLOCK_MONOTONIC axis (p = t * sampleRate): the sample stored at p is due at the DAC at
 * time t. The producer places each decoded block at pts + target latency; the consumer
 * reads the positions the DAC is about to play. Late blocks are dropped, early ones wait
 * (silence until due): that wait is the jitter buffer, there is no priming and no trim.
 *
 * One producer thread (write) + one consumer thread (read); flush() only while the
 * consumer is stopped. samples = frames * channels
 */
class TimelineBuffer {
public:
    // packed: nests into AudioDebugData with fixed layout
    struct __attribute__((packed)) Debug {
        uint32_t lateDrops;        // cumulative: blocks past their slot + read-head jumps over queued audio
        uint32_t silenceFills;     // cumulative: >= 1 ms gaps between blocks played as silence
        uint32_t underruns;        // cumulative: callbacks silenced because read head ran ahead of DAC
        int32_t offsetMeanUs;      // last 5 s window: DAC time - (pts + L)
        int32_t offsetMaxUs;       // last 5 s window: largest |offset|
        uint16_t targetLatencyMs;
        uint32_t dacFallbacks;     // cumulative: callbacks whose DAC time had no stream timestamp
        int32_t dacJitterMaxUs;    // last 5 s window: largest |DAC step - previous callback's duration|
    };

    TimelineBuffer(int sampleRate, int channels)
        : mSampleRate(sampleRate), mChannels(channels),
          mCapFrames((int64_t)sampleRate * CAPACITY_MS / 1000),
          mTolFrames((int64_t)sampleRate * TOLERANCE_NS / NS_PER_SEC),
          mBuf(new int16_t[(size_t)mCapFrames * channels]()) {}

    // any thread; applies to blocks written from now on
    void setTargetLatencyNs(int64_t ns) { mLatencyNs.store(ns, std::memory_order_relaxed); }

    // producer: ptsMonoNs = sender timestamp on local CLOCK_MONOTONIC, 0 = not synced yet
    void write(const int16_t *pcm, size_t samples, int64_t ptsMonoNs) {
        int64_t n = (int64_t)(samples / mChannels);
        if (n <= 0) return;
        const uint32_t gen = mFlushGen.load(std::memory_order_acquire);
        const bool flushed = gen != mAckGen.load(std::memory_order_relaxed);
        if (flushed) mHaveRun = false;

        const int64_t rh = mReadPos.load(std::memory_order_acquire);
        const int64_t latency = mLatencyNs.load(std::memory_order_relaxed);
        const int64_t end = mWriteEnd.load(std::memory_order_relaxed);
        int64_t pos;
        if (ptsMonoNs == 0) {
            pos = mHaveRun ? end : frameAt(monoNs() + latency);
            if (rh != NONE && pos < rh) pos = rh;
        } else {
            pos = frameAt(ptsMonoNs + latency);
            if (rh != NONE && pos < rh) { countLate(); return; }
            if (mHaveRun) {
                const int64_t d = pos - end;
                if (d < -mTolFrames) {
                    // overlaps queued audio (backward clock step, duplicate): keep what's queued
                    if (-d >= n) { countLate(); return; }
                    pcm += -d * mChannels;
                    n += d;
                    pos = end;
                } else if (d < mTolFrames) {
                    pos = end;  // rounding jitter: stay contiguous
                } else {
                    mSilenceFills.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // never more than capacity ahead of playout (unanchored: DAC can't be before now)
        const int64_t floor = rh != NONE ? rh : frameAt(monoNs());
        if (pos + n > floor + mCapFrames) return;

        if (!mHaveRun) mRunStart.store(pos, std::memory_order_release);
        // gap before block plays as silence; positions behind read head or aliased out of
        // the ring (consumer treats < writeEnd - capacity as empty) need no zeroing
        int64_t zeroFrom = std::max(mHaveRun ? end : pos, pos + n - mCapFrames);
        if (rh != NONE) zeroFrom = std::max(zeroFrom, rh);
        if (zeroFrom < pos) fill(zeroFrom, pos - zeroFrom, nullptr);
        fill(pos, n, pcm);
        mWriteEnd.store(pos + n, std::memory_order_release);
        mHaveRun = true;
        if (flushed) mAckGen.store(gen, std::memory_order_release);
    }

    // consumer: out frame 0 leaves the DAC at firstFrameDacMonoNs (CLOCK_MONOTONIC)
    void read(int16_t *out, int32_t frames, int64_t firstFrameDacMonoNs) {
        const size_t need = (size_t)frames * mChannels;
        // flush not yet acknowledged by producer: ring still holds pre-flush audio
        if (mAckGen.load(std::memory_order_acquire) != mFlushGen.load(std::memory_order_relaxed)) {
            memset(out, 0, need * sizeof(int16_t));
            return;
        }
        const int64_t should = frameAt(firstFrameDacMonoNs);
        const int64_t end = mWriteEnd.load(std::memory_order_acquire);
        const int64_t start = std::max(mRunStart.load(std::memory_order_acquire), end - mCapFrames);
        int64_t rh = mReadPos.load(std::memory_order_relaxed);
        // within tolerance read head stays put: DAC/monotonic drift is absorbed by jumps
        // and holds, no resampling
        if (rh == NONE) {
            rh = should;
        } else if (should - rh > mTolFrames) {
            if (rh < end) countLate();  // skipped queued audio
            rh = should;
        } else if (rh - should > mTolFrames) {
            memset(out, 0, need * sizeof(int16_t));
            mUnderruns.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // [rh, a) silence, [a, b) queued audio, [b, rh + frames) silence
        const int64_t stop = rh + frames;
        const int64_t a = std::min(std::max(start, rh), stop);
        const int64_t b = std::min(std::max(end, a), stop);
        memset(out, 0, (size_t)(a - rh) * mChannels * sizeof(int16_t));
        copyOut(a, b - a, out + (a - rh) * mChannels);
        memset(out + (b - rh) * mChannels, 0, (size_t)(stop - b) * mChannels * sizeof(int16_t));
        mReadPos.store(stop, std::memory_order_release);

        if (b > a) noteOffset(firstFrameDacMonoNs - nsAt(rh), firstFrameDacMonoNs);
        else noteOffset(NONE, firstFrameDacMonoNs);
    }

    // consumer, before read(): DAC estimate of this callback's first frame; fallback = no
    // stream timestamp, guessed as now + buffer size
    void noteDacEstimate(int64_t dacNs, int32_t frames, bool fallback) {
        if (fallback) mDacFallbacks.fetch_add(1, std::memory_order_relaxed);
        const int64_t prev = mPrevDacNs.load(std::memory_order_relaxed);
        if (prev != NONE) {
            const int64_t j = (dacNs - prev) - mPrevFrames * NS_PER_SEC / mSampleRate;
            mWinDacJitterMaxNs = std::max(mWinDacJitterMaxNs, j < 0 ? -j : j);
        }
        mPrevDacNs.store(dacNs, std::memory_order_relaxed);
        mPrevFrames = frames;
    }

    // drop queued audio; only while consumer (output callback) is stopped
    void flush() {
        mReadPos.store(NONE, std::memory_order_relaxed);
        mPrevDacNs.store(NONE, std::memory_order_relaxed);  // restarted stream: no previous callback
        mFlushGen.fetch_add(1, std::memory_order_release);
    }

    // any thread
    Debug debugInfo() const {
        Debug d{};
        d.lateDrops = mLateDrops.load(std::memory_order_relaxed);
        d.silenceFills = mSilenceFills.load(std::memory_order_relaxed);
        d.underruns = mUnderruns.load(std::memory_order_relaxed);
        d.offsetMeanUs = mOffsetMeanUs.load(std::memory_order_relaxed);
        d.offsetMaxUs = mOffsetMaxUs.load(std::memory_order_relaxed);
        d.targetLatencyMs = (uint16_t)(mLatencyNs.load(std::memory_order_relaxed) / 1'000'000);
        d.dacFallbacks = mDacFallbacks.load(std::memory_order_relaxed);
        d.dacJitterMaxUs = mDacJitterMaxUs.load(std::memory_order_relaxed);
        return d;
    }

private:
    static constexpr int64_t NONE = INT64_MIN;           // read head not anchored / no offset sample
    static constexpr int CAPACITY_MS = 3000;
    static constexpr int64_t TOLERANCE_NS = 1'000'000;   // 1 ms: snap/hold window
    static constexpr int64_t WINDOW_NS = 5 * NS_PER_SEC; // offset stats window

    // split multiply: t * sampleRate overflows int64 after ~2 days of uptime
    int64_t frameAt(int64_t ns) const {
        return ns / NS_PER_SEC * mSampleRate + ns % NS_PER_SEC * mSampleRate / NS_PER_SEC;
    }
    int64_t nsAt(int64_t frame) const {
        return frame / mSampleRate * NS_PER_SEC + frame % mSampleRate * NS_PER_SEC / mSampleRate;
    }
    int64_t slotOf(int64_t pos) const { return ((pos % mCapFrames) + mCapFrames) % mCapFrames; }

    void countLate() { mLateDrops.fetch_add(1, std::memory_order_relaxed); }

    // producer: src == nullptr writes silence
    void fill(int64_t pos, int64_t frames, const int16_t *src) {
        while (frames > 0) {
            const int64_t slot = slotOf(pos);
            const int64_t k = std::min(frames, mCapFrames - slot);
            int16_t *dst = &mBuf[(size_t)slot * mChannels];
            const size_t bytes = (size_t)k * mChannels * sizeof(int16_t);
            if (src) { memcpy(dst, src, bytes); src += k * mChannels; }
            else memset(dst, 0, bytes);
            pos += k;
            frames -= k;
        }
    }

    void copyOut(int64_t pos, int64_t frames, int16_t *dst) const {
        while (frames > 0) {
            const int64_t slot = slotOf(pos);
            const int64_t k = std::min(frames, mCapFrames - slot);
            memcpy(dst, &mBuf[(size_t)slot * mChannels], (size_t)k * mChannels * sizeof(int16_t));
            dst += k * mChannels;
            pos += k;
            frames -= k;
        }
    }

    // consumer: accumulate offset samples, publish mean/max once per window
    void noteOffset(int64_t offsetNs, int64_t nowNs) {
        if (mWinStartNs == NONE) mWinStartNs = nowNs;
        if (offsetNs != NONE) {
            mWinSumNs += offsetNs;
            mWinCount++;
            mWinMaxAbsNs = std::max(mWinMaxAbsNs, offsetNs < 0 ? -offsetNs : offsetNs);
        }
        if (nowNs - mWinStartNs < WINDOW_NS) return;
        mOffsetMeanUs.store(mWinCount ? (int32_t)(mWinSumNs / mWinCount / 1000) : 0,
                            std::memory_order_relaxed);
        mOffsetMaxUs.store((int32_t)(mWinMaxAbsNs / 1000), std::memory_order_relaxed);
        mDacJitterMaxUs.store((int32_t)std::min<int64_t>(mWinDacJitterMaxNs / 1000, INT32_MAX),
                              std::memory_order_relaxed);
        mWinDacJitterMaxNs = 0;
        mWinStartNs = nowNs;
        mWinSumNs = 0;
        mWinCount = 0;
        mWinMaxAbsNs = 0;
    }

    const int mSampleRate;
    const int mChannels;
    const int64_t mCapFrames;
    const int64_t mTolFrames;
    std::unique_ptr<int16_t[]> mBuf;

    std::atomic<int64_t> mLatencyNs{90'000'000};
    std::atomic<int64_t> mReadPos{NONE};     // consumer-owned; producer reads for late check
    std::atomic<int64_t> mWriteEnd{0};       // producer-owned: end of queued audio
    std::atomic<int64_t> mRunStart{0};       // producer-owned: start of current run
    std::atomic<uint32_t> mFlushGen{0};      // bumped by flush()
    std::atomic<uint32_t> mAckGen{0};        // producer: last flush it has applied
    bool mHaveRun = false;                   // producer-only: mWriteEnd belongs to current run

    std::atomic<uint32_t> mLateDrops{0};
    std::atomic<uint32_t> mSilenceFills{0};
    std::atomic<uint32_t> mUnderruns{0};
    std::atomic<int32_t> mOffsetMeanUs{0};
    std::atomic<int32_t> mOffsetMaxUs{0};
    std::atomic<uint32_t> mDacFallbacks{0};
    std::atomic<int32_t> mDacJitterMaxUs{0};
    std::atomic<int64_t> mPrevDacNs{NONE};   // consumer; reset by flush() while consumer stopped
    int64_t mPrevFrames = 0;                 // consumer-only

    // consumer-only stats window
    int64_t mWinStartNs = NONE;
    int64_t mWinSumNs = 0;
    int64_t mWinCount = 0;
    int64_t mWinMaxAbsNs = 0;
    int64_t mWinDacJitterMaxNs = 0;
};

#endif  // TIMELINE_BUFFER_H
