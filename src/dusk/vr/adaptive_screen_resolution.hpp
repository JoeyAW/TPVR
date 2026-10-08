#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dusk::vr {

struct ScreenResolution {
    std::uint32_t width;
    std::uint32_t height;
};

// Dynamic render resolution, after GalaxyQuest's: a scale in 5% steps between a floor
// and 100%. Two frames that miss refreshes less than a second apart drop it two steps
// and keep it below the scale that missed for 20 s; it rises one step after 1.5 s
// without misses, changes or a busy frame. Misses in the 300 ms after a change are
// reallocation hitches and don't count. Time is the runtime's predicted display time.
class AdaptiveScreenResolution {
public:
    // Call once per eligible XR refresh, not once per rendered eye pair. minPercent is
    // the floor (live setting). waitFrameMs is runtime pacing headroom, not a GPU timing
    // measurement: a frame that barely waited in xrWaitFrame is busy.
    void observe(std::int64_t predictedDisplayTimeNs, std::int64_t predictedDisplayPeriodNs,
                 double waitFrameMs, int minPercent, bool frameEligible) noexcept {
        if (!frameEligible || predictedDisplayTimeNs <= 0 || predictedDisplayPeriodNs <= 0 ||
            !std::isfinite(waitFrameMs) || waitFrameMs < 0.0) {
            lastDisplayTimeNs_ = 0;
            return;
        }

        const std::int64_t now = predictedDisplayTimeNs;
        const int floor = std::clamp(minPercent, 1, 100);
        if (percent_ < floor) {
            setPercent(floor, now); // the floor was raised
        }
        if (lastDisplayTimeNs_ == 0 || now <= lastDisplayTimeNs_) {
            // (Re)starting: nothing to judge yet, and headroom has to show afresh.
            lastDisplayTimeNs_ = now;
            quietSinceNs_ = now;
            return;
        }

        const std::int64_t refreshes =
            (now - lastDisplayTimeNs_ + predictedDisplayPeriodNs / 2) / predictedDisplayPeriodNs;
        lastDisplayTimeNs_ = now;
        if (now < ignoreUntilNs_) {
            return;
        }

        if (refreshes > 1) {
            // One frame counts once however many refreshes it missed: a single hitch
            // (shader compile, stall) can miss several, an overloaded GPU keeps missing.
            quietSinceNs_ = now;
            missCount_ = missCount_ > 0 && now - lastMissNs_ < kMissWindowNs ? missCount_ + 1 : 1;
            lastMissNs_ = now;
            if (missCount_ >= 2) {
                ceilingPercent_ = percent_ - kStepPercent;
                ceilingUntilNs_ = now + kCeilingNs;
                setPercent(std::max(floor, percent_ - 2 * kStepPercent), now);
            }
            return;
        }

        if (waitFrameMs * 1'000'000.0 < static_cast<double>(predictedDisplayPeriodNs) * kHeadroomFraction) {
            quietSinceNs_ = now;
            return;
        }
        if (percent_ < 100 && now - quietSinceNs_ >= kRecoveryNs &&
            (percent_ + kStepPercent <= ceilingPercent_ || now >= ceilingUntilNs_)) {
            setPercent(std::min(percent_ + kStepPercent, 100), now);
        }
    }

    ScreenResolution resolution(std::uint32_t baseWidth, std::uint32_t baseHeight,
                                std::uint32_t allocationWidth,
                                std::uint32_t allocationHeight) const noexcept {
        if (baseWidth == 0 || baseHeight == 0 || allocationWidth == 0 || allocationHeight == 0) {
            return {0, 0};
        }

        std::uint32_t width = std::min(baseWidth, allocationWidth);
        std::uint32_t height = std::min(baseHeight, allocationHeight);
        if (static_cast<std::uint64_t>(width) * baseHeight >
            static_cast<std::uint64_t>(height) * baseWidth) {
            width = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(height) * baseWidth / baseHeight);
        } else {
            height = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(width) * baseHeight / baseWidth);
        }

        const auto scale = static_cast<std::uint64_t>(percent_);
        return {static_cast<std::uint32_t>(width * scale / 100),
                static_cast<std::uint32_t>(height * scale / 100)};
    }

private:
    static constexpr int kStepPercent = 5;
    static constexpr std::int64_t kMissWindowNs = 1'000'000'000;
    static constexpr std::int64_t kCooldownNs = 300'000'000;
    static constexpr std::int64_t kRecoveryNs = 1'500'000'000;
    static constexpr std::int64_t kCeilingNs = 20'000'000'000;
    static constexpr double kHeadroomFraction = 0.20;

    // Every decision, even one the floor leaves without effect, restarts the miss count,
    // the cooldown and the recovery wait.
    void setPercent(int percent, std::int64_t now) noexcept {
        percent_ = percent;
        missCount_ = 0;
        ignoreUntilNs_ = now + kCooldownNs;
        quietSinceNs_ = now;
    }

    std::int64_t lastDisplayTimeNs_ = 0;
    std::int64_t lastMissNs_ = 0;
    std::int64_t ignoreUntilNs_ = 0;
    std::int64_t quietSinceNs_ = 0;
    std::int64_t ceilingUntilNs_ = 0;
    int ceilingPercent_ = 100;
    int missCount_ = 0;
    int percent_ = 100;
};

} // namespace dusk::vr
