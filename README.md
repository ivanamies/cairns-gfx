## SDL3 App From Source Minimal Example
This is a minimal example for building and using SDL3, SDL_Mixer, SDL_Image, and SDL_ttf_ from source 
using C++ and CMake. It also demonstrates setting up things like macOS/iOS
bundles.
See [src/main.cpp](src/main.cpp) for the code. 

### Building And Running
Are you a complete beginner? If so, read [this](https://github.com/Ravbug/sdl3-sample/wiki/Setting-up-your-computer)!
Otherwise, install CMake and your favorite compiler, and follow the commands below:
```sh
# You need to clone with submodules, otherwise SDL will not download.
git clone https://github.com/Ravbug/sdl3-sample --depth=1 --recurse-submodules
cd sdl3-sample
cmake -S . -B build
```
You can also use an init script inside [`config/`](config/). Then open the IDE project inside `build/` 
(If you had CMake generate one) and run!

### cairns-gfx: selecting the GPU backend (macOS)
Generate the Xcode project for a specific backend with `-DCAIRNS_GFX_BACKEND=metal|vulkan`:
```sh
cmake -B build/metal -G Xcode -DCAIRNS_GFX_BACKEND=metal    # Metal
cmake -B build/vk    -G Xcode -DCAIRNS_GFX_BACKEND=vulkan   # Vulkan (MoltenVK; run via ./run_vk.sh)
```

### cairns-gfx: iOS device (Metal, via Xcode)
```sh
cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCAIRNS_GFX_BACKEND=metal -B build/ios -S .
open build/ios/sdl-min.xcodeproj
```
Pick the connected iPhone in Xcode's destination selector and Cmd-R.

### cairns-gfx: iOS Simulator (Metal, headless CLI)
Boot a sim, build for it, install, launch. Bundle id = `com.ravbug.sdl3-sample`.
```sh
# project already generated above

SIM=$(xcrun simctl list devices booted | awk -F'[()]' '/iPhone/ {print $2; exit}')
[ -z "$SIM" ] && SIM=$(xcrun simctl list devices available | awk -F'[()]' '/iPhone 16 Pro/ {print $2; exit}')
xcrun simctl boot "$SIM" 2>/dev/null

xcodebuild -project build/ios/sdl-min.xcodeproj \
           -scheme sdl-min -configuration Debug \
           -destination "platform=iOS Simulator,id=$SIM" build

xcrun simctl install "$SIM" build/ios/Debug/sdl-min.app
xcrun simctl launch  "$SIM" com.ravbug.sdl3-sample

# crash logs:
ls -1t ~/Library/Logs/DiagnosticReports/sdl-min*.ips | head -3
```
Note: `CMakeLists.txt` must link Apple frameworks via `-framework <Name>`
(not `find_library(... REQUIRED)`), otherwise the linker bakes in the
iPhoneOS device SDK path and fails when linking the Simulator binary.

### cairns-gfx: Android (Vulkan via SDL3)

Android does **not** drive the root `CMakeLists.txt` directly. The build is
controlled by a gradle project that lives inside the SDL submodule:
```
third_party/SDL/android-project/
```
Its `build.gradle` calls back up into our root `CMakeLists.txt` via
`../../../../CMakeLists.txt` with `-DCAIRNS_GFX_BACKEND=vulkan` and
`MOBILE_ASSETS_DIR=<...>/app/src/main/assets`. SDK path is set in
`third_party/SDL/android-project/local.properties` (not checked in).

Constraints baked into the gradle config:
- `minSdkVersion 33` (need Vulkan loader symbols visible).
- `ANDROID_STL=c++_static`.
- `android:largeHeap="true"` in the SDL `AndroidManifest.xml`.
- Package / activity: `org.libsdl.app/.SDLActivity`.

#### Stage assets first (required for both device and emulator)
GLBs and shaders MUST be copied into the gradle assets dir before building, or
the APK won't ship them:
```sh
cp media/*.glb third_party/SDL/android-project/app/src/main/assets/
cp build/<...>/shaders/*.spv third_party/SDL/android-project/app/src/main/assets/
```

#### Real device
1. Enable Developer Options + USB debugging on the phone, plug in USB.
2. Confirm `adb` sees it:
   ```sh
   adb devices                          # should list the device, "device" not "unauthorized"
   ```
3. Build + install + launch:
   ```sh
   cd third_party/SDL/android-project
   ./gradlew assembleDebug              # or assembleRelease
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   adb shell am start -n org.libsdl.app/.SDLActivity
   adb logcat -v color SDL:V cairns:V '*:S'
   ```
4. Multi-device: pass `-s <serial>` to every `adb` call.

#### Emulator
1. Boot an AVD:
   ```sh
   ~/Library/Android/sdk/emulator/emulator -list-avds
   ~/Library/Android/sdk/emulator/emulator -avd <name> -no-snapshot-load &
   until adb shell getprop sys.boot_completed | grep -q 1; do sleep 4; done
   adb devices                          # should show "emulator-5554  device"
   ```
2. Same gradle + adb sequence as the device path:
   ```sh
   cd third_party/SDL/android-project
   ./gradlew assembleDebug
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   adb shell am start -n org.libsdl.app/.SDLActivity
   adb logcat -v color SDL:V cairns:V '*:S'
   ```
3. Emulators ARM64 are best (`arm64-v8a` AVDs run our arm64 code natively on
   Apple Silicon hosts; x86_64 AVDs need an x86_64 ABI build).

#### Rebuild after C++ changes
Gradle's externalNativeBuild caches CMake state under
`third_party/SDL/android-project/app/.cxx/`. Incremental rebuilds work; nuke
only when CMake gets confused:
```sh
rm -rf third_party/SDL/android-project/app/.cxx
rm -rf third_party/SDL/android-project/app/build
```

## Supported Platforms
I have tested the following:
| Platform | Architecture | Generator |
| --- | --- | --- |
| macOS | x86_64, arm64 | Xcode |
| iOS | x86_64, arm64 | Xcode |
| tvOS | x86_64, arm64 | Xcode |
| visionOS* | arm64 | Xcode |
| Windows | x86_64, arm64 | Visual Studio |
| Linux | x86_64, arm64 | Ninja, Make |
| Web* | wasm | Ninja, Make |
| Android* | x86, x64, arm, arm64 | Ninja via Android Studio |

*See further instructions in [`config/`](config/)

Note: UWP support was [removed from SDL3](https://github.com/libsdl-org/SDL/pull/10731) during its development. For historical reasons, you can get a working UWP sample via this commit: [df270da](https://github.com/Ravbug/sdl3-sample/tree/df270daa8d6d48426e128e50c73357dfdf89afbf)

## Updating SDL
Just update the submodule:
```sh
cd SDL
git pull
cd ..

cd SDL_ttf
git pull
```
You don't need to use a submodule, you can also copy the source in directly. This
repository uses a submodule to keep its size to a minimum.

## Reporting issues
Is something not working? Create an Issue or send a Pull Request on this repository!
