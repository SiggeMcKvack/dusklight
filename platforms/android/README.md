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

## Build Native Libraries

```bash
cmake --preset android-arm64
cmake --build --preset android-arm64
```

This build produces `build/android-arm64/libmain.so`, plus the shared objects needed for custom GPU driver support (`libdusk_vulkan_shim.so` and the libadrenotools hook libraries listed in `build/android-arm64/dusk-android.properties`). Gradle stages all of them into the APK.

## Custom GPU Drivers

On arm64 devices the Video settings tab offers a "GPU Driver" option. It accepts the same
driver packages as other emulators (a `.zip` containing `meta.json` and the driver `.so`,
e.g. Mesa Turnip builds) and loads them through [libadrenotools](https://github.com/bylaws/libadrenotools).
Packages are stored under the app's internal files directory in `gpu_drivers/<id>/`.

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
adb shell am start -n dev.twilitrealm.dusk/.DuskActivity \
  --es borealis_args "--backend vulkan"
```

Supported extras:

- `borealis_args`: single shell-like argument string
- `borealis_argv`: string-array argv

The legacy `dusk_args` and `dusk_argv` names remain accepted during the shell
transition.
