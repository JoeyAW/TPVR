# Android Shell

This directory contains Dusklight's Android shell built on top of Borealis.

## Prerequisites

- Android SDK with Platform 37 installed (`ANDROID_HOME`)
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
```

This build produces `build/android-arm64/libmain.so`

## Build APK

```bash
cd platforms/android
./gradlew :app:assembleDebug
```

Output APK:

- `app/build/outputs/apk/debug/app-arm64-v8a-debug.apk`

Aurora needs a hardware-backed graphics adapter. If an AVD has GPU
acceleration disabled, launch it with `-gpu host`.

## Launch With Runtime Args (adb)

You can pass command-line args through the activity intent:

```bash
adb shell am start -n com.joeyaw.tpvr/.DuskActivity \
  --es borealis_args "--backend vulkan"
```

Supported extras:

- `borealis_args`: single shell-like argument string
- `borealis_argv`: string-array argv

The legacy `dusk_args` and `dusk_argv` names remain accepted during the shell
transition.
