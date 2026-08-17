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

// Left-stick smoothing -- added per user report ("navigation works,
// however it is insanely fast unlike a regular controller"). Navigation
// REPEAT SPEED itself is governed entirely by dusk/ui/input.cpp's own
// wall-clock repeat_interval() ramp -- identical code path for a real
// gamepad and this virtual one, confirmed by reading it, and NOT
// magnitude-dependent -- so a difference "unlike a regular controller"
// can't come from that shared ramp itself. The likely real mechanism:
// dusk/ui/input.cpp's press/release hysteresis
// (kGamepadAxisPressThreshold=16384, kGamepadAxisReleaseThreshold=12000)
// assumes a real gamepad's mechanically-stabilized stick (resting against
// a thumb, often braced by the same hand gripping the controller body),
// which naturally crosses that band once per deliberate push. A VR
// controller's stick is held in an unsupported, floating hand -- normal
// hand tremor can flicker the raw value back and forth across that same
// band several times a second. Each crossing fires an immediate, un-
// ramped FRESH press (begin_gamepad_key(), input.cpp) independent of the
// intended hold-then-ramp repeat behavior -- several of those firing in
// quick succession from tremor would read as erratic rapid-fire, not a
// smooth accelerating repeat, matching the report. Low-pass filtering the
// raw stick value before it ever reaches SDL smooths out that tremor
// while still tracking a real, deliberate push -- framerate-independent
// (uses dtSeconds, not a fixed per-frame blend factor) via the standard
// exponential-smoothing formula, same shape as this project's other
// smoothed-value trackers (e.g. vr_stereo_render.hpp's HUD orientation
// damping). kMenuStickSmoothingTimeConstant is an untested starting
// guess, not derived from anything -- the one constant to retune if this
// still feels off, or feels too sluggish to be responsive.
namespace detail {
inline float g_leftStickSmoothedX = 0.f;
inline float g_leftStickSmoothedY = 0.f;
}  // namespace detail
inline constexpr float kMenuStickSmoothingTimeConstant = 0.08f;  // seconds

// Call once per real frame with whatever this frame's VR controller state
// already computed for gameplay (see vr_main.cpp's tick() -- these are ALL
// already-read locals, no new OpenXR action reads needed). Values use
// SDL's OWN ranges, not PADStatus's s8 scale: sticks are Sint16
// (SDL_JOYSTICK_AXIS_MIN..MAX, i.e. -32768..32767), the trigger axis is
// 0..SDL_JOYSTICK_AXIS_MAX (SDL_gamepad.h's own doc: "Trigger axis values
// range from 0 (released) to SDL_JOYSTICK_AXIS_MAX (fully pressed)").
// dtSeconds drives the menu-chord cooldown above -- pass
// pacing.presentation_dt_seconds (real measured frame time), same source
// every other per-frame timer in this codebase uses. No-ops gracefully if
// the joystick isn't open yet, same reasoning as
// updateVrMenuGamepadPlayerIndex() above.
inline void updateVrMenuGamepadState(float leftStickX, float leftStickY, bool rightAHeld,
                                      bool rightBHeld, bool menuChordHeld, float triggerChordValue,
                                      float dtSeconds) {
    if (detail::g_vrMenuGamepadInstanceId == 0) {
        return;
    }
    SDL_Joystick* joystick = SDL_GetJoystickFromID(detail::g_vrMenuGamepadInstanceId);
    if (joystick == nullptr) {
        return;
    }

    if (detail::g_menuChordCooldownRemaining > 0.f) {
        detail::g_menuChordCooldownRemaining -= dtSeconds;
        menuChordHeld = false;
        triggerChordValue = 0.f;
    } else if (menuChordHeld && triggerChordValue > 0.5f) {
        // 0.5 roughly matches dusk/ui/input.cpp's own kGamepadAxisPressThreshold
        // (16384 of 32767) -- only start the cooldown once the chord is
        // actually about to fire, not on every frame it's merely trending
        // upward.
        detail::g_menuChordCooldownRemaining = kMenuChordCooldownSec;
    }

    // Left-stick smoothing -- see kMenuStickSmoothingTimeConstant's own
    // comment for why. dtSeconds-based (not a fixed blend factor) so the
    // filter's actual time constant stays correct regardless of VR's
    // real, sometimes-variable frame rate.
    const float smoothingAlpha =
        1.f - std::exp(-dtSeconds / kMenuStickSmoothingTimeConstant);
    detail::g_leftStickSmoothedX += (leftStickX - detail::g_leftStickSmoothedX) * smoothingAlpha;
    detail::g_leftStickSmoothedY += (leftStickY - detail::g_leftStickSmoothedY) * smoothingAlpha;
    leftStickX = detail::g_leftStickSmoothedX;
    leftStickY = detail::g_leftStickSmoothedY;

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
    // first-attempt false negative, see its comment above): every
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

// Releases/centers every input this module drives. Called unconditionally
// from every early-return path in tick()'s "reset up front" block so
// nothing can ever get stuck held (e.g. a menu chord held down across a
// dropped frame) -- same reasoning as that block's existing
// g_duskVREyePassOpen/desktop-mirror resets. dtSeconds still decrements
// the cooldown above even here, so an early-return-heavy stretch of
// frames doesn't stall it.
inline void neutralizeVrMenuGamepadState(float dtSeconds) {
    updateVrMenuGamepadState(0.f, 0.f, false, false, false, 0.f, dtSeconds);
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
