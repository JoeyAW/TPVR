#pragma once

#include <array>

#include "dusk/config_var.hpp"
#include "dusk/ui/controls.hpp"

namespace dusk {

using config::ConfigVar;
using config::ActionBindConfigVar;

// Default internal resolution scale (0 = Auto). On standalone VR (Quest) Auto crashes the game
// when the map is opened, so it defaults to a fixed 1x there instead. Shared by the compiled
// default and the first-launch "Dusklight" preset so a fresh Quest install never lands on Auto.
// Keyed on TARGET_ANDROID rather than TARGET_PC, since TARGET_PC is defined on every non-console
// build, Android included.
#if defined(TARGET_ANDROID) || defined(__ANDROID__)
inline constexpr int kDefaultInternalResolutionScale = 1;
#else
inline constexpr int kDefaultInternalResolutionScale = 0;
#endif

enum class BloomMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class DepthOfFieldMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class Resampler : int {
    Bilinear = 0,
    Area = 1,
};

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
    Japanese = 6,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

enum class FrameInterpMode : u8 {
    Off = 0,
    Capped = 1,
    Unlimited = 2,
};

enum class LetterboxMode : u8 {
    Off = 0,
    On = 1,
    GameplayOnly = 2,
    CutsceneOnly = 3,
};

enum class TouchTargeting : u8 {
    Hybrid = 0,
    Hold = 1,
    Switch = 2,
};

enum class MenuScaling : u8 {
    GameCube = 0,
    Wii = 1,
    Dusklight = 2,
};

enum class AlwaysGreatspinMode : u8 {
    OFF = 0,
    AFTER_SKILL = 1,
    ALWAYS = 2,
};

enum class MagicArmorMode : u8 {
    NORMAL = 0,
    ON_DAMAGE = 1,
    DOUBLE_DEFENSE = 2,
    INVINCIBLE = 3,
    COSMETIC = 4,
};

enum class AudioOutputMode : u8 {
    StereoSpeakers = 0,
    StereoHeadphones = 1,   // spatial audio
    Surround6ch = 2,        // discrete 5.1
    Surround8ch = 3,        // discrete 7.1
};

namespace config {
template <>
struct ConfigEnumRange<BloomMode> {
    static constexpr auto min = BloomMode::Off;
    static constexpr auto max = BloomMode::Dusk;
};

template <>
struct ConfigEnumRange<DepthOfFieldMode> {
    static constexpr auto min = DepthOfFieldMode::Off;
    static constexpr auto max = DepthOfFieldMode::Dusk;
};

template <>
struct ConfigEnumRange<Resampler> {
    static constexpr auto min = Resampler::Bilinear;
    static constexpr auto max = Resampler::Area;
};

template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Japanese;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

template <>
struct ConfigEnumRange<FrameInterpMode> {
    static constexpr auto min = FrameInterpMode::Off;
    static constexpr auto max = FrameInterpMode::Unlimited;
};

template <>
struct ConfigEnumRange<LetterboxMode> {
    static constexpr auto min = LetterboxMode::Off;
    static constexpr auto max = LetterboxMode::CutsceneOnly;
};

template <>
struct ConfigEnumRange<TouchTargeting> {
    static constexpr auto min = TouchTargeting::Hybrid;
    static constexpr auto max = TouchTargeting::Switch;
};

template <>
struct ConfigEnumRange<MenuScaling> {
    static constexpr auto min = MenuScaling::GameCube;
    static constexpr auto max = MenuScaling::Dusklight;
};

template <>
struct ConfigEnumRange<AlwaysGreatspinMode> {
    static constexpr auto min = AlwaysGreatspinMode::OFF;
    static constexpr auto max = AlwaysGreatspinMode::ALWAYS;
};

template <>
struct ConfigEnumRange<MagicArmorMode> {
    static constexpr auto min = MagicArmorMode::NORMAL;
    static constexpr auto max = MagicArmorMode::COSMETIC;
};

template <>
struct ConfigEnumRange<AudioOutputMode> {
    static constexpr auto min = AudioOutputMode::StereoSpeakers;
    static constexpr auto max = AudioOutputMode::Surround8ch;
};

template <>
struct ConfigValueTraits<ui::ControlLayout> {
    static constexpr bool enabled = true;
};
}  // namespace config

// Persistent user settings

struct UserSettings {
    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        ConfigVar<int> maxFrameRate;
        ConfigVar<bool> rememberWindowSize;
        ConfigVar<int> lastWindowWidth;
        ConfigVar<int> lastWindowHeight;
        ConfigVar<int> uiScale;
    } video;

    struct {
        // Audio
        ConfigVar<AudioOutputMode> outputMode;
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;
        ConfigVar<bool> hideTvSettingsScreen;
        ConfigVar<bool> biggerWallets;
        ConfigVar<bool> noReturnRupees;
        ConfigVar<bool> disableRupeeCutscenes;
        ConfigVar<bool> fastTransitions;
        ConfigVar<bool> noSwordRecoil;
        ConfigVar<int> damageMultiplier;
        ConfigVar<bool> noHeartDrops;
        ConfigVar<bool> instantDeath;
        ConfigVar<bool> fastClimbing;
        ConfigVar<bool> noMissClimbing;
        ConfigVar<bool> fastTears;
        ConfigVar<bool> no2ndFishForCat;
        ConfigVar<bool> buttonFishing;
        ConfigVar<bool> instantSaves;
        ConfigVar<bool> instantText;
        ConfigVar<bool> holdToMash;
        ConfigVar<bool> sunsSong;
        ConfigVar<bool> autoSave;
        ConfigVar<bool> enhancedMapMenus;
        ConfigVar<bool> aimingReticle;

        // Preferences
        ConfigVar<bool> enableMirrorMode;
        ConfigVar<bool> minimalHUD;
        ConfigVar<float> hudScale;
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableLinkDollRotation;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;
        ConfigVar<bool> enableDiscordPresence;
        ConfigVar<MenuScaling> menuScalingMode;

        // Graphics
        ConfigVar<BloomMode> bloomMode;
        ConfigVar<float> bloomMultiplier;
        ConfigVar<DepthOfFieldMode> depthOfFieldMode;
        ConfigVar<bool> disableWaterRefraction;
        ConfigVar<bool> enableTextureReplacements;
        ConfigVar<FrameInterpMode> enableFrameInterpolation;
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<Resampler> resampler;
        ConfigVar<bool> enableMapBackground;
        ConfigVar<bool> disableCutscenePillarboxing;
        ConfigVar<LetterboxMode> disableLetterboxing;
        ConfigVar<bool> enableHighQualityMinimapTextures;
        // Shows one VR eye's rendered view on the desktop window instead of
        // leaving it stale/blank while in the headset. Reuses aurora's
        // existing present-resample pass (see aurora::gfx::
        // set_present_source_mirror()) -- no extra render pass, no CPU
        // readback, near-zero cost.
        ConfigVar<bool> vrDesktopMirror;
        // Hides Link's whole body model in VR (any outfit/armor -- gates
        // the modelDraw(mpLinkModel, ...) call itself, not per-outfit
        // material indices, so it works uniformly regardless of which
        // clothing/armor resource is currently loaded) while leaving the
        // tracked hand model, sword/shield, and held items untouched.
        // Default off (added 2026-08-18 per explicit user request -- purely
        // a player preference for players who don't want to see their own
        // avatar body in VR, not a bug workaround).
        ConfigVar<bool> vrShowBody;
        // Forces the WHOLE game into third-person VR, the same way Wolf
        // form/cutscenes already render (camera anchored to the flatscreen
        // third-person eye instead of Link's head, headset position/
        // rotation still applied on top -- see isFirstPerson(),
        // vr_link_visibility.hpp) -- rather than a separate camera-anchor
        // mode invented from scratch. Also forces Link's body (and stowed
        // sword/shield) to show even if "Hide Body" above is on, since a
        // third-person view with an invisible body would be pointless --
        // see the vrThirdPerson checks at both call sites. Default off
        // (added 2026-08-18 per explicit user request: "add a third person
        // option that shows link's body and puts the entire game in third
        // person").
        ConfigVar<bool> vrThirdPerson;
        // Attaches Link's body rotation to the headset's own yaw. Without
        // this, current.angle.y/shape_angle.y (daAlink_c::
        // setSpeedAndAngleNormal(), d_a_alink.cpp) only ease toward
        // mMoveAngle (which already includes the HMD's yaw -- see
        // getHeadMoveAngleS(), the 2026-08-07 movement-direction fix) at
        // the base game's normal walking turn rate -- meaning a fast real
        // head turn while standing still visibly lags behind before the
        // body catches up. With this on, current.angle.y/shape_angle.y are
        // snapped directly to the HMD's yaw every real sim tick instead
        // (no turn-rate smoothing), so Link's body always faces exactly
        // where the player is looking. Scoped to setSpeedAndAngleNormal()
        // only, which is already never called while Z-targeting/locked-on,
        // throwing an item, or hookshot-moving (see that function's own
        // call site) -- exactly the states where "always face the
        // headset" would fight with more appropriate existing facing
        // logic, so those are unaffected either way. Default ON per
        // explicit user request (added 2026-09-08).
        ConfigVar<bool> vrAttachBodyRotationToHead;
        // EXPERIMENTAL. Default off (added 2026-08-20, explicit user
        // request). Normal behavior as of this same request: real scripted
        // CUTSCENES (isRealCutsceneRunning(), vr_link_visibility.hpp --
        // distinguishes them from plain dialogue and door/treasure
        // transitions, both unaffected by this setting either way) now
        // default to THIRD-PERSON while in first-person VR mode, instead of
        // the previous "first-person whenever Link's own body is actually
        // drawn in the shot" behavior from the 2026-08-08 fix. Turning this
        // on restores that exact previous behavior for cutscenes
        // specifically (still gated by checkPlayerNoDraw() -- a cutscene
        // that hides/swaps out Link's real body still falls back to third-
        // person either way) -- see isFirstPerson()'s own final branch.
        // Labeled EXPERIMENTAL in the UI since forcing first-person into an
        // authored cutscene camera not designed to be viewed that way can
        // put the viewer somewhere the shot was never built for (the
        // original reason this fallback existed as third-person-by-default
        // in the first place, before the 2026-08-08 request).
        ConfigVar<bool> vrExperimentalCutsceneFirstPerson;
        // Swaps which physical controller the sword and shield track when
        // drawn: sword to the RIGHT hand, shield to the LEFT (opposite of
        // the base game's own left-handed convention -- sword=
        // mLeftItemJntNo, shield=mRightItemJntNo, unchanged either way;
        // only which tracked controller matrix composes with that rig data
        // changes). Also swaps which controller's swing/thrust gesture
        // drives the sword-attack/shield-bash combat controls, so the hand
        // now holding each item is still the one whose gesture triggers its
        // action. See vr_link_visibility.hpp's refreshTrackedItemMtxLive()
        // (grip orientation, via a mirror-reflection of the existing rig
        // data through Link's own body plane -- vrSwapSwordGripMirrorAxis/
        // vrSwapShieldGripMirrorAxis below are the live-adjustable knobs
        // for this) and vr_main.cpp's tick() (gesture-to-controller swap).
        // Does NOT affect the "raise shield" hold control (still left
        // squeeze, unmentioned by the original request) or any
        // ranged-weapon aiming hand. Default off (added 2026-08-20,
        // explicit user request).
        ConfigVar<bool> vrSwapSwordShieldHands;
        // Which local axis (0=X, 1=Y, 2=Z) mirrorLocalMtxAxis()
        // (vr_link_visibility.hpp) reflects the sword's grip data through
        // when vrSwapSwordShieldHands is on -- see that function's own
        // comment for the math. Debug-only for now (Debug > Graphics
        // Settings, ImGuiMenuTools.cpp), NOT exposed in the main VR
        // settings tab -- this is a one-time internal calibration knob
        // (which axis is the rig's own sagittal plane), not a per-player
        // preference. SPLIT from a single shared axis into two independent
        // ones (this + vrSwapShieldGripMirrorAxis below) 2026-08-20, same
        // day -- the first in-headset test found one shared axis value
        // that looked "about right" for the sword but left the shield
        // showing its back/straps instead of its front face ("backwards"),
        // proving the two items need their own axis (different item
        // joints, no reason to assume the same local-frame convention).
        // Once each is confirmed, update its default to match and remove
        // the debug sliders, per this project's normal practice.
        ConfigVar<int> vrSwapSwordGripMirrorAxis;
        // Shield's counterpart to vrSwapSwordGripMirrorAxis above -- see
        // that field's comment for the full reasoning behind the split.
        ConfigVar<int> vrSwapShieldGripMirrorAxis;
        // Additional 180-degree rotation (0=X, 1=Y, 2=Z; -1 = none) applied
        // AFTER vrSwapSwordGripMirrorAxis's mirror -- rotate180LocalMtxAxis()
        // (vr_link_visibility.hpp) for the math. Added 2026-08-20 same-day
        // follow-up: the mirror alone can get which FACE of an item shows
        // right while leaving it spun 180 degrees the wrong way around
        // that face's own normal. -1 default (no extra flip). Debug-only,
        // same reasoning as the mirror axis settings above.
        //
        // A genuine GEOMETRIC mesh mirror (via J3DModel::setBaseScale(),
        // moving an asymmetric feature like the shield's handle to the
        // mesh's own mirror-correct side, rather than just repositioning
        // the rigid attachment the way this setting and
        // vrSwapSwordGripMirrorAxis do) was also attempted the same day,
        // but never got the required cull-mode compensation working
        // despite five separate attempts -- see vr-mod-notes for the full
        // trail if revisiting this. Removed entirely per explicit user
        // request rather than left disabled.
        ConfigVar<int> vrSwapSwordExtraFlipAxis;
        // Shield's counterpart to vrSwapSwordExtraFlipAxis above.
        ConfigVar<int> vrSwapShieldExtraFlipAxis;
        // Fixed positional nudge (game world units, 100 units/metre -- see
        // kHorseCameraBackUnits's own comment for the same conversion used
        // elsewhere) applied to the item's grip AFTER the mirror/extra-flip
        // above, in that already-corrected local frame -- i.e. these move
        // the item along ITS OWN current local axes, not world axes. Added
        // 2026-08-20 same-day follow-up: once grip rotation was fixed
        // (Extra Flip Axis), the remaining shield complaint was purely
        // positional -- "the hand is still gripping the handle [correctly],
        // is it possible to move the shield so the hand is on the other
        // side?" -- i.e. the shield mesh's own handle attachment sits
        // off-center (authored for left-hand use, per the abandoned
        // geometric mesh-mirror investigation), so a fixed sideways nudge
        // compensates without touching mesh data. Debug-only, live-
        // adjustable (Debug > Graphics Settings, ImGuiMenuTools.cpp), same
        // reasoning as the axis settings above -- 0.0f default (no nudge)
        // until confirmed in-headset, then bake in and remove the sliders.
        ConfigVar<float> vrSwapSwordGripOffsetX;
        ConfigVar<float> vrSwapSwordGripOffsetY;
        ConfigVar<float> vrSwapSwordGripOffsetZ;
        // Shield's counterpart to the sword offsets above.
        ConfigVar<float> vrSwapShieldGripOffsetX;
        ConfigVar<float> vrSwapShieldGripOffsetY;
        ConfigVar<float> vrSwapShieldGripOffsetZ;
        // Live-adjustable VR gamma-compensation exponent for every runtime
        // EXCEPT SteamVR (which keeps its own separate, independently-tuned,
        // decoupled baseline -- see vr_xr_submit.hpp's
        // Session::effectiveGammaExponent()) -- added 2026-08-16 after
        // "washed out / too bright in headset, fine on the desktop mirror"
        // feedback reproduced on all three runtimes, meaning the pre-
        // existing SteamVR-only compensation couldn't be the whole story.
        // Live-adjustable via the Debug > Graphics Settings ImGui slider
        // (ImGuiMenuTools.cpp) so it can be dialed in per-headset without a
        // rebuild each guess. 2.0 was confirmed correct on Virtual Desktop
        // 2026-08-16, but RESET to 1.0 as of 2026-08-20 -- that tuning was
        // specific to submitting via the plain non-SRGB swapchain format,
        // which vr_xr_submit.hpp's createSwapchain() no longer prefers for
        // these runtimes (see its own comment for the OpenXR-issue-#467
        // reasoning behind now preferring an SRGB-tagged swapchain
        // universally). NOT yet re-tested in-headset against that change on
        // either VD or Meta Link.
        ConfigVar<float> vrGammaCompensation;
        // Live-adjustable VR gamma-compensation exponent for SteamVR
        // specifically, decoupled from vrGammaCompensation above (see its
        // sibling comment) -- SteamVR's own compositor needs a
        // structurally different correction than VD/Meta Link's zero-
        // baseline case, so the two must stay independently tunable.
        // Originally defaulted to the section-6 tuned brightening value
        // (~0.4545 = 1.0/2.2). A 2026-08-16 report ("undersaturated")
        // flipped the default to 1.0 (no compensation); a same-day-later
        // reconsideration flipped it back to ~0.4545; then, immediately
        // after that same day's createSwapchain() SRGB-preference reorder
        // (vr_xr_submit.hpp) confirmed Virtual Desktop correct at 1.0/100%
        // with zero compensation, explicit follow-up: "It should be 1.0 or
        // 100%, not 45%." Back to 1.0 (no compensation) as the compiled
        // default -- THIRD flip on this exact value in one project. Treat
        // any future report about it with real skepticism (a direct
        // side-by-side against the known-correct desktop mirror, not a
        // memory-based impression) before changing it a fourth time.
        ConfigVar<float> vrGammaCompensationSteamVr;
        // Camera-only 6DOF: real head TRANSLATION (leaning, ducking,
        // side-stepping) now offsets the VR camera (and, so hands don't
        // visually desync from a leaning head, the tracked-hand anchor too
        // -- both read the same getVrCameraEyeAnchor(), vr_link_visibility.hpp)
        // relative to a calibrated reference position, on top of the
        // existing rigid core/head-joint anchor. Does NOT move Link's
        // actual in-game position/collision -- see vrPositionalTrackingRadius
        // below for the clamp, and getVrCameraEyeAnchor()'s own comment for
        // the calibration-on-activation + rotate-by-smooth-turn-yaw math
        // (reuses the exact same rotateYawXr()/VR_SCALE_FACTOR conversion
        // already proven for tracked hands). Default ON (added 2026-09-11,
        // explicit user request: "6 degrees of freedom so the headset can
        // move horizontally and vertically from its position" -- scoped to
        // camera-only for now; moving Link's own body/collision with it is
        // an explicitly deferred phase 2, to be picked up once the ongoing
        // body-rotation investigation is resolved). NOT yet tested in
        // headset as of this addition.
        ConfigVar<bool> vrPositionalTracking;
        // Maximum real-world distance (metres) the head-translation offset
        // above is allowed to move the camera from its calibrated
        // reference position, in any direction -- clamped by magnitude
        // (not per-axis) so leaning further than this just stops moving
        // the camera rather than producing an unbounded offset if someone
        // stands up fully or walks away from their calibrated spot.
        // Untested starting guess, not derived from anything -- the first
        // thing to retune (Debug > Graphics Settings slider,
        // ImGuiMenuTools.cpp) if leaning/ducking feels too restrictive or
        // lets the camera drift uncomfortably far.
        ConfigVar<float> vrPositionalTrackingRadius;

        // Audio
        ConfigVar<bool> noLowHpSound;
        ConfigVar<bool> midnasLamentNonStop;

        // Input
        ConfigVar<bool> enableGyroAim;
        ConfigVar<bool> enableGyroRollgoal;
        ConfigVar<float> gyroSensitivityX;
        ConfigVar<float> gyroSensitivityY;
        ConfigVar<float> gyroSensitivityRollgoal;
        ConfigVar<float> gyroSmoothing;
        ConfigVar<float> gyroDeadband;
        ConfigVar<bool> gyroInvertPitch;
        ConfigVar<bool> gyroInvertYaw;
        ConfigVar<bool> enableMouseCamera;
        ConfigVar<bool> enableMouseAim;
        ConfigVar<float> mouseAimSensitivity;
        ConfigVar<float> mouseCameraSensitivity;
        ConfigVar<bool> invertMouseY;
        ConfigVar<bool> freeCamera;
        ConfigVar<bool> enableTouchControls;
        ConfigVar<TouchTargeting> touchTargeting;
        ConfigVar<bool> enableMenuPointer;
        ConfigVar<ui::ControlLayout> touchControlsLayout;
        ConfigVar<bool> invertCameraXAxis;
        ConfigVar<bool> invertCameraYAxis;
        ConfigVar<bool> invertFirstPersonXAxis;
        ConfigVar<bool> invertFirstPersonYAxis;
        ConfigVar<bool> invertAirSwimX;
        ConfigVar<bool> invertAirSwimY;
        ConfigVar<float> freeCameraXSensitivity;
        ConfigVar<float> freeCameraYSensitivity;
        ConfigVar<float> touchCameraXSensitivity;
        ConfigVar<float> touchCameraYSensitivity;
        ConfigVar<bool> debugFlyCam;
        ConfigVar<bool> debugFlyCamLockEvents;
        ConfigVar<bool> allowBackgroundInput;
        std::array<ConfigVar<bool>, 4> enableLED;
        ConfigVar<bool> swapDirectSelect;

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> infiniteArrows;
        ConfigVar<bool> infiniteSeeds;
        ConfigVar<bool> infiniteBombs;
        ConfigVar<bool> infiniteOil;
        ConfigVar<bool> infiniteOxygen;
        ConfigVar<bool> infiniteRupees;
        ConfigVar<bool> enableIndefiniteItemDrops;
        ConfigVar<bool> moonJump;
        ConfigVar<bool> superClawshot;
        ConfigVar<AlwaysGreatspinMode> alwaysGreatspin;
        ConfigVar<bool> enableFastIronBoots;
        ConfigVar<bool> canTransformAnywhere;
        ConfigVar<bool> fastRoll;
        ConfigVar<bool> fastSpinner;
        ConfigVar<MagicArmorMode> armorRupeeDrain;
        ConfigVar<bool> invincibleEnemies;
        ConfigVar<bool> easyQuickSpin;

        // Technical
        ConfigVar<bool> restoreWiiGlitches;

        // Controls
        ConfigVar<bool> enableTurboKeybind;
        ConfigVar<bool> enableResetKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> liveSplitEnabled;
        ConfigVar<bool> showSpeedrunRTATimer;
        ConfigVar<bool> recordingMode;
        ConfigVar<bool> removeQuestMapMarkers;
        ConfigVar<bool> showInputViewer;
        ConfigVar<bool> showInputViewerGyro;
        ConfigVar<bool> enableMoveLinkCombo;
        ConfigVar<bool> enableTeleportCombo;

        ConfigVar<std::string> lastSelectedGameModeId;
    } game;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<bool> checkForModUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
    } backend;

    // Arrays of size 4 for 4 ports
    struct {
        std::array<ActionBindConfigVar, 4> firstPersonCamera;
        std::array<ActionBindConfigVar, 4> callMidna;
        std::array<ActionBindConfigVar, 4> openMapScreen;
        std::array<ActionBindConfigVar, 4> toggleMinimap;
        std::array<ActionBindConfigVar, 4> openDusklightMenu;
        std::array<ActionBindConfigVar, 4> turboSpeedButton;
    } actionBindings;
};

UserSettings& getSettings();

void registerSettings();

void applyInternalResolutionScale(int scale);
void applyResampler(Resampler resampler);

inline bool isLetterboxingDisabled(bool inCutscene) {
    const auto mode = getSettings().game.disableLetterboxing.getValue();
    return mode == LetterboxMode::On ||
           (mode == LetterboxMode::CutsceneOnly && inCutscene) ||
           (mode == LetterboxMode::GameplayOnly && !inCutscene);
}

// Transient settings

struct CollisionViewSettings {
    bool enableTerrainView;
    bool enableWireframe;
    bool enableAtView;
    bool enableTgView;
    bool enableCoView;
    float terrainViewOpacity;
    float colliderViewOpacity;
    float drawRange;
};

struct TriggerViewSettings {
    bool loadZones;
    bool eventAreas;
    bool switchAreas;
    bool eventTags;
    bool midnaStops;
    bool twilightGates;
    bool checkpoints;
    bool paths;
    bool transformDists;
    bool attentionDists;
    bool purpleMistAvoid;
    bool leevers;
    float opacity;
};

struct TransientSettings {
    CollisionViewSettings collisionView;
    TriggerViewSettings triggerView;
    bool turboMode;
    bool moveLinkActive;
    bool stateShareLoadActive;
};

TransientSettings& getTransientSettings();

}  // namespace dusk
