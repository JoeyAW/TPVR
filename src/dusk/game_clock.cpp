#include "dusk/game_clock.h"

#include <aurora/time.hpp>

#include <algorithm>
#include <chrono>
#include <dusk/interp/frame_interpolation.h>
#include "dusk/vr/vr_main.hpp"

namespace dusk::game_clock {

using native_clock = aurora::time::native_clock;
using game_clock = aurora::time::game_clock;

FrameTiming g_frameTiming{.dt = kUiInitialDt};

namespace {
bool s_initialized = false;
bool s_simTickActive = false;
native_clock::time_point s_previousNativeSample{};
game_clock::time_point s_latestGameSample{};
game_clock::time_point s_currentSnapshotTime{};
game_clock::time_point s_pendingSimTime{};
uint64_t s_presentationEpoch = 1;
bool s_timingModeInitialized = false;
bool s_previousSeparatePresentation = false;
bool s_previousInterpolating = false;
bool s_previousTimeStopped = false;

constexpr game_clock::duration kSimPeriodDuration =
    std::chrono::duration_cast<game_clock::duration>(std::chrono::duration<float>(kSimPeriod));
constexpr native_clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = static_cast<int>(aurora::time::kMaximumTimeScale) * 4;

float ui_dt() {
    if (s_simTickActive) {
        return kSimPeriod;
    }

    const float maximumDt = kUiMaximumDt * aurora::time::scale();
    return std::clamp(g_frameTiming.dt, 0.0f, maximumDt);
}
}  // namespace

void initialize() {
    if (s_initialized) {
        return;
    }
    s_previousNativeSample = native_clock::now();
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample;
    s_pendingSimTime = s_latestGameSample;
    s_initialized = true;
}

void reset() {
    s_previousNativeSample = native_clock::now();
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample - kSimPeriodDuration;
    s_pendingSimTime = s_currentSnapshotTime;
    s_simTickActive = false;
    ++s_presentationEpoch;
}

void set_sim_rate(float hz) {
    const float maximumHz = aurora::time::kMaximumTimeScale / kSimPeriod;
    aurora::time::set_scale(std::clamp(hz, 1.0f, maximumHz) * kSimPeriod);
    reset();
}

float get_sim_rate() {
    return aurora::time::scale() / kSimPeriod;
}

const FrameTiming& advance() {
    const auto nativeNow = native_clock::now();
    const auto gameNow = game_clock::now();
    const auto nativeFrameGap = nativeNow - s_previousNativeSample;
    const auto gameFrameGap = gameNow - s_latestGameSample;
    s_previousNativeSample = nativeNow;
    s_latestGameSample = gameNow;

    auto& out = g_frameTiming;
    out = {
        .dt = std::chrono::duration<float>(gameFrameGap).count(),
        .presentationEpoch = s_presentationEpoch,
    };

    // VR real per-eye rendering ONLY ever runs from inside the interpolating
    // branch of m_Do_main.cpp's main loop (dusk::vr::tick() is called there,
    // nowhere else) -- on a genuinely fresh config with no config.json yet,
    // enableFrameInterpolation defaults to Off, which used to mean this whole
    // branch (and therefore all real VR rendering) never ran at all: the game
    // played completely normally in the background, but the headset just sat
    // on the runtime's own loading splash forever. Force interpolation on
    // whenever a VR session is active, independent of the config default, so
    // a fresh install doesn't need a manual config.json edit to ever render
    // anything in the headset.
    const float timeScale = aurora::time::scale();
    const bool interpolating = dusk::getSettings().game.enableFrameInterpolation.getValue() !=
                                    dusk::FrameInterpMode::Off ||
                                dusk::vr::isActive();
    const bool separatePresentation = interpolating || timeScale != 1.0f;
    out.interpolating = interpolating;
    out.separatePresentation = separatePresentation;

    const bool timeStopped = timeScale == 0.0f;
    const bool timingModeChanged =
        s_timingModeInitialized &&
        (separatePresentation != s_previousSeparatePresentation ||
            interpolating != s_previousInterpolating || timeStopped != s_previousTimeStopped);
    const bool abnormalGap = nativeFrameGap > kAbnormalGapResetThreshold;
    if (timingModeChanged || abnormalGap) {
        ++s_presentationEpoch;
        out.presentationEpoch = s_presentationEpoch;
    }
    s_timingModeInitialized = true;
    s_previousSeparatePresentation = separatePresentation;
    s_previousInterpolating = interpolating;
    s_previousTimeStopped = timeStopped;

    if (!separatePresentation) {
        s_currentSnapshotTime = gameNow;
        out.numSimTicks = 1;
        return out;
    }

    const auto simulationTarget = interpolating ? gameNow - kSimPeriodDuration : gameNow;
    if (timeStopped || abnormalGap) {
        s_currentSnapshotTime = simulationTarget;
        out.numSimTicks = 0;
        return out;
    }

    int numSimTicks = 0;
    auto projectedSnapshotTime = s_currentSnapshotTime;
    while (numSimTicks < kMaxSimTicksPerFrame) {
        const bool tickDue = interpolating ?
                                 projectedSnapshotTime < simulationTarget :
                                 projectedSnapshotTime + kSimPeriodDuration <= simulationTarget;
        if (!tickDue) {
            break;
        }
        projectedSnapshotTime += kSimPeriodDuration;
        numSimTicks++;
    }
    out.numSimTicks = numSimTicks;
    return out;
}

void begin_sim_tick() {
    s_pendingSimTime = g_frameTiming.separatePresentation ? s_currentSnapshotTime + kSimPeriodDuration :
                                                            s_latestGameSample;
    s_simTickActive = true;
}

void commit_sim_tick() {
    if (s_simTickActive) {
        s_currentSnapshotTime = s_pendingSimTime;
        s_simTickActive = false;
    } else {
        s_currentSnapshotTime += kSimPeriodDuration;
    }
}

bool is_sim_frame() {
    return !g_frameTiming.separatePresentation || s_simTickActive;
}

bool is_presentation_frame() {
    return !g_frameTiming.separatePresentation || !s_simTickActive;
}

float sample_interpolation_step() {
    const float step =
        std::chrono::duration<float>(game_clock::now() - s_currentSnapshotTime).count() /
        kSimPeriod;
    return std::clamp(step, 0.0f, 1.0f);
}

double sample_time() {
    const auto now = s_simTickActive ? s_pendingSimTime : game_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

float original_frames() {
    return ui_dt() / kSimPeriod;
}

float consume_interval(double& lastSample) {
    const double now = sample_time();
    const float dt = std::max(0.0, now - lastSample);
    lastSample = now;
    return dt;
}

}  // namespace dusk::game_clock
