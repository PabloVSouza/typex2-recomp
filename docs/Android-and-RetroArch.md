# Android and RetroArch

## Building

The runtime needs the NDK; the app (the launcher, built with Tauri) also needs
the SDK, a JDK and Rust's `aarch64-linux-android` target.

```sh
git clone -b release-3.4.20 --depth 1 https://github.com/libsdl-org/SDL.git third_party/SDL
git clone -b n7.1.1 --depth 1 https://git.ffmpeg.org/ffmpeg.git third_party/ffmpeg   # movies (optional)
tools/android/build_ffmpeg.sh third_party/ffmpeg third_party/ffmpeg-android
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android --target x2app packs typex2_libretro
# the app: its game side comes from build-android/app (X2_APP_NATIVE to use another folder)
cd launcher && ANDROID_HOME=... NDK_HOME=$ANDROID_NDK npx tauri android build --apk --target aarch64 [--debug]
```

- **Targets.**
  - `x2app`: the app's game side, without games: `libmain.so`, `libSDL3.so`
    and SDL's Java activity, in `build-android/app`.
  - `packs`: one game pack per game, `build-android/packs/x2-<size>-<entry>.so`.
  - `typex2_libretro`: the core. Name it: the default target doesn't rebuild
    it. The build also keeps an unstripped `typex2_libretro_android.so.sym`
    for the profiler.
- **The Android project** is `launcher/src-tauri/gen/android`. Tauri rewrites
  its `app/build.gradle.kts` on every build, so the app's own Gradle settings
  live in the root `build.gradle.kts`.
- **Debug builds** are `com.typex2.recomp.dev`, installed next to the release
  app. Release builds are signed when `X2_KEYSTORE` and `X2_KEYSTORE_PASSWORD`
  are set (CI).

## Games on the device

- **Folders.** Each game lives in its own folder named `<Game>.x2`, for
  example `ROMs/arcade/The King of Fighters XIII.x2/game.exe`.
- **`x2.env`.** A file in the game folder holds `KEY=VALUE` lines. These set
  `X2_*` switches, such as a pad layout or `X2_FPS=1`. The app writes it from
  each game's settings.
- **Logs** go to logcat with the tag `x2`.

## The app

**Type X2** (`com.typex2.recomp`) is the launcher, built for Android. It
carries no game code: each game's code comes as a **game pack**, built on a
computer.

1. **Install the app** from the releases page (`typex2-android.apk`). On
   first start, allow "All files access": the app reads the ROMs folder and
   writes each game's `x2.env`. Coming from a Type X2 app older than 0.2.0, uninstall
   it first: the new app is signed with the release key, and Android won't
   install it over the old one (saves and settings are in the game folders).
2. **Make the packs** on a computer: in the launcher, Export → **Game packs
   for the Android app**, with the games you own.
3. **Send them:** Export → Last exports → **Send to device**, with the device
   connected (USB debugging on). Or copy the `x2-*.so` files to
   `TypeX2/packs` on the device's internal storage yourself.
4. **Open the app.** It installs the packs, finds the games in ES-DE's ROMs
   folder (`ROMs/arcade`, or ES-DE's own ROM directory setting) and lists
   them. Games it can't play yet are listed too, as *Not supported yet*.

- **A game's page:** Play, the FPS counter, the render scale, the on-screen
  touch pad (automatic, always, never), the pad layout and extra `X2_*`
  switches. The settings go into the game's `x2.env`.
- **From ES-DE** a game starts straight away, with the same settings: ES-DE
  launches the app's game activity (`es_find_rules.xml`).
- **How a game runs:** in its own process, in SDL's activity. `libmain`
  identifies the game from its executable and loads its pack from the app's
  private storage, the only place besides the APK Android loads code from.
- **Updating a game** after a runtime fix means sending a new pack, not
  reinstalling the app.

## The RetroArch core

1. Copy `typex2_libretro_android.so` to `RetroArch/downloads/` on shared
   storage, or let the launcher do it: Export → Last exports → **Send to
   device**, with the device connected (USB debugging on).
2. In RetroArch: *Load Core → Install or Restore a Core*, then pick the file.
   Android only loads cores from RetroArch's own data folder.

Settings and limits:
- **Video.** The core and the app render with Vulkan 1.3 (most devices from
  2022 on, with Android 13 or later). RetroArch switches to its `vulkan`
  driver by itself while "Allow driver switching" (Settings → Drivers) is
  on, the default. Older devices use OpenGL ES instead: the app switches by
  itself when Vulkan 1.3 is missing; in RetroArch, set the video driver to
  `gl` and put `X2_RENDERER=gl` in the game's `x2.env`. Both renderers draw
  the same; OpenGL ES is usually slower on the same GPU.
- **Clock.** RetroArch's frame loop clocks the game, so pause, fast-forward
  and slow motion work.
- **Save states** work within a run of the game, on the screen they were
  saved on ([Save states](Save-States.md)). No rewind or run-ahead. Reset (Quick
  Menu → Restart) restarts the game with Vulkan; with OpenGL it isn't
  supported.
- **Switching games.** Close a game (Quick Menu → Close Content) and load
  another, no restart needed. A closed game gives back almost all its memory;
  1-3 MB stays until RetroArch quits. Launched from ES-DE, RetroArch
  quits when the game closes (as it does with Settings → User Interface →
  "Quit on Close Content").
- **Latency.** One frame in flight: the game computes frame N+1 while
  RetroArch shows frame N, as on the cabinet's driver. `X2_PIPELINE=0` runs in
  lockstep (a frame less latency, drops when a frame's work exceeds 16.7 ms).
- **Remaps.** RetroArch's remaps work as with FinalBurn Neo: copy
  `remaps/FinalBurn Neo/FinalBurn Neo.rmp` to `remaps/Type X2/Type X2.rmp`
  (with `input_libretro_device_p1/p2 = "1"`) to share one layout. Button
  layouts per game: [Controls](Controls.md).

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
| ThinLTO for generated code (`X2_GEN_LTO`, on for Android) | Guest functions inline across the generated files. Climax's game thread on heavy stages: 15.3-17.5 → 10.8-13.2 ms a frame; SSF4 about 1.5 ms less. |
| Per-site cache for indirect calls (`X2_ICALL`); imports dispatched before the code table | Removes the 27 MB table lookup from virtual calls and from about a million import calls a second (Climax): `x2_lookup` 4.2 % of the game thread → gone. |

### Measuring

| Switch | Shows |
|---|---|
| `X2_PACELOG=1` | Every 2 s: retro_run intervals, the slowest guest frame ("guest work max"), the slowest replay. |
| `X2_PROF=1` `X2_PROF_OUT=<file>` | A sampling profile per second and thread. Symbolise the offsets against the `.sym` file. |
| `X2_PROF_SLOW=<ms>` | With `X2_PROF`: what the game thread did in each frame slower than `<ms>`. |
| `X2_GPUTIME=1` / `=2` | GPU time per frame / per pass. |
| `X2_CSPROF=1` | Lock contention. |

Measure only while the tablet is awake and unlocked: it throttles while
locked or dozing. Check with `dumpsys window | grep isKeyguardShowing`.
