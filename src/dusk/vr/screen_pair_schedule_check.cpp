// Host-runnable behaviour check for screen_pair_schedule.hpp (no game/GPU deps):
//   c++ -std=c++17 src/dusk/vr/screen_pair_schedule_check.cpp -o /tmp/screen_pair_check && /tmp/screen_pair_check
// Models the Session buffers as scene ids per generation/eye and the acquired
// swapchain image as what the front copies put into it each refresh.
#undef NDEBUG
#include "screen_pair_schedule.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>

using dusk::vr::ScreenPairInput;
using dusk::vr::ScreenPairPlan;
using dusk::vr::ScreenPairScheduler;

namespace {

// Scene an eye shows: sim state + presentation epoch/sync + interpolation step.
uint64_t sceneId(const ScreenPairInput& in, float step) {
    uint32_t stepBits = 0;
    std::memcpy(&stepBits, &step, sizeof(stepBits));
    return (in.simTick << 40) ^ (in.presentationEpoch << 32) ^ (uint64_t{in.presentationSync} << 31) ^ stepBits;
}

struct Gpu {
    uint64_t buffer[2][2] = {};  // [generation][eye] -> scene id last computed there
};

struct Frame {
    ScreenPairPlan plan;
    uint32_t rendered = 0;   // eyes rendered this refresh
    bool published = false;  // quads submitted over a complete image set
    uint64_t left = 0;
    uint64_t right = 0;
};

Frame refresh(ScreenPairScheduler& s, Gpu& gpu, const ScreenPairInput& in, uint32_t failMask = 0) {
    Frame f;
    f.plan = s.begin(in);
    uint32_t ok = 0;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const uint32_t bit = 1u << eye;
        if ((f.plan.renderMask & bit) == 0) {
            continue;
        }
        ++f.rendered;
        if (failMask & bit) {
            continue;
        }
        // A frozen eye re-presents at the saved step, but it can only show the
        // CURRENT sim state -- a pair finished across a sim tick would mismatch.
        const float step = f.plan.freezePresentation ? f.plan.frozenStep : in.interpolationStep;
        gpu.buffer[f.plan.cacheable ? f.plan.renderGeneration : 0][eye] = sceneId(in, step);
        ok |= bit;
    }
    s.complete(f.plan, ok);
    if (f.plan.cacheable && s.frontValid()) {
        // Every refresh copies the whole front set into the newly acquired image.
        f.published = true;
        f.left = gpu.buffer[s.frontGeneration()][0];
        f.right = s.stereo() ? gpu.buffer[s.frontGeneration()][1] : f.left;
    }
    return f;
}

ScreenPairInput stereoInput() {
    ScreenPairInput in;
    in.cacheable = true;
    in.stereo = true;
    in.width = 1280;
    in.height = 720;
    in.simTick = 10;
    in.presentationEpoch = 4;
    in.interpolationStep = 0.25f;
    return in;
}

void checkWarmupAndAlternation() {
    ScreenPairScheduler s;
    Gpu gpu;
    ScreenPairInput in = stereoInput();

    // Cold start: both eyes in one refresh, published immediately.
    Frame f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront && f.published);
    assert(f.left == f.right && f.left == sceneId(in, 0.25f));

    // 72 Hz display / 30 Hz sim: a sim tick every 2-3 refreshes. Steady state
    // renders exactly one eye per refresh and always submits a matched pair.
    const bool tickAt[] = {true, false, false, true, false, true, false, false, true, false,
                           true, false, false, true, false, true, false, false, true, false};
    uint64_t lastPair = f.left;
    int flips = 0;
    for (int r = 0; r < 20; ++r) {
        if (tickAt[r]) {
            ++in.simTick;
        }
        in.interpolationStep = 0.05f * static_cast<float>(r % 7);
        f = refresh(s, gpu, in);
        assert(f.rendered == 1u);
        assert(f.published && f.left == f.right);
        if (f.plan.freezePresentation) {
            assert(f.plan.renderMask == 2u);
        }
        if (f.left != lastPair) {
            ++flips;
            lastPair = f.left;
        }
    }
    assert(flips >= 8);
}

void checkSceneChangeBetweenEyes() {
    ScreenPairScheduler s;
    Gpu gpu;
    ScreenPairInput in = stereoInput();
    refresh(s, gpu, in);  // warm
    const uint64_t warmPair = gpu.buffer[s.frontGeneration()][0];

    // Eye 0 of a new pair, then a sim tick: the partial pair is discarded and the
    // old complete pair stays on screen.
    ++in.simTick;
    Frame f = refresh(s, gpu, in);
    assert(f.plan.startPair && f.left == warmPair && f.right == warmPair);
    ++in.simTick;
    f = refresh(s, gpu, in);
    assert(f.plan.startPair && !f.plan.freezePresentation);
    assert(f.left == warmPair && f.right == warmPair);

    // Epoch and sync changes are scene changes too.
    ++in.presentationEpoch;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && f.published && f.left == f.right);  // 2nd discard in a row: whole pair
    const uint64_t caughtUp = f.left;
    assert(caughtUp == sceneId(in, in.interpolationStep));

    f = refresh(s, gpu, in);
    assert(f.plan.startPair);
    in.presentationSync = true;
    f = refresh(s, gpu, in);
    assert(f.plan.startPair && f.left == caughtUp && f.right == caughtUp);

    // Second eye with no scene change: frozen at eye 0's step, then published.
    const float firstStep = in.interpolationStep;
    in.interpolationStep = 0.9f;
    f = refresh(s, gpu, in);
    assert(f.plan.freezePresentation && f.plan.frozenStep == firstStep);
    assert(f.published && f.left == f.right && f.left != caughtUp);
}

void checkStarvationGuard() {
    // Sim ticks between every pair of refreshes: split pairs never complete, so
    // a whole pair is rendered after kMaxDiscardedPairs discards.
    ScreenPairScheduler s;
    Gpu gpu;
    ScreenPairInput in = stereoInput();
    refresh(s, gpu, in);
    int sinceFlip = 0;
    uint64_t last = gpu.buffer[s.frontGeneration()][0];
    for (int r = 0; r < 30; ++r) {
        ++in.simTick;
        const Frame f = refresh(s, gpu, in);
        assert(f.published && f.left == f.right);
        assert(f.rendered <= 2u);
        if (f.left != last) {
            last = f.left;
            sinceFlip = 0;
        } else {
            ++sinceFlip;
        }
        assert(sinceFlip <= static_cast<int>(ScreenPairScheduler::kMaxDiscardedPairs));
    }
}

void checkTransitionsAndFailures() {
    ScreenPairScheduler s;
    Gpu gpu;
    ScreenPairInput in = stereoInput();

    // Partial warm-up failure publishes nothing.
    Frame f = refresh(s, gpu, in, 2u);
    assert(!f.published);
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && f.published);
    const uint64_t front = f.left;

    // Second-eye failure keeps the complete front pair.
    ++in.simTick;
    f = refresh(s, gpu, in);
    f = refresh(s, gpu, in, 2u);
    assert(f.plan.finishPair && f.published && f.left == front && f.right == front);

    // A resize requested mid-pair waits for that pair, then warms up at the new size.
    f = refresh(s, gpu, in);
    assert(f.plan.startPair);
    in.width = 1088;
    in.height = 612;
    f = refresh(s, gpu, in);
    assert(f.plan.finishPair && f.plan.width == 1280u && f.published && f.left == f.right);
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront && f.plan.width == 1088u && f.published);

    // Gamma / stereo depth baked into cached bytes: warm-up.
    in.gammaExponent = 2.0f;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront);
    in.stereoDepth = 1.5f;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront);

    // Mono: renders every other refresh, every refresh still submits the image.
    in.stereo = false;
    in.width = 1600;
    in.height = 900;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 1u && !f.plan.copyFront && f.published);
    for (int r = 0; r < 6; ++r) {
        ++in.simTick;
        f = refresh(s, gpu, in);
        assert(f.published);
        assert(f.rendered == ((r % 2 == 0) ? 0u : 1u));
    }

    // Back to stereo: cold warm-up, never a mono image as a stereo pair.
    in.stereo = true;
    in.width = 1280;
    in.height = 720;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront && f.published && f.left == f.right);

    // Losing the cacheable transport falls back to full rendering and drops the cache.
    in.cacheable = false;
    f = refresh(s, gpu, in);
    assert(!f.plan.cacheable && f.plan.renderMask == 3u && !s.frontValid());
    in.cacheable = true;
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront);

    // Explicit invalidation (screen mode off / copy failure) also re-warms.
    s.invalidate();
    f = refresh(s, gpu, in);
    assert(f.plan.renderMask == 3u && !f.plan.copyFront);
}

}  // namespace

int main() {
    checkWarmupAndAlternation();
    checkSceneChangeBetweenEyes();
    checkStarvationGuard();
    checkTransitionsAndFailures();
    return 0;
}
