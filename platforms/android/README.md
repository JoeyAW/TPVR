# Android Shell

This directory contains a minimal SDLActivity-based Android app wrapper for Dusklight.

## Prerequisites

- Android SDK installed (`ANDROID_HOME`)
- Android NDK version used by CMake presets (`ANDROID_NDK_VERSION`)
- JDK 17+

Example:

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_VERSION="29.0.14206865"
export JAVA_HOME="/usr/lib/jvm/java-17-openjdk"
```

### NDK toolchain fix required for the native build (2026-09-16)

Without this, `cmake --build --preset android-arm64`/`android-x86_64` fails
compiling aurora's `nod` Rust dependency (transitively, its bzip2-sys/
liblzma-sys/zstd-sys build scripts) with:

```
error: Unversioned target triples are not supported!
```

Root cause (confirmed by tracing the actual generated build command, not
guessed): corrosion, the CMake<->Rust bridge `nod`/`nod-ffi` uses, always
invokes Rust's C build scripts with the bare, unversioned NDK `clang.exe`
(no `--target=...<API level>` suffix), and this can't be fixed from
CMakeLists.txt -- see that file's own "ANDROID NOTE" comment (added right
after `add_subdirectory(extern/aurora)`) for the two approaches that were
tried and confirmed NOT to work.

Fix: drop a `clang.cfg` and `clang++.cfg` file next to the NDK's own
`clang.exe`/`clang++.exe` (same directory), each containing:

```
-D__ANDROID_MIN_SDK_VERSION__=28
```

On Windows, that directory is (adjust the NDK version to yours):

```
%ANDROID_HOME%\ndk\<version>\toolchains\llvm\prebuilt\windows-x86_64\bin\
```

Clang auto-loads a config file matching its own invoked name -- since
corrosion invokes it as bare `clang`/`clang++` (not a target-prefixed
name), only `clang.cfg`/`clang++.cfg` gets picked up automatically, not a
target-specific variant. This is a per-machine NDK setup step, not
something tracked by this repo -- redo it after reinstalling or upgrading
the NDK. Change `28` if your `ANDROID_PLATFORM` (CMakePresets.json's
`android-base` preset) ever changes from `android-28`.

## Build Native Libraries

```bash
cmake --preset android-arm64
cmake --build --preset android-arm64

cmake --preset android-x86_64
cmake --build --preset android-x86_64
```

These builds produce:

- `build/android-arm64/Binaries/libmain.so`
- `build/android-x86_64/Binaries/libmain.so`

## Stage Libraries Into APK Project

```bash
./android/scripts/stage-jni-libs.sh
```

This copies:

- `libmain.so` -> `android/app/src/main/jniLibs/arm64-v8a/`
- `libmain.so` -> `android/app/src/main/jniLibs/x86_64/`

## Refresh SDL Java Shim (Optional)

If you update SDL and want to refresh the embedded Java shim files:

```bash
./android/scripts/sync-sdl-java.sh
```

## Build APK

```bash
cd android
./gradlew :app:assembleDebug
```

Output APK:

- `android/app/build/outputs/apk/debug/app-debug.apk`

## Launch With Runtime Args (adb)

You can pass command-line args through the activity intent:

```bash
adb shell am start -n dev.twilitrealm.dusk/.DuskActivity \
  --es dusk_args "--backend vulkan"
```

Supported extras:

- `dusk_args`: single shell-like argument string
- `dusk_argv`: string-array argv
