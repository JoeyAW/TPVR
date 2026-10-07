#undef NDEBUG
#include "adaptive_screen_resolution.hpp"

#include <cassert>
#include <cstdint>

namespace {

constexpr std::int64_t kPeriodNs = 10'000'000;
constexpr double kHeadroomWaitMs = 3.0; // >= 20% of the period
constexpr double kTightWaitMs = 1.0;    // < 20% of the period

struct Refreshes {
    dusk::vr::AdaptiveScreenResolution controller;
    std::int64_t displayTimeNs = 1'000'000'000;

    void interval(std::int64_t periods, double waitMs = kTightWaitMs) {
        displayTimeNs += periods * kPeriodNs;
        controller.observe(displayTimeNs, kPeriodNs, waitMs, true);
    }

    void warmUp() {
        controller.observe(displayTimeNs, kPeriodNs, kHeadroomWaitMs, true);
        for (int i = 0; i < 8; ++i) {
            interval(1, kHeadroomWaitMs);
        }
    }

    void expectSize(std::uint32_t width, std::uint32_t height) const {
        const auto size = controller.resolution(1600, 900, 2000, 1200);
        assert(size.width == width);
        assert(size.height == height);
    }
};

} // namespace

int main() {
    // Isolated misses, including one longer hitch, age out without lowering.
    {
        Refreshes r;
        r.warmUp();
        r.interval(2);
        r.interval(1);
        r.interval(1);
        r.interval(1);
        r.interval(2);
        r.interval(1);
        r.interval(1);
        r.interval(1);
        r.interval(3);
        r.expectSize(1600, 900);
    }

    // Half-rate cadence: each rendered refresh misses while the cached refresh fits.
    {
        Refreshes r;
        r.warmUp();
        r.interval(2);
        r.interval(1);
        r.expectSize(1600, 900);
        r.interval(2);
        r.expectSize(1360, 765);
        r.interval(1);
        r.interval(2);
        r.interval(1);
        r.interval(2);
        r.expectSize(1120, 630);
    }

    // Recovery needs 1.5 s of on-time refreshes that also show runtime wait headroom.
    {
        Refreshes r;
        r.warmUp();
        r.interval(2);
        r.interval(2);
        r.expectSize(1360, 765);
        for (int i = 0; i < 150; ++i) {
            r.interval(1, kTightWaitMs);
        }
        r.expectSize(1360, 765);
        for (int i = 0; i < 149; ++i) {
            r.interval(1, kHeadroomWaitMs);
        }
        r.expectSize(1360, 765);
        r.interval(1, kHeadroomWaitMs);
        r.expectSize(1600, 900);
    }

    // Focus loss and long stalls restart warm-up without discarding the current tier.
    {
        Refreshes r;
        r.warmUp();
        r.interval(2);
        r.interval(2);
        r.expectSize(1360, 765);

        r.displayTimeNs += kPeriodNs;
        r.controller.observe(r.displayTimeNs, kPeriodNs, kTightWaitMs, false);
        r.displayTimeNs += 50 * kPeriodNs;
        r.controller.observe(r.displayTimeNs, kPeriodNs, kTightWaitMs, true);
        r.interval(2);
        r.interval(2);
        r.expectSize(1360, 765);

        for (int i = 0; i < 6; ++i) {
            r.interval(1);
        }
        r.interval(10);
        r.interval(2);
        r.interval(2);
        r.expectSize(1360, 765);
    }

    // Every tier fits the swapchain allocation, shrinks monotonically, and clamps at the floor.
    {
        Refreshes r;
        r.warmUp();
        std::uint32_t previousWidth = 0;
        for (int tier = 0; tier < 4; ++tier) {
            const auto size = r.controller.resolution(1600, 900, 1000, 400);
            assert(size.width > 0 && size.width <= 1000);
            assert(size.height > 0 && size.height <= 400);
            assert(tier == 0 || size.width < previousWidth);
            previousWidth = size.width;
            r.interval(2);
            r.interval(2);
        }
        assert(r.controller.resolution(1600, 900, 1000, 400).width == previousWidth);
    }
}
