#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dusk::vr {

struct ScreenResolution {
    std::uint32_t width;
    std::uint32_t height;
};

class AdaptiveScreenResolution {
public:
    void observe(std::int64_t predictedDisplayTimeNs, std::int64_t predictedDisplayPeriodNs,
                 double waitFrameMs, bool frameEligible) noexcept {
        // Call once per eligible XR refresh, not once per rendered eye pair.
        // Display-time gaps reveal missed refreshes; waitFrameMs is runtime pacing headroom,
        // not a GPU timing measurement.
        if (!frameEligible || predictedDisplayTimeNs <= 0 || predictedDisplayPeriodNs <= 0 ||
            !std::isfinite(waitFrameMs) || waitFrameMs < 0.0) {
            lastDisplayTimeNs_ = 0;
            resetFeedback();
            return;
        }

        if (lastDisplayTimeNs_ == 0) {
            lastDisplayTimeNs_ = predictedDisplayTimeNs;
            resetFeedback();
            return;
        }

        if (predictedDisplayTimeNs <= lastDisplayTimeNs_) {
            lastDisplayTimeNs_ = predictedDisplayTimeNs;
            resetFeedback();
            return;
        }

        const std::int64_t deltaNs = predictedDisplayTimeNs - lastDisplayTimeNs_;
        lastDisplayTimeNs_ = predictedDisplayTimeNs;
        const std::int64_t remainder = deltaNs % predictedDisplayPeriodNs;
        const bool roundUp = remainder >= predictedDisplayPeriodNs / 2 + predictedDisplayPeriodNs % 2;
        const std::int64_t elapsedFrames =
            deltaNs / predictedDisplayPeriodNs + (roundUp ? 1 : 0);
        const std::int64_t remainderNs = roundUp ? remainder - predictedDisplayPeriodNs : remainder;
        if (elapsedFrames < 1 || elapsedFrames > kMaxGapFrames ||
            remainderNs < -predictedDisplayPeriodNs / 4 ||
            remainderNs > predictedDisplayPeriodNs / 4) {
            resetFeedback();
            return;
        }

        if (warmupIntervals_ < kWarmupIntervals) {
            ++warmupIntervals_;
            return;
        }

        const bool missed = elapsedFrames > 1;
        missWindow_ = ((missWindow_ << 1) | (missed ? 1u : 0u)) & kMissWindowMask;
        if (missed) {
            headroomNs_ = 0;
            if (std::popcount(missWindow_) >= kMissedIntervalsToLower) {
                tier_ = std::min(tier_ + 1, kScalePercent.size() - 1);
                missWindow_ = 0;
            }
            return;
        }

        if (tier_ == 0) {
            headroomNs_ = 0;
            return;
        }

        const double periodMs = static_cast<double>(predictedDisplayPeriodNs) / 1'000'000.0;
        if (waitFrameMs < periodMs * kHeadroomFraction) {
            headroomNs_ = 0;
            return;
        }

        headroomNs_ = std::min(kRecoveryNs, headroomNs_ + deltaNs);
        if (headroomNs_ >= kRecoveryNs) {
            --tier_;
            headroomNs_ = 0;
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

        const std::uint32_t scale = kScalePercent[tier_];
        return {static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * scale / 100),
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(height) * scale / 100)};
    }

private:
    static constexpr std::array<std::uint32_t, 4> kScalePercent{100, 85, 70, 60};
    static constexpr std::int64_t kRecoveryNs = 1'500'000'000;
    static constexpr unsigned kWarmupIntervals = 8;
    // Two missed intervals among the last four lower one tier: alternating rendered/cached
    // refreshes still register as sustained pressure, while one isolated miss ages out.
    static constexpr unsigned kMissWindowMask = 0b1111;
    static constexpr int kMissedIntervalsToLower = 2;
    static constexpr std::int64_t kMaxGapFrames = 4;
    static constexpr double kHeadroomFraction = 0.20;

    void resetFeedback() noexcept {
        warmupIntervals_ = 0;
        missWindow_ = 0;
        headroomNs_ = 0;
    }

    std::int64_t lastDisplayTimeNs_ = 0;
    std::int64_t headroomNs_ = 0;
    unsigned warmupIntervals_ = 0;
    unsigned missWindow_ = 0;
    std::size_t tier_ = 0;
};

} // namespace dusk::vr
