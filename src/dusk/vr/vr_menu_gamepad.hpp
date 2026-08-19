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
// for context; skip to "CURRENT DESIGN (v8)" below for what's actually
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
// (In hindsight: strong evidence the ORIGINAL "confirmed working" v1
// report was a shallow test -- quick taps only, never a sustained hold --
// since a plain rise-time filter can never fix an ongoing repeat-ramp
// problem; see v6's reasoning below.)
//
// v6: took v5's result as proof the filter itself is the wrong tool
// (it only ever controls the FIRST press's rise time, never the ONGOING
// rate once a real sustained hold lets dusk/ui/input.cpp's own repeat
// ramp take over) and restored v4's pulse gate (with its confirmed
// hysteresis fix) at a much slower cadence (0.6s/move). STILL reported
// "not fixed."
//
// v7: per the user's direct question, reproduced the ORIGINAL v1 code
// verbatim -- including its double-per-frame-application quirk via
// neutralizeVrMenuGamepadState() -- on the theory that some other,
// separate bug (not the filter itself) was the real culprit all along.
// STILL reported broken; the user asked to just disable the menu-open
// chord entirely rather than keep guessing, which is where this sat
// until now.
//
// CURRENT DESIGN (v8): two changes, one a REFINEMENT of v6's already-
// mechanically-proven pulse gate, one a genuinely NEW root cause never
// addressed in v1-v7 at all:
//
// (1) DOMINANT-AXIS SELECTION, new this round. v1-v7 all treated the
// stick's X and Y axes as two fully independent inputs, each with its
// own deadzone/gate/repeat state -- exactly matching how a real gamepad
// stick is usually pushed close to purely along one axis (a thumb resting
// on a desk-braced controller has a stable pivot to push cleanly along
// one direction). A hand floating in the air holding a VR controller has
// no such brace -- an intended "straight up" push very plausibly drifts a
// few degrees off-axis, which is enough to cross BOTH axes' deadzones at
// once. With independent per-axis gating, that fires TWO concurrent,
// independently-repeating navigation inputs (e.g. up AND right) instead
// of one -- which would read exactly as "spamming through entries" /
// "too fast", and is untouched by any smoothing-time-constant or
// pulse-interval retuning, since it's not a rate problem at all. Fixed by
// zeroing whichever raw axis has the smaller magnitude BEFORE any
// deadzone/gate logic runs, so at most one direction can ever be
// requested at a time -- see advanceMenuStickPulse() below.
//
// (2) Pulse gate restored from v6, decoupled from
// neutralizeVrMenuGamepadState() entirely this time. v7's double-
// application was deliberately reproduced for the OLD smoothing filter
// because that filter's own convergence math tolerates being advanced
// twice a frame (see the removed v7 comment, kept only in git history
// now). This pulse gate is different in kind: like
// computeMenuChordGate()'s hold-elapsed timer (see the TAKE 2 comment
// below), it's a real cross-frame STATE MACHINE with phase timers that
// must accumulate real dtSeconds exactly once per real frame to produce
// the intended fixed cadence -- calling it a second time from neutralize
// would silently shrink every phase's real duration, the same class of
// bug TAKE 2 already root-caused for the hold-to-open timer. So, like
// that timer, advanceMenuStickPulse() is called from EXACTLY ONE place:
// updateVrMenuGamepadState(), the real per-frame path.
// neutralizeVrMenuGamepadState() writes plain neutral values directly via
// writeVrMenuGamepadOutput() instead, same shape as the chord's own
// neutral path.
//
// v8 RESULT: user tested and sent back a real [dusk::vr::menustickpulse]
// capture (finally -- the first round in this whole saga to actually have
// one for its own tuning). Dominant-axis selection held up perfectly --
// every single transition across the whole session showed lockedY=0,
// zero evidence of diagonal double-firing. But the capture's very FIRST
// episode fired TWO pulses (Idle->Active->Gap->Active(re-engaged)->Gap->
// Idle) instead of one -- confirmed by the user's own report, "flicking
// the stick moves to the end of the menu right away" (2 tab-advances on
// one flick, on a several-tab bar, reads exactly like an unwanted jump).
//
// ROOT CAUSE (v8.1): the Gap phase's re-engage check only ever sampled
// the stick's position ONCE, at the exact instant the gap timer expired
// -- not continuously through the gap. A real physical release (spring-
// back to center) can easily take longer than kMenuStickPulseGapSec
// (0.22s) to fully settle -- if the stick was still mid-release and
// happened to read above the (lower) release threshold at that one exact
// frame, a single intended flick got misread as a sustained hold and
// fired a second pulse. FIX: advanceMenuStickPulse()'s Gap case now
// checks the release condition EVERY real frame during the gap, not just
// at expiry -- the moment the stick dips below the release threshold at
// any point, it transitions to Idle immediately (canceling any possible
// re-engage), regardless of what it reads afterward. Only a stick that
// NEVER drops below the release threshold for the entire gap (a genuine
// sustained hold) can produce a second pulse now. This is a strictly
// tighter version of the same hysteresis idea already proven correct in
// v4 -- not a new mechanism, just applied continuously instead of at one
// sampled instant.
//
// NOT yet re-tested in-headset after this refinement.
namespace detail {
enum class StickPulsePhase { Idle, Active, Gap };
inline StickPulsePhase g_stickPulsePhase = StickPulsePhase::Idle;
inline float g_stickPulsePhaseRemaining = 0.f;
inline float g_stickPulseLockedX = 0.f;
inline float g_stickPulseLockedY = 0.f;
}  // namespace detail

// Enter threshold (raw stick magnitude, post dominant-axis selection).
inline constexpr float kMenuStickDeadzone = 0.35f;
// Exit/re-engage threshold -- deliberately LOWER than the enter threshold
// (hysteresis). Without this, a value hovering right at one single
// boundary can flip-flop frame to frame and fire a fresh press on every
// flip -- this was a real, confirmed bug in an earlier round (v4), found
// via a live capture showing the same key firing fresh-press -> released
// -> fresh-press every single real frame during what should have been
// one steady tap. Keep this fix intact if this mechanism is ever touched
// again.
inline constexpr float kMenuStickReleaseThreshold = 0.2f;
// How long the virtual axis is driven to full deflection per pulse --
// must stay comfortably under dusk/ui/input.cpp's own
// kGamepadRepeatInitialDelay (0.32s), so input.cpp never gets the chance
// to start its own accelerating repeat ramp within a single pulse; this
// module's fixed cadence is the only thing that should ever determine
// the rate. Untested starting guess, not derived from anything.
inline constexpr float kMenuStickPulseActiveSec = 0.08f;
// Forced-neutral gap between pulses -- together with the active duration
// above, sets the effective navigation rate (0.08 + 0.22 = 0.3s/move,
// ~3.3 moves/sec). Untested starting guess -- the constant to retune
// first if this still feels too fast/slow, since it directly controls
// pace in a way none of v1-v7's mechanisms could isolate cleanly (a
// smoothing time constant only ever governed the FIRST press's rise
// time, never the steady-state rate).
inline constexpr float kMenuStickPulseGapSec = 0.22f;

// Advances the fixed-cadence pulse-gate state machine by one dtSeconds
// step and returns this frame's output (each axis is exactly -1, 0, or
// +1 -- full deflection or nothing, never a partial value, so the eventual
// SDL axis write reliably clears input.cpp's press/release thresholds
// every single transition). MUST be called exactly once per real frame,
// only from the real per-frame path -- see the v8 design note above for
// why (the same reasoning as computeMenuChordGate()'s hold-elapsed
// timer, in the TAKE 2 comment below).
inline void advanceMenuStickPulse(float rawX, float rawY, float dtSeconds, float& outX,
                                   float& outY) {
    // Dominant-axis selection -- see the v8 design note above. Zero
    // whichever raw axis has the smaller magnitude so at most one
    // direction can ever be in play.
    if (std::abs(rawX) >= std::abs(rawY)) {
        rawY = 0.f;
    } else {
        rawX = 0.f;
    }
    const float rawMagnitude = std::abs(rawX) + std::abs(rawY);  // exactly one term is nonzero

    switch (detail::g_stickPulsePhase) {
    case detail::StickPulsePhase::Idle: {
        outX = 0.f;
        outY = 0.f;
        if (rawMagnitude >= kMenuStickDeadzone) {
            detail::g_stickPulseLockedX = rawX > 0.f ? 1.f : (rawX < 0.f ? -1.f : 0.f);
            detail::g_stickPulseLockedY = rawY > 0.f ? 1.f : (rawY < 0.f ? -1.f : 0.f);
            detail::g_stickPulsePhase = detail::StickPulsePhase::Active;
            detail::g_stickPulsePhaseRemaining = kMenuStickPulseActiveSec;
        }
        return;
    }
    case detail::StickPulsePhase::Active: {
        outX = detail::g_stickPulseLockedX;
        outY = detail::g_stickPulseLockedY;
        detail::g_stickPulsePhaseRemaining -= dtSeconds;
        if (detail::g_stickPulsePhaseRemaining <= 0.f) {
            detail::g_stickPulsePhase = detail::StickPulsePhase::Gap;
            detail::g_stickPulsePhaseRemaining = kMenuStickPulseGapSec;
        }
        return;
    }
    case detail::StickPulsePhase::Gap: {
        outX = 0.f;
        outY = 0.f;

        // Re-engage requires the stick to have stayed continuously past
        // the release threshold for the WHOLE gap, checked every real
        // frame -- not just sampled once at the instant the gap timer
        // expires. Found via a real in-headset report + log capture: a
        // single deliberate flick-and-release can easily take longer to
        // physically settle back to center than kMenuStickPulseGapSec
        // (0.22s) -- if the stick happened to still read above the
        // (lower) release threshold at that one exact instant, purely by
        // bad luck of timing mid-release, the OLD one-shot-at-expiry check
        // would treat a single intended flick as a sustained hold and fire
        // a second pulse -- landing 2 tabs over instead of 1, reported as
        // "flicking the stick moves to the end of the menu right away".
        // Checking every frame means the very first frame the stick dips
        // below threshold during the gap immediately cancels any
        // re-engage, regardless of what it reads later -- only a stick
        // that TRULY never dropped below threshold for the entire gap
        // (a genuine sustained hold, not a releasing flick) can produce a
        // second pulse.
        const bool stillHeldX = detail::g_stickPulseLockedX != 0.f &&
                                 rawX * detail::g_stickPulseLockedX > 0.f &&
                                 std::abs(rawX) >= kMenuStickReleaseThreshold;
        const bool stillHeldY = detail::g_stickPulseLockedY != 0.f &&
                                 rawY * detail::g_stickPulseLockedY > 0.f &&
                                 std::abs(rawY) >= kMenuStickReleaseThreshold;
        if (!stillHeldX && !stillHeldY) {
            detail::g_stickPulsePhase = detail::StickPulsePhase::Idle;
            return;
        }

        detail::g_stickPulsePhaseRemaining -= dtSeconds;
        if (detail::g_stickPulsePhaseRemaining <= 0.f) {
            // Held past the release threshold for the entire gap -- a
            // genuine sustained hold, not a releasing flick. Re-engage.
            detail::g_stickPulsePhase = detail::StickPulsePhase::Active;
            detail::g_stickPulsePhaseRemaining = kMenuStickPulseActiveSec;
        }
        return;
    }
    }
}

// Fixes the stuck-right bug (same fix shape as v7's, just retargeted at
// this state machine instead of a smoothing accumulator): call once,
// directly from vr_main.cpp's tick(), on the real menu-closed->open
// transition. Hard-resets to Idle so a freshly-opened menu never inherits
// a locked direction/mid-pulse state left over from before the menu was
// open.
inline void resetMenuStickPulseState() {
    detail::g_stickPulsePhase = detail::StickPulsePhase::Idle;
    detail::g_stickPulsePhaseRemaining = 0.f;
    detail::g_stickPulseLockedX = 0.f;
    detail::g_stickPulseLockedY = 0.f;
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
// are fed UNCONDITIONALLY -- see advanceMenuStickPulse()'s own comment for
// why this call site deliberately does NOT gate them by menu visibility
// (that's handled separately, via resetMenuStickPulseState() at the real
// transition, called directly from vr_main.cpp). dtSeconds drives the
// menu-chord cooldown/hold-to-open timer and the stick pulse gate -- pass
// pacing.presentation_dt_seconds (real measured frame time), same source
// every other per-frame timer in this codebase uses.
inline void updateVrMenuGamepadState(float leftStickX, float leftStickY, bool rightAHeld,
                                      bool rightBHeld, bool menuChordHeld, float triggerChordValue,
                                      float dtSeconds) {
    bool gatedMenuChordHeld = false;
    float gatedTriggerChordValue = 0.f;
    computeMenuChordGate(menuChordHeld, triggerChordValue, dtSeconds, gatedMenuChordHeld,
                          gatedTriggerChordValue);

    float pulsedLeftStickX = 0.f;
    float pulsedLeftStickY = 0.f;
    advanceMenuStickPulse(leftStickX, leftStickY, dtSeconds, pulsedLeftStickX, pulsedLeftStickY);

    writeVrMenuGamepadOutput(pulsedLeftStickX, pulsedLeftStickY, rightAHeld, rightBHeld,
                              gatedMenuChordHeld, gatedTriggerChordValue);
}

// Releases/centers every input this module drives.
inline void neutralizeVrMenuGamepadState() {
    writeVrMenuGamepadOutput(0.f, 0.f, false, false, false, 0.f);
}

// ROOT CAUSE FOUND 2026-08-18, via a real user report ("if I hold down A,
// it spams the entry on and off") that turned out to affect EVERY input
// this module drives, not just the stick -- a plain, non-pulse-gated
// button (A) doing the exact same thing proved the bug was never in the
// stick's own pulse-gate design (v8/v8.1) at all.
//
// neutralizeVrMenuGamepadState() used to be called UNCONDITIONALLY at the
// very top of EVERY tick(), before ANY early-return check -- "reset up
// front in case of an early return" (same shape as the harmless plain-flag
// resets alongside it, e.g. g_duskVREyePassOpen). But unlike those flags,
// neutralize performs a REAL, OBSERVABLE side effect every time it runs:
// it writes actual "everything false/neutral" values to the SDL virtual
// joystick and calls SDL_UpdateJoysticks(), which FLUSHES those writes
// into real queued SDL_EVENT_GAMEPAD_* events immediately. On every normal
// frame that does NOT early-return, the real per-frame path
// (updateVrMenuGamepadState()) runs LATER that same frame and writes the
// ACTUAL held state -- also flushed via its own SDL_UpdateJoysticks() call.
// Net effect, every single real VR frame, for anything actually held:
// neutralize writes false+flushes (a real RELEASE event, since the
// previous frame's real value is still cached) immediately followed by the
// real update writing true+flushing (a real PRESS event) -- a full
// press/release cycle on EVERY frame (~72-90Hz), not just once on a real
// press/release. dusk/ui/input.cpp's emit_key_press() calls
// context.ProcessKeyDown() UNCONDITIONALLY every time it's invoked, with
// no dedup against an "already held" state -- so every one of these
// spurious per-frame presses reached RmlUi as a genuinely fresh key-down,
// which for a toggle-style Confirm/Submit control means toggling on every
// single frame it's held. This fully explains the report: it's not a
// repeat-rate problem (dusk/ui/input.cpp's own KI_RETURN isn't even
// marked repeatable -- is_repeatable_key() -- so its OWN repeat system
// was never involved), it's genuine duplicate press/release SDL events
// generated by this module's own redundant per-frame write-then-overwrite.
//
// This also fully explains the "flick jumps 2 tabs" symptom the v8.1 fix
// (above) targeted -- that fix wasn't wrong, but it was chasing a much
// smaller effect on top of this larger one; the stick's OWN internal phase
// state machine (Idle/Active/Gap) was never affected by this bug (it
// doesn't read anything back from SDL), which is exactly why the
// [dusk::vr::menustickpulse] capture showed clean, correct transitions
// even while the ACTUAL SDL-level events were spamming underneath,
// invisible to that diagnostic.
//
// FIX: stop calling neutralizeVrMenuGamepadState() unconditionally. It
// should only ever run as a genuine fallback -- when the real per-frame
// update DIDN'T run this frame (tick() has SEVEN different early-return
// points below where it used to sit: no session, session state
// transitions/teardown, session-not-running, xrWaitFrame/xrBeginFrame
// failure, shouldRender==false, view-not-ready). Manually adding a
// neutralize call at each of those seven return sites would work today but
// is fragile against tick() gaining an EIGHTH early-return path later and
// someone forgetting to add a call there too -- this project already hit
// and fixed the identical shape of problem once before, for tick()'s own
// reentrancy flag (see TickReentrancyGuard, vr_main.cpp) -- so this reuses
// that same RAII solution: construct MenuGamepadFrameGuard once, at the
// exact spot the old unconditional call used to sit; its destructor
// neutralizes automatically UNLESS markRealUpdateRan() was called first,
// covering every existing early-return path AND any future one, by
// construction, with no per-site bookkeeping required.
class MenuGamepadFrameGuard {
public:
    MenuGamepadFrameGuard() = default;
    ~MenuGamepadFrameGuard() {
        if (!mRealUpdateRan) {
            neutralizeVrMenuGamepadState();
        }
    }
    MenuGamepadFrameGuard(const MenuGamepadFrameGuard&) = delete;
    MenuGamepadFrameGuard& operator=(const MenuGamepadFrameGuard&) = delete;

    // Called once, right alongside the real updateVrMenuGamepadState()
    // call site (the only place that should ever call it) -- tells the
    // guard's destructor a real update already ran this frame, so it must
    // NOT also neutralize (that would reintroduce the exact bug this
    // class exists to fix: a real-value write followed by a neutral write
    // in the same frame).
    void markRealUpdateRan() { mRealUpdateRan = true; }

private:
    bool mRealUpdateRan = false;
};

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
