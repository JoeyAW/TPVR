#include "dusk/settings.h"
#include "dusk/config.hpp"
#include <aurora/aurora.h>

namespace dusk {

UserSettings g_userSettings = {
    .video = {
        .enableFullscreen {"video.enableFullscreen", false},
        .enableVsync {"video.enableVsync", true},
        .lockAspectRatio {"video.lockAspectRatio", false},
        .enableFpsOverlay {"game.enableFpsOverlay", false},
        .fpsOverlayCorner {"game.fpsOverlayCorner", 0},
        .maxFrameRate {"video.maxFrameRate", 240},
        .rememberWindowSize {"video.rememberWindowSize", false},
        .lastWindowWidth {"video.lastWindowWidth", 0},
        .lastWindowHeight {"video.lastWindowHeight", 0},
    },

    .audio = {
        .masterVolume {"audio.masterVolume", 60},
        .mainMusicVolume {"audio.mainMusicVolume", 100},
        .subMusicVolume {"audio.subMusicVolume", 100},
        .soundEffectsVolume {"audio.soundEffectsVolume", 100},
        .fanfareVolume {"audio.fanfareVolume", 100},
        .enableReverb {"audio.enableReverb", true},
        .enableHrtf {"audio.enableHrtf", false},
        .menuSounds {"audio.menuSounds", true},
    },

    .game = {
        .language { "game.language", GameLanguage::English },

        // Quality of Life
        .enableQuickTransform {"game.enableQuickTransform", false},
        .hideTvSettingsScreen {"game.hideTvSettingsScreen", true},
        .biggerWallets {"game.biggerWallets", false},
        .noReturnRupees {"game.noReturnRupees", false},
        .disableRupeeCutscenes {"game.disableRupeeCutscenes", false},
        .noSwordRecoil {"game.noSwordRecoil", false},
        .damageMultiplier {"game.damageMultiplier", 1},
        .noHeartDrops {"game.noHeartDrops", false},
        .instantDeath {"game.instantDeath", false},
        .fastClimbing {"game.fastClimbing", false},
        .noMissClimbing {"game.noMissClimbing", false},
        .fastTears {"game.fastTears", false},
        .no2ndFishForCat {"game.no2ndFishForCat", false},
        .buttonFishing {"game.buttonFishing", false},
        .instantSaves {"game.instantSaves", false},
        .instantText {"game.instantText", false},
        .sunsSong {"game.sunsSong", false},
        .autoSave {"game.autoSave", false},
        .enhancedMapMenus {"game.enhancedMapMenus", false},

        // Preferences
        .enableMirrorMode {"game.enableMirrorMode", false},
        .minimalHUD {"game.minimalHUD", false},
        .hudScale {"game.hudScale", 1.0f},
        .pauseOnFocusLost {"game.pauseOnFocusLost", false},
        .enableLinkDollRotation {"game.enableLinkDollRotation", false},
        .enableAchievementToasts {"game.enableAchievementToasts", true},
        .enableControllerToasts {"game.enableControllerToasts", true},
        .enableDiscordPresence {"game.enableDiscordPresence", true},
        .menuScalingMode {"game.menuScalingMode", MenuScaling::Wii},

        // Graphics
        .bloomMode {"game.bloomMode", BloomMode::Dusk},
        .bloomMultiplier {"game.bloomMultiplier", 1.0f},
        .depthOfFieldMode{"game.depthOfFieldMode", DepthOfFieldMode::Dusk},
        .disableWaterRefraction {"game.disableWaterRefraction", false},
        .enableTextureReplacements {"game.enableTextureReplacements", true},
        .enableFrameInterpolation {"game.enableFrameInterpolation", FrameInterpMode::Off},
        .internalResolutionScale {"game.internalResolutionScale", 0},
        .shadowResolutionMultiplier {"game.shadowResolutionMultiplier", 1},
        .resampler {"game.resampler", Resampler::Bilinear},
        .enableMapBackground {"game.enableMapBackground", true},
        .disableCutscenePillarboxing {"game.disableCutscenePillarboxing", false},
        .vrDesktopMirror {"game.vrDesktopMirror", true},
        .vrHideBody {"game.vrHideBody", false},
        .vrThirdPerson {"game.vrThirdPerson", false},
        .vrExperimentalCutsceneFirstPerson {"game.vrExperimentalCutsceneFirstPerson", false},
        .vrSwapSwordShieldHands {"game.vrSwapSwordShieldHands", false},
        // Key string deliberately kept as the original "game.vrSwapGripMirrorAxis"
        // (not renamed to match the C++ member) so a value the user already
        // tuned before the sword/shield split (2026-08-20) carries over
        // instead of silently resetting to the default.
        .vrSwapSwordGripMirrorAxis {"game.vrSwapGripMirrorAxis", 0},
        .vrSwapShieldGripMirrorAxis {"game.vrSwapShieldGripMirrorAxis", 0},
        .vrSwapSwordExtraFlipAxis {"game.vrSwapSwordExtraFlipAxis", -1},
        .vrSwapShieldExtraFlipAxis {"game.vrSwapShieldExtraFlipAxis", -1},
        .vrSwapSwordGripOffsetX {"game.vrSwapSwordGripOffsetX", 0.0f},
        .vrSwapSwordGripOffsetY {"game.vrSwapSwordGripOffsetY", 0.0f},
        .vrSwapSwordGripOffsetZ {"game.vrSwapSwordGripOffsetZ", 0.0f},
        .vrSwapShieldGripOffsetX {"game.vrSwapShieldGripOffsetX", 0.0f},
        .vrSwapShieldGripOffsetY {"game.vrSwapShieldGripOffsetY", 0.0f},
        .vrSwapShieldGripOffsetZ {"game.vrSwapShieldGripOffsetZ", 0.0f},
        // RESET TO 1.0, 2026-08-20 (was 2.0, confirmed correct on Virtual
        // Desktop 2026-08-16): that tuning was specific to submitting via
        // the plain non-SRGB native swapchain format, which
        // vr_xr_submit.hpp's createSwapchain() no longer prefers for these
        // runtimes -- see its own comment (OpenXR issue #467 investigation)
        // for why an SRGB-tagged swapchain is now believed to be the more
        // correct choice for a spec-faithful compositor like Meta's.
        // **CONFIRMED CORRECT AT 1.0 ON VIRTUAL DESKTOP, same day** -- user:
        // "the game looks correct at 100 percent." The SRGB-submission fix
        // alone was sufficient; no further compensation curve needed.
        // Meta Link not yet separately tested with this same value -- if
        // it's ever reported off there, retune via the live "VR Gamma
        // Compensation" slider rather than assuming 1.0 is automatically
        // right for every runtime this setting covers. Applies to VD/Meta
        // Link/etc. only, NOT SteamVR (which keeps its own independently-
        // tuned, decoupled baseline -- see vr_xr_submit.hpp's
        // effectiveGammaExponent()/isSteamVr_).
        .vrGammaCompensation {"game.vrGammaCompensation", 1.0f},
        // THIRD REVERSAL, 2026-08-20 (same day as the second): briefly set
        // back to ~0.4545 (1.0/2.2) earlier this session after the user
        // reconsidered and said the original brightening baseline read
        // SteamVR's colors correctly all along -- then, immediately after
        // the same-day createSwapchain() SRGB-preference reorder (see
        // vr_xr_submit.hpp) and confirming Virtual Desktop looks correct at
        // 1.0/100% with zero compensation, explicit follow-up: "What's the
        // steamvr compiled default? It should be 1.0 or 100%, not 45%."
        // Set to 1.0 (no compensation) per that direct instruction. This
        // value has now flipped three times in one project (section 6 ->
        // 2026-08-16 "undersaturated" -> this session's "correct after
        // all" -> this same-day reversal back to 1.0) -- if it's ever
        // reported wrong again, get a real side-by-side against the
        // known-correct desktop mirror before changing it a fourth time,
        // not another memory-based impression.
        .vrGammaCompensationSteamVr {"game.vrGammaCompensationSteamVr", 1.0f},

        // Audio
        .noLowHpSound {"game.noLowHpSound", false},
        .midnasLamentNonStop {"game.midnasLamentNonStop", false},

        // Input
        .enableGyroAim {"game.enableGyroAim", false},
        .enableGyroRollgoal {"game.enableGyroRollgoal", false},
        .gyroSensitivityX {"game.gyroSensitivityX", 1.0f},
        .gyroSensitivityY {"game.gyroSensitivityY", 1.0f},
        .gyroSensitivityRollgoal {"game.gyroSensitivityRollgoal", 1.0f},
        .gyroSmoothing {"game.gyroSmoothing", 0.65f},
        .gyroDeadband {"game.gyroDeadband", 0.04f},
        .gyroInvertPitch {"game.gyroInvertPitch", false},
        .gyroInvertYaw {"game.gyroInvertYaw", false},
        .enableMouseCamera {"game.enableMouseCamera", false},
        .enableMouseAim {"game.enableMouseAim", false},
        .mouseAimSensitivity {"game.mouseAimSensitivity", 1.0f},
        .mouseCameraSensitivity {"game.mouseCameraSensitivity", 1.0f},
        .invertMouseY {"game.invertMouseY", false},
        .freeCamera {"game.freeCamera", false},
        .enableTouchControls {"game.enableTouchControls", false},
        .touchTargeting {"game.touchTargeting", TouchTargeting::Hybrid},
        .enableMenuPointer {"game.enableMenuPointer", true},
        .touchControlsLayout {"game.touchControlsLayout", ui::ControlLayout{}},
        .invertCameraXAxis {"game.invertCameraXAxis", false},
        .invertCameraYAxis {"game.invertCameraYAxis", false},
        .invertFirstPersonXAxis {"game.invertFirstPersonXAxis", false},
        .invertFirstPersonYAxis {"game.invertFirstPersonYAxis", false},
        .invertAirSwimX {"game.invertAirSwimX", false},
        .invertAirSwimY {"game.invertAirSwimY", false},
        .freeCameraXSensitivity {"game.freeCameraXSensitivity", 1.0f},
        .freeCameraYSensitivity {"game.freeCameraYSensitivity", 1.0f},
        .touchCameraXSensitivity {"game.touchCameraXSensitivity", 1.0f},
        .touchCameraYSensitivity {"game.touchCameraYSensitivity", 1.0f},
        .debugFlyCam {"game.debugFlyCam", false},
        .debugFlyCamLockEvents {"game.debugFlyCamLockEvents", true},
        .allowBackgroundInput {"game.allowBackgroundInput", true},
        .enableLED {
            ConfigVar<bool>{"game.enableLED_port0", true},
            ConfigVar<bool>{"game.enableLED_port1", true},
            ConfigVar<bool>{"game.enableLED_port2", true},
            ConfigVar<bool>{"game.enableLED_port3", true},
        },
        .swapDirectSelect {"game.swapDirectSelect", false},

        // Cheats
        .infiniteHearts {"game.infiniteHearts", false},
        .infiniteArrows {"game.infiniteArrows", false},
        .infiniteSeeds {"game.infiniteSeeds", false},
        .infiniteBombs {"game.infiniteBombs", false},
        .infiniteOil {"game.infiniteOil", false},
        .infiniteOxygen {"game.infiniteOxygen", false},
        .infiniteRupees {"game.infiniteRupees", false},
        .enableIndefiniteItemDrops {"game.enableIndefiniteItemDrops", false},
        .moonJump {"game.moonJump", false},
        .superClawshot {"game.superClawshot", false},
        .alwaysGreatspin {"game.alwaysGreatspin", false},
        .enableFastIronBoots {"game.enableFastIronBoots", false},
        .canTransformAnywhere {"game.canTransformAnywhere", false},
        .fastRoll {"game.fastRoll", false},
        .fastSpinner {"game.fastSpinner", false},
        .armorRupeeDrain {"game.armorRupeeDrain", MagicArmorMode::NORMAL},
        .invincibleEnemies {"game.invincibleEnemies", false},

        // Technical
        .restoreWiiGlitches {"game.restoreWiiGlitches", false},

        // Controls
        .enableTurboKeybind {"game.enableTurboKeybind", false},
        .enableResetKeybind {"game.enableResetKeybind", false},

        // Tools
        .speedrunMode {"game.speedrunMode", false},
        .liveSplitEnabled {"game.liveSplitEnabled", false},
        .showSpeedrunRTATimer {"game.showSpeedrunRTATimer", true},
        .recordingMode {"game.recordingMode", false},
        .removeQuestMapMarkers {"game.removeQuestMapMarkers", false},
        .showInputViewer {"game.showInputViewer", false},
        .showInputViewerGyro {"game.showInputViewerGyro", false}
    },

    .backend = {
        .isoPath {"backend.isoPath", ""},
        .isoVerification {"backend.isoVerification", DiscVerificationState::Unknown},
        .graphicsBackend {"backend.graphicsBackend", "auto"},
        .skipPreLaunchUI {"backend.skipPreLaunchUI", false},
        .wasPresetChosen {"backend.wasPresetChosen", false},
        .checkForUpdates {"backend.checkForUpdates", true},
        .cardFileType {"backend.cardFileType", static_cast<int>(CARD_GCIFOLDER)},
        .enableAdvancedSettings {"backend.enableAdvancedSettings", false},
    },

    // Not sure if there's a better way to declare this
    .actionBindings = {
        .firstPersonCamera {
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .callMidna {
            ActionBindConfigVar{"actionBindings.callMidna_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openMapScreen {
            ActionBindConfigVar{"actionBindings.openMapScreen_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleMinimap {
            ActionBindConfigVar{"actionBindings.toggleMinimap_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openDusklightMenu {
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .turboSpeedButton {
            ActionBindConfigVar{"actionBindings.turboButton_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port3", PAD_NATIVE_BUTTON_INVALID},
        },
    }
};

UserSettings& getSettings() {
    return g_userSettings;
}

void registerSettings() {
    // Video
    Register(g_userSettings.video.enableFullscreen);
    Register(g_userSettings.video.enableVsync);
    Register(g_userSettings.video.lockAspectRatio);
    Register(g_userSettings.video.enableFpsOverlay);
    Register(g_userSettings.video.fpsOverlayCorner);
    Register(g_userSettings.video.maxFrameRate);
    Register(g_userSettings.video.rememberWindowSize);
    Register(g_userSettings.video.lastWindowWidth);
    Register(g_userSettings.video.lastWindowHeight);

    // Audio
    Register(g_userSettings.audio.masterVolume);
    Register(g_userSettings.audio.mainMusicVolume);
    Register(g_userSettings.audio.subMusicVolume);
    Register(g_userSettings.audio.soundEffectsVolume);
    Register(g_userSettings.audio.fanfareVolume);
    Register(g_userSettings.audio.enableReverb);
    Register(g_userSettings.audio.enableHrtf);
    Register(g_userSettings.audio.menuSounds);

    // Game
    Register(g_userSettings.game.language);
    Register(g_userSettings.game.enableQuickTransform);
    Register(g_userSettings.game.hideTvSettingsScreen);
    Register(g_userSettings.game.biggerWallets);
    Register(g_userSettings.game.noReturnRupees);
    Register(g_userSettings.game.disableRupeeCutscenes);
    Register(g_userSettings.game.noSwordRecoil);
    Register(g_userSettings.game.damageMultiplier);
    Register(g_userSettings.game.noHeartDrops);
    Register(g_userSettings.game.instantDeath);
    Register(g_userSettings.game.fastClimbing);
    Register(g_userSettings.game.fastTears);
    Register(g_userSettings.game.no2ndFishForCat);
    Register(g_userSettings.game.buttonFishing);
    Register(g_userSettings.game.instantSaves);
    Register(g_userSettings.game.instantText);
    Register(g_userSettings.game.sunsSong);
    Register(g_userSettings.game.autoSave);
    Register(g_userSettings.game.enhancedMapMenus);
    Register(g_userSettings.game.enableMirrorMode);
    Register(g_userSettings.game.invertCameraXAxis);
    Register(g_userSettings.game.invertCameraYAxis);
    Register(g_userSettings.game.invertFirstPersonXAxis);
    Register(g_userSettings.game.invertFirstPersonYAxis);
    Register(g_userSettings.game.invertAirSwimX);
    Register(g_userSettings.game.invertAirSwimY);
    Register(g_userSettings.game.freeCameraXSensitivity);
    Register(g_userSettings.game.freeCameraYSensitivity);
    Register(g_userSettings.game.touchCameraXSensitivity);
    Register(g_userSettings.game.touchCameraYSensitivity);
    Register(g_userSettings.game.minimalHUD);
    Register(g_userSettings.game.hudScale);
    Register(g_userSettings.game.pauseOnFocusLost,
        [](const bool& value, const bool&) { aurora_set_pause_on_focus_lost(value); });
    Register(g_userSettings.game.enableDiscordPresence);
    Register(g_userSettings.game.bloomMode);
    Register(g_userSettings.game.bloomMultiplier);
    Register(g_userSettings.game.depthOfFieldMode);
    Register(g_userSettings.game.disableWaterRefraction);
    Register(g_userSettings.game.enableTextureReplacements);
    Register(g_userSettings.game.internalResolutionScale);
    Register(g_userSettings.game.resampler);
    Register(g_userSettings.game.shadowResolutionMultiplier);
    Register(g_userSettings.game.enableMapBackground);
    Register(g_userSettings.game.disableCutscenePillarboxing);
    Register(g_userSettings.game.vrDesktopMirror);
    Register(g_userSettings.game.vrHideBody);
    Register(g_userSettings.game.vrThirdPerson);
    Register(g_userSettings.game.vrExperimentalCutsceneFirstPerson);
    Register(g_userSettings.game.vrSwapSwordShieldHands);
    Register(g_userSettings.game.vrSwapSwordGripMirrorAxis);
    Register(g_userSettings.game.vrSwapShieldGripMirrorAxis);
    Register(g_userSettings.game.vrSwapSwordExtraFlipAxis);
    Register(g_userSettings.game.vrSwapShieldExtraFlipAxis);
    Register(g_userSettings.game.vrSwapSwordGripOffsetX);
    Register(g_userSettings.game.vrSwapSwordGripOffsetY);
    Register(g_userSettings.game.vrSwapSwordGripOffsetZ);
    Register(g_userSettings.game.vrSwapShieldGripOffsetX);
    Register(g_userSettings.game.vrSwapShieldGripOffsetY);
    Register(g_userSettings.game.vrSwapShieldGripOffsetZ);
    Register(g_userSettings.game.vrGammaCompensation);
    Register(g_userSettings.game.vrGammaCompensationSteamVr);
    Register(g_userSettings.game.enableFastIronBoots);
    Register(g_userSettings.game.canTransformAnywhere);
    Register(g_userSettings.game.fastRoll);
    Register(g_userSettings.game.armorRupeeDrain);
    Register(g_userSettings.game.restoreWiiGlitches);
    Register(g_userSettings.game.enableLinkDollRotation);
    Register(g_userSettings.game.enableAchievementToasts);
    Register(g_userSettings.game.enableControllerToasts);
    Register(g_userSettings.game.noMissClimbing);
    Register(g_userSettings.game.noLowHpSound);
    Register(g_userSettings.game.midnasLamentNonStop);
    Register(g_userSettings.game.enableTurboKeybind);
    Register(g_userSettings.game.enableResetKeybind);
    Register(g_userSettings.game.speedrunMode);
    Register(g_userSettings.game.liveSplitEnabled);
    Register(g_userSettings.game.showSpeedrunRTATimer);
    Register(g_userSettings.game.recordingMode);
    Register(g_userSettings.game.menuScalingMode);
    Register(g_userSettings.game.removeQuestMapMarkers);
    Register(g_userSettings.game.showInputViewer);
    Register(g_userSettings.game.showInputViewerGyro);
    Register(g_userSettings.game.fastSpinner);
    Register(g_userSettings.game.infiniteHearts);
    Register(g_userSettings.game.infiniteArrows);
    Register(g_userSettings.game.infiniteSeeds);
    Register(g_userSettings.game.infiniteBombs);
    Register(g_userSettings.game.infiniteOil);
    Register(g_userSettings.game.infiniteOxygen);
    Register(g_userSettings.game.infiniteRupees);
    Register(g_userSettings.game.enableIndefiniteItemDrops);
    Register(g_userSettings.game.moonJump);
    Register(g_userSettings.game.superClawshot);
    Register(g_userSettings.game.alwaysGreatspin);
    Register(g_userSettings.game.invincibleEnemies);
    Register(g_userSettings.game.enableFrameInterpolation);
    Register(g_userSettings.game.enableGyroAim);
    Register(g_userSettings.game.enableGyroRollgoal);
    Register(g_userSettings.game.gyroSensitivityX);
    Register(g_userSettings.game.gyroSensitivityY);
    Register(g_userSettings.game.gyroSensitivityRollgoal);
    Register(g_userSettings.game.gyroDeadband);
    Register(g_userSettings.game.gyroSmoothing);
    Register(g_userSettings.game.gyroInvertPitch);
    Register(g_userSettings.game.gyroInvertYaw);
    Register(g_userSettings.game.enableMouseCamera);
    Register(g_userSettings.game.enableMouseAim);
    Register(g_userSettings.game.mouseAimSensitivity);
    Register(g_userSettings.game.mouseCameraSensitivity);
    Register(g_userSettings.game.invertMouseY);
    Register(g_userSettings.game.freeCamera);
    Register(g_userSettings.game.enableTouchControls);
    Register(g_userSettings.game.touchTargeting);
    Register(g_userSettings.game.enableMenuPointer);
    Register(g_userSettings.game.touchControlsLayout);
    Register(g_userSettings.game.debugFlyCam);
    Register(g_userSettings.game.debugFlyCamLockEvents);
    Register(g_userSettings.game.allowBackgroundInput);
    Register(g_userSettings.game.enableLED[0]);
    Register(g_userSettings.game.enableLED[1]);
    Register(g_userSettings.game.enableLED[2]);
    Register(g_userSettings.game.enableLED[3]);
    Register(g_userSettings.game.swapDirectSelect);

    Register(g_userSettings.backend.isoPath);
    Register(g_userSettings.backend.isoVerification);
    Register(g_userSettings.backend.graphicsBackend);
    Register(g_userSettings.backend.skipPreLaunchUI);
    Register(g_userSettings.backend.wasPresetChosen);
    Register(g_userSettings.backend.checkForUpdates);
    Register(g_userSettings.backend.cardFileType);
    Register(g_userSettings.backend.enableAdvancedSettings);

    Register(g_userSettings.actionBindings.firstPersonCamera[0]);
    Register(g_userSettings.actionBindings.firstPersonCamera[1]);
    Register(g_userSettings.actionBindings.firstPersonCamera[2]);
    Register(g_userSettings.actionBindings.firstPersonCamera[3]);
    Register(g_userSettings.actionBindings.callMidna[0]);
    Register(g_userSettings.actionBindings.callMidna[1]);
    Register(g_userSettings.actionBindings.callMidna[2]);
    Register(g_userSettings.actionBindings.callMidna[3]);
    Register(g_userSettings.actionBindings.openMapScreen[0]);
    Register(g_userSettings.actionBindings.openMapScreen[1]);
    Register(g_userSettings.actionBindings.openMapScreen[2]);
    Register(g_userSettings.actionBindings.openMapScreen[3]);
    Register(g_userSettings.actionBindings.toggleMinimap[0]);
    Register(g_userSettings.actionBindings.toggleMinimap[1]);
    Register(g_userSettings.actionBindings.toggleMinimap[2]);
    Register(g_userSettings.actionBindings.toggleMinimap[3]);
    Register(g_userSettings.actionBindings.openDusklightMenu[0]);
    Register(g_userSettings.actionBindings.openDusklightMenu[1]);
    Register(g_userSettings.actionBindings.openDusklightMenu[2]);
    Register(g_userSettings.actionBindings.openDusklightMenu[3]);
    Register(g_userSettings.actionBindings.turboSpeedButton[0]);
    Register(g_userSettings.actionBindings.turboSpeedButton[1]);
    Register(g_userSettings.actionBindings.turboSpeedButton[2]);
    Register(g_userSettings.actionBindings.turboSpeedButton[3]);
}

// Transient settings

static TransientSettings g_transientSettings = {
    .collisionView = {
        .enableTerrainView = false,
        .enableWireframe = false,
        .enableAtView = false,
        .enableTgView = false,
        .enableCoView = false,
        .terrainViewOpacity = 50.0f,
        .colliderViewOpacity = 50.0f,
        .drawRange = 100.0f,
    },
    .skipFrameRateLimit = false,
};

TransientSettings& getTransientSettings() {
    return g_transientSettings;
}

}
