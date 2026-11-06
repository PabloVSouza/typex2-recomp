# typex2-recomp

Static recompilation of Taito Type X2 games (SNK's KOF XII / XIII) to native
code, so they run on macOS and Android: as standalone apps (launched by ES-DE)
or as a RetroArch/libretro core.

**Status:** KOF XII, KOF XIII and KOF XIII Climax are playable on macOS and on
Android (arm64, GLES 3.2), standalone or in RetroArch. Rendering is D3D9 →
OpenGL 3.3 / GLES 3.2 (D3D9 shader bytecode and runtime HLSL both translated
to GLSL).

## How it works

```
game.exe ──x2recomp──► games/kof12/gen/*.c ──clang──► x2gen_kof12 (objects) ─┬─► x2-kof12 (desktop executable)
                                                                             ├─► libx2_kof12.so (Android app)
runtime/ (C): memory, threads, Win32/D3D9/DSound/DirectShow,                 └─► typex2_libretro (all games)
              JVS I/O, GL renderer; SDL3 or libretro platform layer
```

* **recompiler/** (C++20, capstone): recursive-descent discovery of every
  function, then x86 → C lifting, one C function per guest function. GPRs and
  flags are C locals (dead flags vanish under clang); x87 is modelled with
  doubles; SSE/MMX on unions.
* **runtime/**: 4 GiB guest address space, dispatcher for indirect calls,
  host implementations of every import (kernel32, user32, gdi32, advapi32
  crypto, ole32), Direct3D 9 / D3DX / DirectSound / DirectShow (FFmpeg movies)
  as host COM objects, an emulated JVS I/O board on COM2, and a GL renderer.
  The guest runs on its own threads and records frames; the platform thread
  replays them: `platform_sdl.c` (standalone) or `platform_libretro.c`
  (`retro_run`, which also clocks the guest).
* **tools/**: `arc` (unpack `gamediskimg.arc`), `kofarc` (list/extract KOF
  XIII `media.kof`, decrypting TXAC entries with the game's
  `TypeXmulti/txac` key), `lifttest` (differential test of the lifter against
  Unicorn), `retro_host` (minimal libretro frontend for testing the core),
  `android/` (APK packaging, FFmpeg cross-build), `es-de/` (ES-DE system
  files), `libretro/` (core info file).

## Build (macOS / desktop)

```sh
brew install capstone sdl3 ffmpeg     # unicorn too, for lifttest
cmake -S . -B build -G Ninja
cmake --build build --target x2recomp
build/recompiler/x2recomp "<game dir>/game.exe" games/kof12/gen
cmake -S . -B build && cmake --build build
```

This builds `build/x2-<game>` for every `games/<game>/gen` and the core
`build/typex2_libretro.dylib` (`-DX2_LIBRETRO=OFF` / `-DX2_STANDALONE=OFF`
to skip either).

## Run (desktop)

```sh
build/x2-kof12 "<KOF XII folder>"        # add -f for fullscreen, -v/-vv/-vvv for logs
build/tools/retro_host build/typex2_libretro.dylib "<game folder>"   # the core, without RetroArch
```

| Keys | |
|---|---|
| arrows / Z X C V | move / A B C D |
| 1, 2 | start P1, P2 |
| 5, 6 | coin P1, P2 |
| F2 / F1 | test / service |
| F9 | capture: log every draw of the next frame (shaders, textures, samplers) + x2_capture-N.png |
| F10 | FPS overlay (game fps, display fps, guest CPU ms per frame) |
| F11 / F12 / Esc | fullscreen / screenshot / quit |

Gamepads map to players 1–2 with the same button layout as the RetroArch core
(below, `X2_PAD` to change it); Back = coin, Back+Start quits (for ES-DE).
On touch screens an on-screen pad appears while no gamepad is connected
(`X2_TOUCH=1/0` forces it on/off).

## Android

Needs the NDK and SDK build-tools (no Gradle). Every game goes into one
app, **Type X2** (`com.typex2.recomp`), which identifies the game from its
`game.exe`.

```sh
git clone -b release-3.4.20 --depth 1 https://github.com/libsdl-org/SDL.git third_party/SDL
# movies (optional): a static, WMV/WMA-only FFmpeg
git clone -b n7.1.1 --depth 1 https://git.ffmpeg.org/ffmpeg.git third_party/ffmpeg
tools/android/build_ffmpeg.sh third_party/ffmpeg third_party/ffmpeg-android
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android --target apk typex2_libretro
adb install -r build-android/typex2.apk
```

* Games live in folders named `<Game>.x2` (e.g. `ROMs/arcade/The King of
  Fighters XIII.x2/game.exe`). The app asks for "All files access" the
  first time it reads shared storage.
* **ES-DE**: the games join the Arcade system. Copy the `<system>` block of
  `tools/es-de/android/es_systems.xml` into `ES-DE/custom_systems/es_systems.xml`
  and `es_find_rules.xml` into `ES-DE/custom_systems/`. The block replaces
  ES-DE's built-in Arcade system (its MAME/FBNeo commands are kept) and adds
  "Type X2 recomp (Standalone)" and "Type X2 (RetroArch)"; MAME stays the
  default, so set "Alternative emulator" on each Type X2 game.
* `x2.env` in a game folder sets the environment switches below (one
  `KEY=VALUE` per line); logs go to logcat, tag `x2`.

## RetroArch core

`typex2_libretro` (`typex2_libretro_android.so` on Android) contains every
recompiled game. Load the game folder, its `game.exe`, or any file inside it.

Installing:

* Android: copy the core to `RetroArch/downloads/` on shared storage, then
  *Load Core → Install or Restore a Core* (Android only lets RetroArch load
  cores from its own data folder). ES-DE then launches games with it through
  the system's "Type X2 (RetroArch)" command.
* Desktop: copy it (and `tools/libretro/typex2_libretro.info`) into
  RetroArch's cores folder.

Running:

* Video driver `gl` (OpenGL 3.3 core / GLES 3.2); Vulkan is not supported.
* The game is clocked by RetroArch's frame loop, so pause, fast-forward and
  slow-motion work. No save states, rewind, run-ahead or reset.
* One game per RetroArch session: guest threads cannot be torn down, so after
  closing content RetroArch must be restarted to load another game.
* RetroPad: FinalBurn Neo's Neo Geo layout ("Modern" pad): B A = A B (bottom
  row), Y X = C D (top row), R2 = A+B, R = C+D, L2 = A+B+C, L = B+C+D; Start =
  start, Select = coin, L3 = test, R3 = service. `X2_PAD` changes it (below);
  RetroArch remaps apply on top.

## Debugging switches (environment variables)

| Variable | Effect |
|---|---|
| `X2_LOG=0..3` | log level (same as -v/-vv/-vvv) |
| `X2_SHOT=f1,f2,...` `X2_SHOT_PATH=x.png` `X2_SHOT_EXIT=1` | save frames as PNG |
| `X2_SHOT_WINDOW=1` | screenshots capture the window (with overlays) |
| `X2_FPS=1` | start with the FPS overlay on |
| `X2_TOUCH=0/1` | on-screen touch pad off / on |
| `X2_PAD=south=b,east=ab,...` | gamepad buttons (south east west north l1 r1 l2 r2) → cabinet buttons (any of a b c d 5 6, `-` for none); see `runtime/src/padmap.h` |
| `X2_INPUT="600:c,606:-,700:s"` | scripted input (c coin, s start, udlr, abxy, t test, v service, p F9 capture) |
| `X2_DRAWLOG=<frame>[-<last>]` | log every draw of those frames |
| `X2_SHADERLOG=<id>` / `X2_DUMPTEX=<id>[@frame]` `X2_DUMPDIR=dir` | print a translated shader / dump a texture |
| `X2_GLPROF=1` / `X2_UPLOADLOG=1` | replay time per command type / texture+buffer upload log |
| `X2_PACELOG=1` | frame pacing (wake intervals, guest work per frame) every 120 frames |
| `X2_BT=<secs>` `X2_PEEK=addr,...` | periodic guest backtraces / memory peeks |
| `X2_TRACE_READS=1` | log file reads (pipe offsets into `arc locate`) |
| `X2_CALL="addr:args"` (+ `X2_POKE`, `X2_CALL_DELAY`) | call a guest function |
| `X2_GIL=1` | serialize guest threads |
| `X2_VSYNC=1` / `X2_NOPRESENT=1` | vsynced presentation / never present (headless runs) |
| `X2_NO_S3TC=1` | decode DXT textures on the CPU (default on GPUs without S3TC) |
| `X2_FONT=file.ttc` | font for GDI text where CoreText is unavailable (default: Noto Sans CJK) |
| `X2_AUDIOLOG=1` | audio peak level every 2 s |
| CMake `-DX2_FN_TRACE=ON` / `-DX2_WATCH=ON` + `X2_WATCH=lo-hi` | function-entry rings / store watchpoints |
| CMake `-DX2_TSO=0/1/2` | guest memory ordering on ARM: none / full TSO / release stores only (Android default) |
| CMake `-DX2_GEN_OPT=n` | optimization level of generated code (Android default 2, desktop 1) |

## Known gaps

* Structured exceptions (`RaiseException` / C++ `throw`) are not dispatched.
* Guest threads run truly parallel. On ARM, shared (non-stack) guest stores
  are release (and loads acquire, with `X2_TSO=1`) so x86 TSO-dependent code
  (the game's spinlocks) stays correct.
* KOF XII on macOS: the left credit counter shows "0" where the right shows
  "00"; with the stb_truetype font backend both show "00", so it comes from
  CoreText metrics.
* KOF XIII Climax: some background elements flash and fire/electric hit
  effects show stripes. It runs in its 640×480 mode (a 16:9 picture with
  bars); both front ends crop to the 16:9 band, but the picture is SD.
* GDI text with stb_truetype (Android): heavy weights are only emboldened by
  one pixel, so they look lighter than on macOS.
* KOF XIII runs in Japanese by default; switch the language in the test menu
  (F2 / L3 / TEST).
