# Android and RetroArch

## Building

You need the NDK and the SDK build-tools; Gradle isn't used.

```sh
git clone -b release-3.4.20 --depth 1 https://github.com/libsdl-org/SDL.git third_party/SDL
git clone -b n7.1.1 --depth 1 https://git.ffmpeg.org/ffmpeg.git third_party/ffmpeg   # movies (optional)
tools/android/build_ffmpeg.sh third_party/ffmpeg third_party/ffmpeg-android
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android --target apk typex2_libretro
```

Name both targets: the default target doesn't rebuild the core. The build
also keeps an unstripped `typex2_libretro_android.so.sym` for the profiler.

## Games on the device

- **Folders.** Each game lives in its own folder named `<Game>.x2`, for
  example `ROMs/arcade/The King of Fighters XIII.x2/game.exe`.
- **`x2.env`.** A file in the game folder holds `KEY=VALUE` lines. These set
  `X2_*` switches, such as a pad layout or `X2_FPS=1`.
- **Logs** go to logcat with the tag `x2`.

## The standalone app

- **Install:** `adb install -r build-android/typex2.apk`.
- **What it is:** one app, **Type X2** (`com.typex2.recomp`), holding every
  game. It asks for "All files access" the first time it reads shared
  storage.

## The RetroArch core

1. Copy `typex2_libretro_android.so` to `RetroArch/downloads/` on shared
   storage.
2. In RetroArch: *Load Core → Install or Restore a Core*, then pick the file.
   Android only loads cores from RetroArch's own data folder.

Settings and limits:
- **Video driver** `gl` (OpenGL ES 3.2). Vulkan isn't supported.
- **Clock.** RetroArch's frame loop clocks the game, so pause, fast-forward
  and slow motion work.
- **Missing features.** No save states, rewind, run-ahead or reset.
- **One game per session.** After closing a game, restart RetroArch before
  loading another.

## ES-DE

1. Copy the `<system>` block of `tools/es-de/android/es_systems.xml` into
   `ES-DE/custom_systems/es_systems.xml`.
2. Copy `es_find_rules.xml` into `ES-DE/custom_systems/`.
3. On each Type X2 game, set "Alternative emulator".

The block replaces the built-in Arcade system. It keeps the MAME / FBNeo
commands and adds "Type X2 recomp (Standalone)" and "Type X2 (RetroArch)".
MAME stays the default, which is why step 3 is needed.

## Performance work on tile-based GPUs (Mali)

Each change was measured on the test tablet, a Samsung Galaxy Tab A11
(8 GB / 128 GB, Mali-G57 MC2; see [test devices](Hardware.md#test-devices)),
with `X2_AB`, which runs
both variants in the same session because the games pick random stages.

| Change | Effect |
|---|---|
| One frame in flight (core) | Largest win: late frames in heavy scenes went from 37-59 per 60 to 7-9. |
| Alpha test only in shader variants that need it | Keeps early-Z: SSF4 fights 15.0 → 11.6 ms GPU per frame. |
| fp16 pixel shaders | 2.5-4 % in fights, ~10 % in light scenes. |
| RGB565 for opaque DXT1 without S3TC | Half the upload size and memory. |
| Honour `SetThreadPriority` | HIGHEST / TIME_CRITICAL guest threads run on the big cores. |
| Deferred texture uploads past a per-frame budget | Fewer load pauses. |
| BGRA8 uploaded unswapped, swizzled when sampled; `UpdateTexture` from the source | BlazBlue's worst pause ~430 → ~230 ms. |
| Natives for hot routines (zlib, sprite RLE, fills) | Shorter loads. |

### Measuring

| Switch | Shows |
|---|---|
| `X2_PACELOG=1` | Every 2 s: retro_run intervals, the slowest guest frame ("guest work max"), the slowest replay. |
| `X2_PROF=1` `X2_PROF_OUT=<file>` | A sampling profile per second and thread. Symbolise the offsets against the `.sym` file. |
| `X2_GPUTIME=1` / `=2` | GPU time per frame / per pass. |
| `X2_CSPROF=1` | Lock contention. |

Measure only while the tablet is awake and unlocked: it throttles while
locked or dozing. Check with `dumpsys window | grep isKeyguardShowing`.
