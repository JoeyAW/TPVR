#undef NDEBUG
#include "adaptive_screen_resolution.hpp"

#include <cassert>
#include <cstdint>

namespace {

constexpr std::int64_t kPeriodNs = 10'000'000; // 100 refreshes a second
constexpr double kHeadroomWaitMs = 3.0;         // >= 20% of the period
constexpr double kTightWaitMs = 1.0;            // < 20% of the period

struct Refreshes {
    dusk::vr::AdaptiveScreenResolution controller;
    std::int64_t displayTimeNs = 1'000'000'000;
    int minPercent = 80;

    Refreshes() { controller.observe(displayTimeNs, kPeriodNs, kTightWaitMs, minPercent, true); }

    void interval(std::int64_t periods, double waitMs = kTightWaitMs) {
        displayTimeNs += periods * kPeriodNs;
        controller.observe(displayTimeNs, kPeriodNs, waitMs, minPercent, true);
    }

    void onTime(int count, double waitMs = kTightWaitMs) {
        for (int i = 0; i < count; ++i) {
            interval(1, waitMs);
        }
    }

    // A sustained-pressure drop: two missed frames 10 ms apart.
    void drop() {
        interval(2);
        interval(2);
    }

    int percent() const { return static_cast<int>(controller.resolution(1000, 500, 2000, 1000).width / 10); }
};

} // namespace

int main() {
    // Isolated misses, including one long hitch, never lower: each is over a second
    // after the previous one.
    {
        Refreshes r;
        r.interval(3);
        r.onTime(100);
        r.interval(40);
        r.onTime(100);
        r.interval(2);
        assert(r.percent() == 100);
    }

    // Two missed frames within a second lower two steps (10%).
    {
        Refreshes r;
        r.interval(2);
        r.onTime(90);
        assert(r.percent() == 100);
        r.interval(2);
        assert(r.percent() == 90);
    }

    // The floor holds, and a raised floor lifts the scale at once.
    {
        Refreshes r;
        r.drop();
        assert(r.percent() == 90);
        r.onTime(30);
        r.drop();
        assert(r.percent() == 80);
        r.onTime(30);
        r.drop();
        assert(r.percent() == 80);
        r.minPercent = 95;
        r.interval(1);
        assert(r.percent() == 95);
    }

    // Overload where every frame misses several refreshes (the device log that stayed at
    // 100%) still lowers, down to the floor.
    {
        Refreshes r;
        r.minPercent = 50;
        for (int i = 0; i < 100; ++i) {
            r.interval(5);
        }
        assert(r.percent() == 50);
    }

    // Misses in the 300 ms after a change are ignored: two there don't lower, and the
    // first one counted afterwards starts a fresh pair.
    {
        Refreshes r;
        r.drop();
        assert(r.percent() == 90);
        r.interval(2);
        r.interval(2);
        r.onTime(25);
        assert(r.percent() == 90);
        r.interval(2);
        assert(r.percent() == 90);
        r.interval(2);
        assert(r.percent() == 80);
    }

    // Recovery: one step after 1.5 s of on-time refreshes with runtime wait headroom; a
    // busy refresh restarts the wait.
    {
        Refreshes r;
        r.drop();
        assert(r.percent() == 90);
        r.onTime(100, kHeadroomWaitMs);
        r.onTime(1, kTightWaitMs);
        r.onTime(149, kHeadroomWaitMs);
        assert(r.percent() == 90);
        r.onTime(1, kHeadroomWaitMs);
        assert(r.percent() == 95);
    }

    // The ceiling: after a drop from 100 the scale may come back to 95 but not to 100
    // until 20 s after the drop.
    {
        Refreshes r;
        r.drop();
        assert(r.percent() == 90);
        r.onTime(149, kHeadroomWaitMs);
        assert(r.percent() == 90);
        r.onTime(1, kHeadroomWaitMs);
        assert(r.percent() == 95);
        r.onTime(1849, kHeadroomWaitMs);
        assert(r.percent() == 95);
        r.onTime(1, kHeadroomWaitMs);
        assert(r.percent() == 100);
    }

    // An ineligible stretch (screen mode, setting off) is not a missed interval.
    {
        Refreshes r;
        r.interval(2);
        r.displayTimeNs += kPeriodNs;
        r.controller.observe(r.displayTimeNs, kPeriodNs, kTightWaitMs, r.minPercent, false);
        r.displayTimeNs += 30 * kPeriodNs;
        r.controller.observe(r.displayTimeNs, kPeriodNs, kTightWaitMs, r.minPercent, true);
        r.onTime(10);
        assert(r.percent() == 100);
    }

    // Sizes fit the allocation, keep the base aspect, and shrink with the scale.
    {
        Refreshes r;
        const auto full = r.controller.resolution(1600, 900, 1000, 400);
        assert(full.width == 711 && full.height == 400);
        r.drop();
        const auto lower = r.controller.resolution(1600, 900, 1000, 400);
        assert(lower.width == 639 && lower.height == 360);
        assert(r.controller.resolution(0, 900, 1000, 400).width == 0);
    }
}
