# typex2-recomp

Static recompilation of Taito Type X2 games (SNK's KOF XII / XIII, Capcom's
Super Street Fighter IV Arcade Edition, Arc System Works' BlazBlue Calamity
Trigger and Continuum Shift) to native code, so they run on macOS and Android:
as standalone apps (launched by ES-DE) or as a RetroArch/libretro core.

**Status:** KOF XII, KOF XIII and KOF XIII Climax are playable on macOS and on
Android (arm64, GLES 3.2), standalone or in RetroArch. Super Street Fighter IV
Arcade Edition (`games/ssf4ae`) is playable on macOS and in RetroArch on
Android: 60 fps through attract mode, character select and most fights on a
Galaxy Tab A11; a few stages are GPU-bound at 50-55 fps. BlazBlue Continuum
Shift (`games/bbcs`) is playable on macOS and in RetroArch on Android (opening
movie, title, character select, arcade and versus fights at 60 fps); BlazBlue
Calamity Trigger (`games/bbct`) is playable on macOS and runs at 60 fps in
RetroArch on Android (attract, select, fights; 0.3-0.7 s load pauses at fight
start). Rendering is D3D9 → OpenGL 3.3 / GLES 3.2 (D3D9 shader
bytecode, shader assembly, runtime HLSL and compiled D3DX effects all
translated to GLSL).

**Documentation:** [`docs/`](docs/Home.md) (also the project wiki) has the
status of every Type X / X2 game ([Game status](docs/Game-Status.md)), a page
per game, the architecture, porting and debugging guides, and the
[progress log](docs/Progress-Log.md).

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
  crypto, ole32), Direct3D 9 / D3DX (with compiled fx_2_0 effects and their
  preshaders, `gfx/fx.c` + `hle/d3dx_effect.c`) / DirectSound / DirectShow
  (FFmpeg movies) as host COM objects, an emulated JVS I/O board on COM2, and a GL renderer.
  The guest runs on its own threads and records frames; the platform thread
  replays them: `platform_sdl.c` (standalone) or `platform_libretro.c`
  (`retro_run`, which also clocks the guest).
* **tools/**: `arc` (unpack `gamediskimg.arc`), `kofarc` (list/extract KOF
  XIII `media.kof`, decrypting TXAC entries with the game's
  `TypeXmulti/txac` key), `lifttest` (differential test of the lifter against
  Unicorn), `fxdump` (list the compiled D3DX effects in a file, e.g. a
  game.exe), `retro_host` (minimal libretro frontend for testing the core),
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

Game folder names: `kof12`, `kof13`, `kof13cl` (XIII Climax), `ssf4ae`, `bbcs`, `bbct`
(BlazBlue Continuum Shift / Calamity Trigger).

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
(below, `X2_PAD` to change it); Back = coin (hold 2 s: test menu), Back+Start quits (for ES-DE).
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
* One frame in flight: the guest computes frame N+1 while RetroArch shows
  frame N, so guest work and GPU work each get a whole frame (one frame of
  latency, as on the cabinet's D3D9 driver; `X2_PIPELINE=0` for lockstep).
* One game per RetroArch session: guest threads cannot be torn down, so after
  closing content RetroArch must be restarted to load another game.
* RetroPad: FinalBurn Neo's Neo Geo layout ("Modern" pad): Y B X A = A B C D
  (left, bottom, top, right), R2 = A+B, R = C+D, L2 = A+B+C, L = B+C+D; Start =
  start, Select = coin (hold 2 s: test menu). L3 / R3 are left free for
  RetroArch hotkeys. Games with other panels get their own default, using all
  eight buttons (top row Y X R1 L1, bottom row B A R2 L2):
  SSF4 AE LP MP HP on Y X R1, LK MK HK on B A R2, L1 / L2 = three punches /
  kicks; BlazBlue CS A B C on Y X R1, D on B, and its combinations
  A = B+C (throw), R2 = A+B (barrier), L1 = A+B+C, L2 = A+B+C+D.
  The input descriptors RetroArch shows use each game's button names.
  `X2_PAD` changes it (below);
  RetroArch remaps apply on top, and work the same as with FBNeo: copy
  `remaps/FinalBurn Neo/FinalBurn Neo.rmp` to `remaps/Type X2/Type X2.rmp`
  (set `input_libretro_device_p1/p2 = "1"`) to share one layout.

## Performance on Android (tile-based GPUs)

What the renderer does for Mali-class GPUs, each measured on the test tablet
(Samsung Galaxy Tab A11, 8 GB / 128 GB, Mali-G57 MC2) with
`X2_AB` (both halves of the same run, since the game picks random stages):

* **Alpha test in shader variants only**: a `discard` anywhere in a pixel
  shader turns off early-Z and hidden-surface removal, so the alpha-test code
  is compiled into a separate variant used only while `D3DRS_ALPHATESTENABLE`
  is set (SSF4 fights: 15.0 → 11.6 ms GPU per frame).
* **fp16 pixel shaders** (`precision mediump`, texture coordinates stay
  highp): 2.5-4 % in fights, ~10 % in light scenes.
* **RGB565 for opaque DXT1** where S3TC is missing: half the upload size and
  memory.
* **One frame in flight** in the core (above): the largest win; in heavy
  scenes late frames went from 37-59 of 60 to 7-9.
* **Thread priorities**: guest threads that ask for HIGHEST / TIME_CRITICAL
  (SSF4's audio mixer) run on the big cores, so the main thread waits far
  less on their locks.
* **Loading hitches**: BGRA8 textures upload in the game's byte order and
  swap channels when sampled (texture swizzle) instead of on the CPU;
  `UpdateTexture` uploads straight from the source; hot guest routines get
  native replacements (zlib, both BlazBlue games' sprite RLE expanders, see
  `lift.cpp` / `natives.c`). BlazBlue's worst transition spike went from
  ~430 ms to ~230 ms; fights hold 60 fps.

Tools for finding the next bottleneck: `X2_GPUTIME`, `X2_PACELOG`, `X2_PROF`
(sampling profiler; the Android build keeps an unstripped `.sym` copy of the
core to resolve it) and `X2_CSPROF` (lock contention), all below.

## Contributing

Agents and humans: read `CLAUDE.md` (also `AGENTS.md`) for the project rules.
Style is `.clang-format` (new and changed lines only: `tools/lint.sh format`,
`tools/lint.sh fix`), bug-finding checks are `.clang-tidy`
(`tools/lint.sh tidy` after configuring `build/`), editors follow
`.editorconfig`.

## Debugging switches (environment variables)

| Variable | Effect |
|---|---|
| `X2_LOG=0..3` | log level (same as -v/-vv/-vvv) |
| `X2_SHOT=f1,f2,...` `X2_SHOT_PATH=x.png` `X2_SHOT_EXIT=1` | save frames as PNG |
| `X2_SHOT_WINDOW=1` | screenshots capture the window (with overlays) |
| `X2_FPS=1` | start with the FPS overlay on |
| `X2_TOUCH=0/1` | on-screen touch pad off / on |
| `X2_PAD=south=b,east=ab,...` | gamepad buttons (south east west north l1 r1 l2 r2 l3 r3) → cabinet buttons (any of a b c d = KOF's A-D, 1-6 = cabinet SHOT1-6, t test, v service, `-` for none; each game has a default); see `runtime/src/padmap.h` |
| `X2_INPUT="600:c,606:-,700:s"` | scripted input (c coin, s start, udlr, a b x y 5 6 = cabinet SHOT1-6 (KOF: A C - B D -), t test, v service, p F9 capture; upper case C S U D L R A B X Y = player 2) |
| `X2_DRAWLOG=<frame>[-<last>]` | log every draw of those frames |
| `X2_SHADERLOG=<id>` / `X2_DUMPTEX=<id>[@frame]` `X2_DUMPDIR=dir` | print a translated shader / dump a texture (`tex<id>-f<frame>.png`) |
| `X2_GLPROF=1` / `X2_UPLOADLOG=1` | replay time per command type / texture+buffer upload log |
| `X2_PACELOG=1` | frame pacing (wake intervals, guest work per frame) every 120 frames |
| `X2_GPUTIME=1` / `=2` | GPU time per frame / per render-target pass (timer queries) |
| `X2_AB=<frames>` `X2_AB_TEST=pipeline\|mediump` | alternate one experiment every `<frames>`; `X2_GPUTIME=1` and `X2_PACELOG=1` then report both halves of the same scenes |
| `X2_PROF=<secs>` `X2_PROF_OUT=file` | sampling profiler of the runtime's threads (1 ms); offsets resolve against the unstripped `typex2_libretro_android.so.sym` |
| `X2_CSPROF=1` | critical sections threads wait on longest, with what the owner was doing |
| `X2_PIPELINE=0` | libretro: strict lockstep instead of one frame in flight (one frame less latency, drops when guest + replay + GPU exceed a frame) |
| `X2_PS_MEDIUMP=0` | GLES: full-precision pixel shaders (default fp16) |
| `X2_DXT565=0` | without S3TC, decode opaque DXT1 to RGBA8 instead of RGB565 |
| `X2_PRIO=0` | ignore the game's SetThreadPriority (default: HIGHEST / TIME_CRITICAL threads get the fast cores) |
| `X2_WORKERS=little` | keep other guest threads on the little cores |
| CMake `-DX2_ASAN=ON` | AddressSanitizer on the runtime |
| `X2_BT=<secs>` `X2_PEEK=addr,...` | periodic guest backtraces / memory peeks |
| `X2_TRACE_READS=1` | log file reads (pipe offsets into `arc locate`) |
| `X2_CALL="addr:args"` (+ `X2_POKE`, `X2_CALL_DELAY`) | call a guest function |
| `X2_GIL=1` | serialize guest threads |
| `X2_VSYNC=1` / `X2_NOPRESENT=1` | vsynced presentation / never present (headless runs, hidden window; add `SDL_AUDIO_DRIVER=dummy` for silence) |
| `X2_BB_SCALE=n` | render the backbuffer at n× (default: the largest multiple fitting 1280×1024, so 640×480 games get 2×) |
| `X2_NO_S3TC=1` | decode DXT textures on the CPU (default on GPUs without S3TC) |
| `X2_FONT=file.ttc` | font for GDI text where CoreText is unavailable (default: Noto Sans CJK) |
| `X2_AUDIOLOG=1` | audio peak level every 2 s |
| CMake `-DX2_FN_TRACE=ON` / `-DX2_WATCH=ON` + `X2_WATCH=lo-hi` | function-entry rings / store watchpoints |
| CMake `-DX2_TSO=0/1/2` | guest memory ordering on ARM: none / full TSO / release stores only (Android default) |
| CMake `-DX2_GEN_OPT=n` | optimization level of generated code (Android default 2, desktop 1) |

## Known gaps

* SSF4 AE: link play is off (Winsock reports the network down; the build is the
  "stand-alone" one). Its six buttons are cabinet SHOT1-3 (LP MP HP) and SHOT4-6
  (LK MK HK). Its default layout is per game (see RetroArch core above); on an
  8-button stick whose rows are L1 Y X R1 / L2 B A R2 use
  `X2_PAD=l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456` in its
  `x2.env` (digits are cabinet SHOT1-6, see `runtime/src/padmap.h`). The
  opening movie's separate English audio track is not in this build's files.
* BlazBlue CS: four buttons, cabinet SHOT1-4 = A B C D (its INPUT DEVICE
  TEST names them BUTTON 1-4), with a per-game default layout. On an 8-button
  stick whose rows are L1 Y X R1 / L2 B A R2, its "Type B" panel (A B C on
  top, D below A) is `X2_PAD=l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234`.
  The opening movie plays through the game's own DirectShow renderer filter
  (RGB24 samples pushed into its input pin). Effects run without
  saving / restoring device state around Begin / End (the game does not rely
  on it); D3DX effect `Get*Array` / parameter blocks / `CloneEffect` are not
  implemented and stop with a named error if a game calls them.
* BlazBlue CT: same panel and default layout as CS. Its `game.exe` is the
  arcade `launcher.exe` with the save paths changed (`d:\` → `sv\`); the code
  is identical. Before JVS it shakes hands with a controller on the I/O board
  (packets to address 0, commands 0-8), answered by `jvs.c`. The build has
  MSVC run-time checks (`_RTC_CheckEsp` reads the caller's flags; see
  "flag importers" in `lift.cpp`); a failed check now stops with "the game
  called DebugBreak". On the tablet: 60 fps, with 0.3-0.7 s load pauses.
* Structured exceptions (`RaiseException` / C++ `throw`) are not dispatched.
* Guest threads run truly parallel. On ARM, shared (non-stack) guest stores
  are release (and loads acquire, with `X2_TSO=1`) so x86 TSO-dependent code
  (the game's spinlocks) stays correct.
* KOF XII on macOS: the left credit counter shows "0" where the right shows
  "00"; with the stb_truetype font backend both show "00", so it comes from
  CoreText metrics.
* KOF XIII Climax composes its 1280×720 scene into a 640×480 backbuffer (a
  16:9 picture with bars). The backbuffer is rendered at 2× (1280×960) and both
  front ends crop the 16:9 band, so the result is 1280×720.
* GDI text with stb_truetype (Android): heavy weights are only emboldened by
  one pixel, so they look lighter than on macOS.
* KOF XIII runs in Japanese by default; switch the language in the test menu
  (F2, hold Select / Back on a pad, or TEST on the touch pad).
