// vr_swing_detector.hpp
//
// Motion-controller swing detector for gesture-triggered melee attacks.
//
// This is deliberately engine-agnostic and has no OpenXR types in it -
// convert whatever pose you get from xrLocateSpace() into the small Pose
// struct below and feed it in every frame. It hands you back a single
// discrete "swing triggered" edge that you latch into the same virtual
// input path that keyboard/gamepad/touch already feed into.
//
// Usage sketch (inside your OpenXR frame loop, once per controller):
//
//     vr_combat::SwingDetector rightHandSwing;
//     ...
//     XrSpaceLocation loc = ...; // from xrLocateSpace on the grip pose
//     vr_combat::Pose pose{
//         { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z },
//         predictedDisplayTimeInSeconds
//     };
//     auto event = rightHandSwing.update(pose);
//     if (event.triggered) {
//         // Synthesize the same input bit your existing attack button sets.
//         dusk_input_set_attack_pressed(); // <- replace with the real call
//     }

#pragma once

#include <cmath>
#include <cstdint>

namespace vr_combat {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

struct Pose {
    Vec3 position;        // meters, in whatever tracking space you're using
    double timestampSec;  // seconds, monotonic (e.g. derived from the XR
                           // predicted display time for that frame)
};

enum class SwingDirection {
    None,
    Horizontal,  // side to side
    Vertical,    // overhead / downward chop
    Diagonal,
};

struct SwingEvent {
    bool triggered = false;
    float peakSpeed = 0.0f;  // m/s at the moment of trigger
    SwingDirection direction = SwingDirection::None;
};

// Tunables are public on purpose - you will want to tweak these live
// (e.g. via a debug menu) once you can actually playtest with a headset
// rather than guessing at good values up front.
class SwingDetector {
public:
    float triggerSpeed = 2.5f;       // m/s - must exceed this to fire
    float resetSpeed = 0.8f;         // m/s - must drop below this before
                                      // it's allowed to fire again (hysteresis,
                                      // prevents one swing firing twice)
    float minSwingDistance = 0.15f;  // meters - guards against fast little
                                      // wrist/tracking jitter triggering a swing
    double cooldownSec = 0.12;       // minimum seconds between triggers
    float maxPlausibleSpeed = 15.0f; // m/s - ABOVE this, treat the sample as a
                                      // tracking glitch/teleport rather than a
                                      // real swing and refuse to fire (added
                                      // 2026-08-13 after a real capture logged
                                      // a one-frame 31+ m/s spike -- ~4x any
                                      // genuine swing peak seen across every
                                      // capture so far -- that would otherwise
                                      // fire a false attack)

    // Call once per render frame with the controller's current pose.
    // This does NOT need to run at the game's 30Hz tick rate - sample it
    // as fast as you get fresh tracking data, same as any other analog input.
    SwingEvent update(const Pose& pose) {
        SwingEvent event;

        if (!hasPrev_) {
            prev_ = pose;
            hasPrev_ = true;
            return event;
        }

        const double dt = pose.timestampSec - prev_.timestampSec;
        if (dt <= 0.0) {
            return event;  // stale or duplicate sample, ignore
        }

        const Vec3 delta = pose.position - prev_.position;
        const float dist = delta.length();
        const float speed = static_cast<float>(dist / dt);

        // Track distance traveled since speed first crossed resetSpeed, so
        // a single noisy frame can't fire a swing on its own - there has to
        // be a real, sustained motion behind it.
        if (speed > resetSpeed) {
            if (!inSwingWindow_) {
                inSwingWindow_ = true;
                windowStart_ = prev_.position;
            }
        } else {
            inSwingWindow_ = false;
        }

        const float windowDist =
            inSwingWindow_ ? (pose.position - windowStart_).length() : 0.0f;

        const bool aboveTrigger = speed >= triggerSpeed && speed <= maxPlausibleSpeed &&
                                   windowDist >= minSwingDistance;
        const bool offCooldown =
            (pose.timestampSec - lastTriggerSec_) >= cooldownSec;

        if (aboveTrigger && offCooldown && canFire_) {
            event.triggered = true;
            event.peakSpeed = speed;
            event.direction = classifyDirection(delta);
            lastTriggerSec_ = pose.timestampSec;
            canFire_ = false;  // must drop back below resetSpeed OR reverse
                                // direction (below) to re-arm
            lastFireDelta_ = delta;
            hasLastFireDelta_ = true;
        }

        if (speed <= resetSpeed) {
            canFire_ = true;
        }

        // Direction-reversal re-arm: a fast, tight, CONTINUOUS back-and-forth
        // swing (e.g. spamming left-right as quickly as possible) can keep
        // scalar speed elevated the entire time -- the hand's velocity
        // MAGNITUDE never dips, only its DIRECTION keeps flipping at each
        // end. The resetSpeed check above alone would stay stuck un-armed
        // through an entire such flurry after the first swing (confirmed by
        // a real user report: "spam swing it left and right ... link will
        // just stand there after one swing"). A direction reversal (this
        // frame's motion pointing substantially opposite the motion that
        // produced the LAST trigger) is what actually distinguishes "a new
        // swing" in that case, independent of whether speed ever dropped --
        // re-arm on that signal too. Dot product sign alone (no
        // normalization needed) is enough to detect "opposite general
        // direction"; the subsequent trigger's own speed/distance/cooldown
        // checks still gate whether this re-arm actually produces a new
        // event.
        if (!canFire_ && hasLastFireDelta_ && dist > 0.0f) {
            const float dot = delta.x * lastFireDelta_.x + delta.y * lastFireDelta_.y +
                               delta.z * lastFireDelta_.z;
            if (dot < 0.0f) {
                canFire_ = true;
            }
        }

        prev_ = pose;
        return event;
    }

private:
    static SwingDirection classifyDirection(const Vec3& delta) {
        const float horiz = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        const float vert = std::fabs(delta.y);
        if (vert > horiz * 1.4f) return SwingDirection::Vertical;
        if (horiz > vert * 1.4f) return SwingDirection::Horizontal;
        return SwingDirection::Diagonal;
    }

    Pose prev_{};
    bool hasPrev_ = false;
    bool inSwingWindow_ = false;
    bool canFire_ = true;
    Vec3 windowStart_{};
    double lastTriggerSec_ = -1000.0;
    Vec3 lastFireDelta_{};       // motion direction (unnormalized) at the last
                                  // trigger -- used for direction-reversal re-arm
    bool hasLastFireDelta_ = false;
};

}  // namespace vr_combat
