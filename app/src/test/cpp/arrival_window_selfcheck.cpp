// Host self-check for ArrivalWindow. Build + run from the repo root (non-zero exit = failure):
//   clang++ -std=c++17 -Wall -Wextra -I app/src/main/cpp \
//     app/src/test/cpp/arrival_window_selfcheck.cpp -o /tmp/aw && /tmp/aw

#include <cstdio>

#include "arrival_window.h"

static int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); gFailures++; } } while (0)

static constexpr int64_t MS = 1'000'000;

int main() {
    {  // empty window
        ArrivalWindow w;
        CHECK(w.percentileNs(50, 0) == 0);
        CHECK(w.percentileNs(95, 0) == 0);
    }
    {  // p50/p95 on delays 1..100 ms
        ArrivalWindow w;
        for (int i = 1; i <= 100; i++) w.add(i * MS, i * MS);
        CHECK(w.percentileNs(50, 100 * MS) == 50 * MS);
        CHECK(w.percentileNs(95, 100 * MS) == 95 * MS);
        CHECK(w.percentileNs(100, 100 * MS) == 100 * MS);
        CHECK(w.percentileNs(0, 100 * MS) == 1 * MS);
    }
    {  // samples older than 5 s don't count, on add and on read
        ArrivalWindow w;
        w.add(0, 300 * MS);  // 6 s before the next one
        w.add(6000 * MS, 70 * MS);
        CHECK(w.percentileNs(95, 6000 * MS) == 70 * MS);
        CHECK(w.percentileNs(95, 12000 * MS) == 0);  // reading 6 s later: window empty
    }
    {  // capacity: oldest dropped once full
        ArrivalWindow w;
        w.add(0, 500 * MS);
        for (size_t i = 0; i < ArrivalWindow::CAPACITY; i++) w.add(1 * MS, 10 * MS);
        CHECK(w.percentileNs(100, 1 * MS) == 10 * MS);
    }
    {  // clear
        ArrivalWindow w;
        w.add(0, 10 * MS);
        w.clear();
        CHECK(w.percentileNs(50, 0) == 0);
    }
    if (gFailures) { std::fprintf(stderr, "%d failure(s)\n", gFailures); return 1; }
    std::puts("arrival_window_selfcheck: OK");
    return 0;
}
