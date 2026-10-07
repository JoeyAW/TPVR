#pragma once

#include <cstdint>

namespace dusk::vr {

struct ScreenPairInput {
    bool cacheable = false;
    bool stereo = false;
    uint32_t width = 0;
    uint32_t height = 0;
    float gammaExponent = 1.0f;
    float stereoDepth = 1.0f;
    uint64_t simTick = 0;
    uint64_t presentationEpoch = 0;
    float interpolationStep = 1.0f;
    bool presentationSync = false;
};

struct ScreenPairPlan {
    bool cacheable = false;
    bool stereo = false;
    bool copyFront = false;
    bool publishRendered = false;
    bool startPair = false;
    bool finishPair = false;
    bool freezePresentation = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t renderMask = 0;
    uint32_t renderGeneration = 0;
    uint32_t frontGeneration = 0;
    float frozenStep = 1.0f;
    uint64_t simTick = 0;
    uint64_t presentationEpoch = 0;
    bool presentationSync = false;
    float interpolationStep = 1.0f;
};

// Giant Screen scene-eye cache schedule (pure; Session does the GPU work).
//
// Rendered eyes live in two buffer generations. The FRONT generation always
// holds one complete image set -- a mono image, or a stereo pair rendered from
// one scene state -- and is copied into every newly acquired swapchain image.
// Mono re-renders every other refresh in place. Stereo builds the next pair in
// the BACK generation one eye per refresh: eye 0, then eye 1 re-presented at
// eye 0's interpolation step, then the generations flip. A re-presented step
// only reproduces the scene while the sim tick, presentation epoch and sync
// state are unchanged, so any change in between discards the partial pair.
// Cold caches, and sim ticks landing between eyes kMaxDiscardedPairs times in
// a row, render both eyes in one refresh instead.
class ScreenPairScheduler {
public:
    static constexpr uint32_t kMaxDiscardedPairs = 2;

    ScreenPairPlan begin(const ScreenPairInput& input) {
        ScreenPairPlan plan{};
        plan.stereo = input.stereo;
        plan.width = input.width;
        plan.height = input.height;
        plan.simTick = input.simTick;
        plan.presentationEpoch = input.presentationEpoch;
        plan.presentationSync = input.presentationSync;
        plan.interpolationStep = input.interpolationStep;

        if (!input.cacheable || input.width == 0 || input.height == 0) {
            invalidate();
            plan.renderMask = input.stereo ? 3u : 1u;
            return plan;
        }
        plan.cacheable = true;

        const bool modeChanged = !keyValid_ || stereo_ != input.stereo;
        const bool outputChanged = !keyValid_ || gammaExponent_ != input.gammaExponent ||
                                   stereoDepth_ != input.stereoDepth;
        // A size change waits for the pair in flight; the next pair adopts it.
        const bool deferResize = pendingPair_ && !modeChanged && !outputChanged;
        const uint32_t width = deferResize ? width_ : input.width;
        const uint32_t height = deferResize ? height_ : input.height;
        const bool keyChanged = modeChanged || outputChanged || width_ != width || height_ != height;

        if (keyChanged) {
            keyValid_ = true;
            stereo_ = input.stereo;
            width_ = width;
            height_ = height;
            gammaExponent_ = input.gammaExponent;
            stereoDepth_ = input.stereoDepth;
            frontValid_ = false;
            frontGeneration_ = 0;
            pendingPair_ = false;
            monoRenderedLastFrame_ = false;
            discardedPairs_ = 0;
        }

        plan.width = width_;
        plan.height = height_;
        plan.frontGeneration = frontGeneration_;
        plan.copyFront = frontValid_;

        if (!stereo_) {
            pendingPair_ = false;
            if (!frontValid_ || !monoRenderedLastFrame_) {
                plan.renderMask = 1u;
                plan.renderGeneration = frontValid_ ? frontGeneration_ : 0u;
                plan.publishRendered = true;
            }
            return plan;
        }

        if (!frontValid_) {
            plan.renderMask = 3u;
            plan.renderGeneration = 0u;
            plan.publishRendered = true;
            return plan;
        }

        const uint32_t backGeneration = 1u - frontGeneration_;
        if (pendingPair_) {
            const bool sameScene = input.simTick == pendingSimTick_ &&
                                   input.presentationEpoch == pendingEpoch_ &&
                                   input.presentationSync == pendingPresentationSync_;
            if (sameScene) {
                plan.renderMask = 2u;
                plan.renderGeneration = pendingGeneration_;
                plan.finishPair = true;
                plan.publishRendered = true;
                plan.freezePresentation = true;
                plan.frozenStep = pendingStep_;
                return plan;
            }
            pendingPair_ = false;
            ++discardedPairs_;
        }

        if (discardedPairs_ >= kMaxDiscardedPairs) {
            plan.renderMask = 3u;
            plan.renderGeneration = backGeneration;
            plan.publishRendered = true;
            return plan;
        }

        plan.renderMask = 1u;
        plan.renderGeneration = backGeneration;
        plan.startPair = true;
        return plan;
    }

    // successfulMask: bit e set when eye e of plan.renderMask was rendered and
    // its GPU work queued.
    void complete(const ScreenPairPlan& plan, uint32_t successfulMask) {
        if (!plan.cacheable) {
            return;
        }
        if (!plan.stereo) {
            if (plan.renderMask == 0u) {
                monoRenderedLastFrame_ = false;
                return;
            }
            if ((successfulMask & plan.renderMask) == plan.renderMask) {
                frontValid_ = true;
                frontGeneration_ = plan.renderGeneration;
                monoRenderedLastFrame_ = true;
            } else {
                monoRenderedLastFrame_ = false;
            }
            return;
        }

        if ((successfulMask & plan.renderMask) != plan.renderMask) {
            if (plan.startPair || plan.finishPair) {
                pendingPair_ = false;
            }
            return;
        }

        if (plan.startPair) {
            pendingPair_ = true;
            pendingGeneration_ = plan.renderGeneration;
            pendingSimTick_ = plan.simTick;
            pendingEpoch_ = plan.presentationEpoch;
            pendingPresentationSync_ = plan.presentationSync;
            pendingStep_ = plan.interpolationStep;
        } else if (plan.publishRendered) {
            frontValid_ = true;
            frontGeneration_ = plan.renderGeneration;
            pendingPair_ = false;
            discardedPairs_ = 0;
        }
    }

    void invalidate() {
        keyValid_ = false;
        frontValid_ = false;
        pendingPair_ = false;
        monoRenderedLastFrame_ = false;
        discardedPairs_ = 0;
        frontGeneration_ = 0;
        width_ = height_ = 0;
    }

    bool frontValid() const { return frontValid_; }
    bool pendingPair() const { return pendingPair_; }
    uint32_t frontGeneration() const { return frontGeneration_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool stereo() const { return stereo_; }

private:
    bool keyValid_ = false;
    bool stereo_ = false;
    bool frontValid_ = false;
    bool pendingPair_ = false;
    bool monoRenderedLastFrame_ = false;
    uint32_t discardedPairs_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frontGeneration_ = 0;
    uint32_t pendingGeneration_ = 0;
    uint64_t pendingSimTick_ = 0;
    uint64_t pendingEpoch_ = 0;
    float pendingStep_ = 1.0f;
    float gammaExponent_ = 1.0f;
    float stereoDepth_ = 1.0f;
    bool pendingPresentationSync_ = false;
};

} // namespace dusk::vr
