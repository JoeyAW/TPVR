#pragma once

// vr_menu_gamepad.hpp
//
// Lets VR controllers open and navigate the Dusklight menu (the RmlUi
// settings/mods overlay, normally opened with F1 or a real gamepad's
// configured button) -- see the approved plan this was built from,
// C:\Users\joeyw\.claude\plans\fizzy-finding-minsky.md, and the
// vr-mod-notes skill for the full research trail.
//
// WHY THIS EXISTS / WHY NOT PADSetVirtualStatus OR ActionBinds' virtual
// binds (both already used elsewhere for VR/touch input): confirmed by
// direct code reading that the Dusklight menu's open/navigate logic
// (chord detection, hold-repeat navigation, the Controller Config
// rebinding screen) is driven ENTIRELY by real SDL SDL_EVENT_GAMEPAD_*
// events flowing through dusk::ui::input::handle_event()
// (src/dusk/ui/input.cpp) -- neither PADSetVirtualStatus (gameplay input)
// nor dusk::setVirtualActionBind (touch-button shortcuts) reach that code
// path at all. Rather than duplicate that pipeline's chord/repeat/rebind
// logic by hand, this registers a REAL SDL3 virtual gamepad
// (SDL_AttachVirtualJoystick) and drives it from VR controller state every
// frame -- SDL synthesizes genuine SDL_EVENT_GAMEPAD_* events from that,
// which flow through the EXISTING, UNMODIFIED menu pipeline for free,
// including the existing per-button rebinding UI. SDL's own header comment
// for this API literally calls out this exact use case: "This has been
// used to make unusual devices, like VR headset controllers, look like
// normal joysticks." First use of this SDL3 API anywhere in this codebase
// -- confirmed via a full-repo grep before choosing this approach.
//
// PORT CHOICE: PAD_CHAN1 (Port 2), deliberately NOT PAD_CHAN0 (the port VR
// gameplay input already injects into via PADSetVirtualStatus). Confirmed
// by reading dusk::ui::input::sync_input_block() (input.cpp) that
// PADBlockInput(any_document_visible()) already zeroes ALL ports' gameplay
// input while any UI document (including this menu) is visible -- so
// sharing PAD_CHAN0 would have been *safe*, but PAD_CHAN1 is strictly
// safer: in a normal (non-DEBUG, non-developmentMode) build,
// mDoCPd_c::create() never even allocates a JUTGamePad for ports 1-3, so
// gameplay never reads that port's status AT ALL, menu open or not. Also
// avoids any interaction with PAD_CHAN0's saved button-mapping profile or
// a real second physical controller ever competing for player-index 0.
//
// SCOPE: input only (this is Phase 1 of the approved plan). Separately
// confirmed (reading extern/aurora/lib/aurora.cpp's end_frame()) that
// RmlUi always composites onto the DESKTOP WINDOW's own surface, never
// into a VR eye or the existing HUD billboard -- so after this lands,
// opening the menu via controller will show it on the monitor, but the
// headset itself stays blank. Making it visible IN the headset is a
// separate, not-yet-attempted follow-up (a new VR billboard capturing
// RmlUi's own render target, similar in kind to the existing desktop-
// mirror feature) -- deliberately not attempted here.

#include <windows.h>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <dolphin/pad.h>  // PAD_CHAN1

namespace dusk::vr {

// See the port-choice comment above.
inline constexpr u32 kVrMenuGamepadPort = PAD_CHAN1;

namespace detail {

// 0 is SDL_JoystickID's documented "invalid" sentinel -- doubles as our
// "not attached" state, so no separate bool is needed.
inline SDL_JoystickID g_vrMenuGamepadInstanceId = 0;

// SDL_AttachVirtualJoystick's own Cleanup callback (SDL_VirtualJoystickDesc
// ::Cleanup) fires on detach -- used here only to log, not for any real
// state cleanup (g_vrMenuGamepadInstanceId is reset by detachVrMenuGamepad()
// itself, not from this callback, since userdata round-tripping through a
// C function pointer is more indirection than this needs for one log line).
inline void SDLCALL vrMenuGamepadCleanup(void* /*userdata*/) {
    OutputDebugStringA("[dusk::vr::menugamepad] virtual joystick Cleanup callback fired\n");
}

}  // namespace detail

// Idempotent -- safe to call every time VR startup succeeds even though
// it's currently only called once (see vr_main.cpp's startup()). Returns
// false (and logs) if SDL_AttachVirtualJoystick itself fails; does not
// retry on its own -- this project's established pattern is to fix a
// real failure via a rebuild, not silently retry-loop around one.
inline bool ensureVrMenuGamepadAttached() {
    if (detail::g_vrMenuGamepadInstanceId != 0) {
        return true;
    }

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = "Dusklight VR Menu Controller";
    // ALL standard buttons/axes advertised as valid -- NOT the sparse
    // "only the six we actually drive" mask this originally had. Found via
    // real in-headset diagnostics (round-trip + live event logging, see
    // vr-mod-notes skill's "hold Start+RightTrigger doesn't open the menu"
    // investigation) that SDL's auto-generated mapping for a virtual
    // SDL_JOYSTICK_TYPE_GAMEPAD does NOT identity-map raw index == enum
    // value the way this project first assumed (and the header docs alone
    // don't spell out) -- it appears to COMPACT raw indices to the
    // ascending-enum-order position among only the bits actually SET in
    // button_mask/axis_mask. With a sparse mask (SOUTH=0, EAST=1, START=6),
    // SOUTH/EAST's compacted position happened to coincidentally equal
    // their own enum value (both are low bits), which is exactly why the
    // original SOUTH-only round-trip test passed and looked like proof of
    // identity mapping -- but START's real compacted position would have
    // been 2, not 6, so writes via SDL_GAMEPAD_BUTTON_START (enum value 6)
    // silently landed on a raw slot nothing was listening to. Confirmed
    // directly: a trigger round-trip test (write raw 32767 to
    // SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, enum value 5) read back as 0 through
    // SDL_GetGamepadAxis(), and zero real SDL_EVENT_GAMEPAD_BUTTON_DOWN
    // events for START ever appeared in a live capture despite the user
    // holding it repeatedly -- both symptoms this exact mismatch predicts.
    // Fix: set every bit from 0 up through the highest index this module
    // ever touches, for both buttons and axes, so there are no gaps below
    // anything we use -- this makes compacted position equal raw enum
    // value unconditionally, regardless of exactly how SDL's real
    // compaction algorithm works internally (never confirmed beyond this
    // empirical fix, and not worth further reverse-engineering an
    // undocumented internal behavior). naxes/nbuttons already use the full
    // enum count, so there's no separate array-sizing concern here.
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.Cleanup = &detail::vrMenuGamepadCleanup;

    const SDL_JoystickID instance = SDL_AttachVirtualJoystick(&desc);
    if (instance == 0) {
        char msg[256];
        _snprintf_s(msg, _TRUNCATE,
                    "[dusk::vr::menugamepad] SDL_AttachVirtualJoystick FAILED: %s\n",
                    SDL_GetError());
        OutputDebugStringA(msg);
        return false;
    }

    detail::g_vrMenuGamepadInstanceId = instance;
    char msg[128];
    _snprintf_s(msg, _TRUNCATE,
                "[dusk::vr::menugamepad] attached, instance id=%u\n",
                static_cast<unsigned>(instance));
    OutputDebugStringA(msg);
    return true;
}

// Call once per real frame. A freshly-attached virtual joystick has no OS
// player slot, so nothing assigns it a port automatically -- and
// aurora::input::apply_port_preferences() (extern/aurora/lib/input.cpp)
// can reset an explicit port assignment back to -1 later if the player has
// ever configured a port preference for Port 2, so this re-asserts rather
// than claiming the port just once. No-ops gracefully (retries next frame)
// if SDL hasn't finished opening the device as a gamepad yet -- aurora's
// own SDL_EVENT_GAMEPAD_ADDED handler (window.cpp's process_event() ->
// aurora::input::add_controller()) does that asynchronously, at most one
// frame after ensureVrMenuGamepadAttached() runs.
inline void updateVrMenuGamepadPlayerIndex() {
    if (detail::g_vrMenuGamepadInstanceId == 0) {
        return;
    }
    SDL_Gamepad* gamepad = SDL_GetGamepadFromID(detail::g_vrMenuGamepadInstanceId);
    if (gamepad == nullptr) {
        return;
    }
    if (SDL_GetGamepadPlayerIndex(gamepad) != static_cast<int>(kVrMenuGamepadPort)) {
        SDL_SetGamepadPlayerIndex(gamepad, static_cast<int>(kVrMenuGamepadPort));
    }
}

// Cooldown for the menu-open chord -- added per explicit user request
// ("add a cooldown to the button press... half a second cause it kinda
// spams") after confirming the menu itself opens correctly: holding the
// chord physically continuously was re-triggering open/close repeatedly,
// most likely because dusk::ui::input.cpp's own chord-release detection
// (is_menu_chord()'s AND of two separately-tracked inputs, each with its
// own press/release threshold) doesn't see one clean continuous hold from
// an analog trigger value/OpenXR button read the way it would from a
// real controller's more binary-feeling digital press. Once the chord is
// let through once, this forces it fully released (both inputs, not just
// one -- releasing only one wouldn't drop is_menu_chord()'s AND) for
// kMenuChordCooldownSec regardless of physical hold state, so at most one
// open/close toggle can happen per cooldown window.
namespace detail {
inline float g_menuChordCooldownRemaining = 0.f;
}  // namespace detail
inline constexpr float kMenuChordCooldownSec = 0.5f;

// Hold-to-open requirement -- added per explicit user request ("make it so
// you have to hold the menu opening hotkey for 1 second instead of just
// pressing it"). Tracks how long the chord has been held CONTINUOUSLY
// (kMenuChordHoldToOpenSec, real wall-clock seconds via dtSeconds) before
// it's allowed to reach the virtual gamepad at all -- below that duration
// the chord is forced fully released (both inputs, same "AND needs both
// dropped" reasoning as the cooldown above) regardless of physical hold
// state, exactly like the cooldown's own forced-release shape. Resets to 0
// the instant the chord is physically released, so a quick tap (or several
// quick taps) can never accidentally open the menu -- only one continuous
// >=1s hold does. Deliberately independent of, and evaluated before, the
// cooldown above: once a hold has been long enough to actually open the
// menu, continuing to hold it still lets the pre-existing cooldown govern
// any further open/close toggling every kMenuChordCooldownSec, unchanged.
namespace detail {
inline float g_menuChordHoldElapsed = 0.f;
}  // namespace detail
inline constexpr float kMenuChordHoldToOpenSec = 1.0f;

// Left-stick handling -- history of this whole block, oldest first (kept
// for context; skip to "CURRENT DESIGN (v7)" below for what's actually
// live):
//
// v1: added per user report ("navigation works, however it is insanely
// fast unlike a regular controller"). Diagnosed as VR hand tremor
// flickering the raw axis back and forth across dusk/ui/input.cpp's
// press/release band several times a second, each crossing firing an
// immediate, un-ramped FRESH press. Fixed with an exponential low-pass
// filter on the raw stick value (0.08s time constant) -- CONFIRMED
// WORKING at the time, per the user.
//
// v2/v3: retuned that same filter's time constant (0.16, then 0.35)
// chasing later "wayy too fast" / "instant full-speed jump" reports.
//
// v4: dropped the filter entirely for a fixed-cadence PULSE GATE (force a
// real release-then-repress of the axis at a chosen interval, so
// dusk/ui/input.cpp's own accelerating repeat ramp -- kGamepadRepeatStartInterval
// 0.12s down to kGamepadRepeatMinInterval 0.045s/~22Hz over a 1s hold --
// never gets the chance to engage at all). Verified mechanically correct
// via two real diagnostic captures, including finding and fixing a real
// hysteresis bug in the deadzone check (kMenuStickReleaseThreshold,
// below) that had been causing a single tap to fire many presses via
// frame-rate flicker right at the threshold.
//
// v5: reverted ALL the way back to v1's plain filter, per user feedback
// that the ORIGINAL filter was the confirmed-good baseline and a later
// architecture bug (see "TAKE 2" below) was what actually broke it, not
// the filter's own inherent behavior. Built and asked for a retest --
// STILL reported "too fast / spams through entries" even at the exact
// original 0.08s constant, running through the corrected architecture.
//
// v6: took v5's result as proof the filter itself is the wrong tool
// (it only ever controls the FIRST press's rise time, never the ONGOING
// rate once a real sustained hold lets dusk/ui/input.cpp's own repeat
// ramp take over) and restored v4's pulse gate (with its confirmed
// hysteresis fix) at a much slower cadence (0.6s/move). STILL reported
// "not fixed."
//
// Six rounds in, the user asked the right question directly: just
// restore the code to literally before the stuck-right/visibility-gating
// fix, since THAT exact code (double-blend-per-frame smoothing at 0.08s,
// fed the raw stick UNCONDITIONALLY regardless of menu visibility) was
// the last known-good state. Rather than keep trying to derive a
// "better" or "more correct" mechanism that reproduces the same feel
// (v2 through v6 all failed to), just reproduce that state exactly --
// including its double-application-via-neutralize quirk, which earlier
// analysis showed gives measurably MORE damping than a single clean pass
// at the same nominal time constant (probably part of why 0.08 felt
// right in the first place).
//
// CURRENT DESIGN (v7): v1's exact smoothing filter and its double-per-
// frame application via neutralizeVrMenuGamepadState() are BOTH restored
// verbatim -- see advanceMenuStickSmoothing() below, called from BOTH
// updateVrMenuGamepadState() (with the real stick value) and
// neutralizeVrMenuGamepadState() (with (0,0), exactly like the original
// architecture, before the TAKE-2 split ever touched this accumulator).
// This is deliberately NOT "corrected" back to a single clean
// application -- that correction is what v2 was already built on top of,
// and per the user's own A/B report, that's what broke the feel to begin
// with. The stuck-right bug is fixed WITHOUT touching any of this
// per-frame dynamic at all: resetMenuStickSmoothingToZero() is called
// exactly once, directly from vr_main.cpp's tick() (not from anywhere in
// this header), on the real menu-closed->open transition -- a plain
// one-shot hard reset that can't perturb the restored original smoothing
// behavior on any OTHER frame, unlike every earlier attempt at fixing
// stuck-right, which all gated or reset the accumulator from INSIDE the
// shared per-frame path and kept interacting badly with it. The deadzone
// (kMenuStickDeadzone) is kept, applied before smoothing -- a separate,
// independently-confirmed-good fix for a different symptom (tiny pushes
// registering as full deflection), unrelated to any of this speed
// history and safe here since it feeds a continuous filter, not a binary
// state machine.
namespace detail {
inline float g_leftStickSmoothedX = 0.f;
inline float g_leftStickSmoothedY = 0.f;
}  // namespace detail
inline constexpr float kMenuStickSmoothingTimeConstant = 0.08f;  // seconds -- ORIGINAL confirmed-good value
inline constexpr float kMenuStickDeadzone = 0.2f;

// Advances the smoothing accumulator by one dtSeconds step toward
// (leftStickX, leftStickY) and returns the current smoothed value. Safe,
// BY DESIGN, to call from both the real per-frame site AND
// neutralizeVrMenuGamepadState() every single frame -- unlike
// computeMenuChordGate()'s hold-elapsed timer (see the TAKE 2 comment
// below), this accumulator doesn't need to accumulate TOWARD a threshold
// over many frames to do its job, so double-applying it every frame
// (once toward (0,0) from neutralize, once toward the real value from
// the real call) is a deliberate, reproduced-on-purpose behavior here,
// not a bug -- see the v7 history note above for why.
inline void advanceMenuStickSmoothing(float leftStickX, float leftStickY, float dtSeconds,
                                       float& outLeftStickX, float& outLeftStickY) {
    if (std::abs(leftStickX) < kMenuStickDeadzone) leftStickX = 0.f;
    if (std::abs(leftStickY) < kMenuStickDeadzone) leftStickY = 0.f;

    const float smoothingAlpha = 1.f - std::exp(-dtSeconds / kMenuStickSmoothingTimeConstant);
    detail::g_leftStickSmoothedX += (leftStickX - detail::g_leftStickSmoothedX) * smoothingAlpha;
    detail::g_leftStickSmoothedY += (leftStickY - detail::g_leftStickSmoothedY) * smoothingAlpha;
    outLeftStickX = detail::g_leftStickSmoothedX;
    outLeftStickY = detail::g_leftStickSmoothedY;
}

// Fixes the stuck-right bug: call once, directly from vr_main.cpp's
// tick(), on the real menu-closed->open transition (tracked via a
// function-local static IN tick() itself, never touched by
// neutralizeVrMenuGamepadState() -- see the v7 history note above for
// why that isolation is exactly the point). Hard-zeroes the accumulator
// so a freshly-opened menu never inherits whatever the player's live
// GAMEPLAY stick position had smoothed to while the menu was closed.
inline void resetMenuStickSmoothingToZero() {
    detail::g_leftStickSmoothedX = 0.f;
    detail::g_leftStickSmoothedY = 0.f;
}

// BUG FIX (TAKE 2): the CHORD gate's hold-to-open/cooldown STATE (below,
// in computeMenuChordGate) is cross-frame in a way that DOES require
// isolation from neutralizeVrMenuGamepadState() -- it has to accumulate
// TOWARD a threshold (kMenuChordHoldToOpenSec) over many consecutive real
// frames, which the stick smoothing accumulator above does not (see its
// own comment for why double-applying IT every frame is fine/intentional
// here, unlike this timer). computeMenuChordGate() is called from
// exactly ONE place: updateVrMenuGamepadState(), itself called from
// exactly one real per-frame site in vr_main.cpp. It must NEVER be
// reachable from neutralizeVrMenuGamepadState() -- that function is called
// UNCONDITIONALLY at the very top of every tick(), before the real
// per-frame update runs, as a "reset up front in case of an early return"
// safety net (see tick()'s own comment). A first version of this hold-to-
// open feature ran the hold-elapsed timer inside the SAME function both
// call sites shared, using a physicallyHeld=false reading on the
// neutralize call to reset it -- which, since neutralize() runs on
// literally every frame (not just ones that actually early-return), wiped
// g_menuChordHoldElapsed back to 0 immediately before the real call each
// frame ran, capping it at a single frame's dt forever -- confirmed
// in-headset as "pressing and holding the right stick and trigger doesn't
// open the menu at all" (it can never reach kMenuChordHoldToOpenSec). The
// fix is this split: writeVrMenuGamepadOutput() (further down) owns NO
// cross-frame state at all -- it just stages already-resolved values onto
// the SDL joystick -- so neutralizeVrMenuGamepadState() can call THAT
// directly every frame with all-neutral values, without ever touching the
// chord timer that only the real per-frame path is allowed to advance.

// Computes the gated (hold-to-open, then cooldown) menu-chord output. MUST
// be called exactly once per REAL frame (see the TAKE 2 comment above) --
// never from neutralizeVrMenuGamepadState(), or the hold-elapsed timer can
// never accumulate past a single frame's dt (this was the actual bug --
// see the TAKE 2 comment above for the full story).
inline void computeMenuChordGate(bool menuChordHeld, float triggerChordValue, float dtSeconds,
                                  bool& outMenuChordHeld, float& outTriggerChordValue) {
    // Hold-to-open gate -- see kMenuChordHoldToOpenSec's own comment.
    // Evaluated BEFORE the cooldown block below: until the chord has been
    // held continuously for long enough, it's forced fully released here,
    // which also naturally keeps the cooldown block's own trigger
    // condition (right below) from ever seeing it as "about to fire."
    const bool physicallyHeld = menuChordHeld && triggerChordValue > 0.5f;
    if (physicallyHeld) {
        detail::g_menuChordHoldElapsed += dtSeconds;
    } else {
        detail::g_menuChordHoldElapsed = 0.f;
    }

    outMenuChordHeld = physicallyHeld && detail::g_menuChordHoldElapsed >= kMenuChordHoldToOpenSec;
    outTriggerChordValue = outMenuChordHeld ? triggerChordValue : 0.f;

    if (detail::g_menuChordCooldownRemaining > 0.f) {
        detail::g_menuChordCooldownRemaining -= dtSeconds;
        outMenuChordHeld = false;
        outTriggerChordValue = 0.f;
    } else if (outMenuChordHeld && outTriggerChordValue > 0.5f) {
        // 0.5 roughly matches dusk/ui/input.cpp's own kGamepadAxisPressThreshold
        // (16384 of 32767) -- only start the cooldown once the chord is
        // actually about to fire, not on every frame it's merely trending
        // upward.
        detail::g_menuChordCooldownRemaining = kMenuChordCooldownSec;
    }
}

// Writes already-fully-resolved values straight onto the virtual
// joystick's SDL state and flushes them via SDL_UpdateJoysticks(). Owns NO
// cross-frame state of its own -- safe to call from both
// updateVrMenuGamepadState() (real per-frame path, after the compute
// functions above have resolved gated/smoothed values) and
// neutralizeVrMenuGamepadState() (the unconditional early-return safety
// net) without either one corrupting the other's timers. Values use SDL's
// OWN ranges, not PADStatus's s8 scale: sticks are Sint16
// (SDL_JOYSTICK_AXIS_MIN..MAX, i.e. -32768..32767), the trigger axis is
// 0..SDL_JOYSTICK_AXIS_MAX (SDL_gamepad.h's own doc: "Trigger axis values
// range from 0 (released) to SDL_JOYSTICK_AXIS_MAX (fully pressed)").
// No-ops gracefully if the joystick isn't open yet, same reasoning as
// updateVrMenuGamepadPlayerIndex() above.
inline void writeVrMenuGamepadOutput(float leftStickX, float leftStickY, bool rightAHeld,
                                      bool rightBHeld, bool menuChordHeld, float triggerChordValue) {
    if (detail::g_vrMenuGamepadInstanceId == 0) {
        return;
    }
    SDL_Joystick* joystick = SDL_GetJoystickFromID(detail::g_vrMenuGamepadInstanceId);
    if (joystick == nullptr) {
        return;
    }

    const auto clamp01 = [](float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); };
    const auto clampSigned = [](float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); };
    const auto toSignedAxis = [&](float v) {
        return static_cast<Sint16>(clampSigned(v) * 32767.f);
    };
    const auto toTriggerAxis = [&](float v) {
        return static_cast<Sint16>(clamp01(v) * 32767.f);
    };

    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, toSignedAxis(leftStickX));
    // SDL's Y+ is down (matches PADStatus's own stickY convention already
    // negated at every other VR->PAD call site in vr_main.cpp) -- negate
    // here so pushing the stick up navigates up, not down.
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, toSignedAxis(-leftStickY));
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                toTriggerAxis(triggerChordValue));
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, rightAHeld);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, rightBHeld);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_START, menuChordHeld);

    // REQUIRED, not optional (found via the round-trip diagnostic's own
    // first-attempt false negative, see this module's own history): every
    // SDL_SetJoystickVirtual* call above only stages a value -- it has no
    // effect on anything that reads gamepad/joystick state (including the
    // SDL_EVENT_GAMEPAD_* events dusk::ui::input::handle_event() needs)
    // until the next SDL_UpdateJoysticks. Calling it explicitly here,
    // right after every frame's writes, means our state is live and its
    // events are already queued by the time aurora's own poll_events()
    // (extern/aurora/lib/window.cpp) next runs its SDL_PollEvent loop --
    // whether that's later this same frame or next frame, rather than
    // leaving it to chance.
    SDL_UpdateJoysticks();
}

// Call once per real frame with whatever this frame's VR controller state
// already computed for gameplay (see vr_main.cpp's tick() -- these are ALL
// already-read locals, no new OpenXR action reads needed). leftStickX/Y
// are fed UNCONDITIONALLY -- see advanceMenuStickSmoothing()'s own
// comment for why this call site deliberately does NOT gate them by menu
// visibility (that's handled separately, via resetMenuStickSmoothingToZero()
// at the real transition, called directly from vr_main.cpp). dtSeconds
// drives the menu-chord cooldown/hold-to-open timer and the stick
// smoothing filter -- pass pacing.presentation_dt_seconds (real measured
// frame time), same source every other per-frame timer in this codebase
// uses.
inline void updateVrMenuGamepadState(float leftStickX, float leftStickY, bool rightAHeld,
                                      bool rightBHeld, bool menuChordHeld, float triggerChordValue,
                                      float dtSeconds) {
    bool gatedMenuChordHeld = false;
    float gatedTriggerChordValue = 0.f;
    computeMenuChordGate(menuChordHeld, triggerChordValue, dtSeconds, gatedMenuChordHeld,
                          gatedTriggerChordValue);

    float smoothedLeftStickX = 0.f;
    float smoothedLeftStickY = 0.f;
    advanceMenuStickSmoothing(leftStickX, leftStickY, dtSeconds, smoothedLeftStickX,
                               smoothedLeftStickY);

    writeVrMenuGamepadOutput(smoothedLeftStickX, smoothedLeftStickY, rightAHeld, rightBHeld,
                              gatedMenuChordHeld, gatedTriggerChordValue);
}

// Releases/centers every input this module drives. Called unconditionally
// from every early-return path in tick()'s "reset up front" block so
// nothing can ever get stuck held (e.g. a menu chord held down across a
// dropped frame) -- same reasoning as that block's existing
// g_duskVREyePassOpen/desktop-mirror resets. Deliberately does NOT call
// computeMenuChordGate() (would corrupt its hold-elapsed timer, see the
// TAKE 2 comment above) -- but DOES call advanceMenuStickSmoothing()
// toward (0,0), on purpose, reproducing v1's original double-per-frame
// application (see the v7 history note above for why this one is
// intentional, unlike the chord timer).
inline void neutralizeVrMenuGamepadState(float dtSeconds) {
    float unusedX = 0.f;
    float unusedY = 0.f;
    advanceMenuStickSmoothing(0.f, 0.f, dtSeconds, unusedX, unusedY);
    writeVrMenuGamepadOutput(0.f, 0.f, false, false, false, 0.f);
}

// Genuine teardown only -- called from tick()'s EXITING/LOSS_PENDING
// branch (real session teardown), deliberately NOT from STOPPING (the
// resumable "headset taken off / dashboard focus-steal" case), matching
// how this file's own XR session handling treats those two differently.
// Safe to call even if never attached.
inline void detachVrMenuGamepad() {
    if (detail::g_vrMenuGamepadInstanceId == 0) {
        return;
    }
    SDL_DetachVirtualJoystick(detail::g_vrMenuGamepadInstanceId);
    detail::g_vrMenuGamepadInstanceId = 0;
    OutputDebugStringA("[dusk::vr::menugamepad] detached\n");
}

}  // namespace dusk::vr
