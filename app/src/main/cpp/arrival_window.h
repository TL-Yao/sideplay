#ifndef ARRIVAL_WINDOW_H
#define ARRIVAL_WINDOW_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "audio_time.h"

/*
 * Last WINDOW_NS of audio arrival delays (local arrival - sender pts, CLOCK_MONOTONIC),
 * at most CAPACITY samples (oldest dropped). One writer (decode thread), any reader.
 */
class ArrivalWindow {
public:
    void add(int64_t nowNs, int64_t delayNs) {
        std::lock_guard<std::mutex> lk(mLock);
        if (mSamples.size() >= CAPACITY) mSamples.pop_front();
        mSamples.emplace_back(nowNs, delayNs);
        evict(nowNs);
    }

    // nearest-rank percentile of samples within the window ending at nowNs; 0 if none
    int64_t percentileNs(int pct, int64_t nowNs = monoNs()) {
        std::lock_guard<std::mutex> lk(mLock);
        evict(nowNs);
        if (mSamples.empty()) return 0;
        std::vector<int64_t> d;
        d.reserve(mSamples.size());
        for (const auto &s : mSamples) d.push_back(s.second);
        std::sort(d.begin(), d.end());
        const int64_t n = (int64_t)d.size();
        const int64_t i = std::clamp((n * pct + 99) / 100 - 1, (int64_t)0, n - 1);
        return d[(size_t)i];
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mLock);
        mSamples.clear();
    }

    static constexpr size_t CAPACITY = 2048;
    static constexpr int64_t WINDOW_NS = 5 * NS_PER_SEC;

private:
    void evict(int64_t nowNs) {  // caller holds mLock
        while (!mSamples.empty() && mSamples.front().first < nowNs - WINDOW_NS) mSamples.pop_front();
    }

    std::mutex mLock;
    std::deque<std::pair<int64_t, int64_t>> mSamples;  // (observedAtNs, delayNs)
};

#endif  // ARRIVAL_WINDOW_H
