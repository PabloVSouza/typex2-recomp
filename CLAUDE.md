# CLAUDE.md

Guidance for AI coding agents (Claude Code, Codex, ...) working in this
repository. `AGENTS.md` is a symlink to this file. Read `README.md` for the
architecture, build steps and the full list of `X2_*` switches.

## What this is

Static recompilation of Taito Type X and Type X2 arcade games (x86 Windows XP PCs) to
native code for macOS and Android: `x2recomp` lifts `game.exe` (or `launcher.exe`) to C, the C
runtime provides memory, threads, Win32 / D3D9 / D3DX / DirectSound /
DirectShow, a JVS I/O board and renderers (Metal by default on macOS, Vulkan on
Android, GL as the reference and fallback). Front ends: a standalone app
(SDL3, launched by ES-DE) and a libretro core holding every game.

| Game | `games/<id>` | State |
|---|---|---|
| The King of Fighters XII | `kof12` | playable (macOS, Android standalone + RetroArch) |
| The King of Fighters XIII | `kof13` | playable |
| The King of Fighters XIII Climax | `kof13cl` | playable |
| Super Street Fighter IV Arcade Edition | `ssf4ae` | playable; a few stages 50-55 fps on the test tablet |
| BlazBlue Continuum Shift | `bbcs` | playable (macOS, RetroArch on Android): movie, arcade, versus; fights 60 fps on the tablet |
| BlazBlue Calamity Trigger | `bbct` | playable (macOS, RetroArch on Android at 60 fps); 0.3-0.5 s load pauses at fight start |
| The King of Fighters '98 Ultimate Match (Type X) | `kof98um` | playable (macOS, RetroArch on Android at 60 fps); fixed-function D3D9, palettized sprites |
| The King of Fighters Maximum Impact Regulation A | `kofmira` | playable on macOS (D3D8 over D3D9, DirectDraw overlay movie, MSVCR80 HLE); Android: attract mode at 60 fps, not played yet |

Every game in the maintainer's collection is now playable.

Documentation lives in `docs/` (also published as the GitHub wiki by
`tools/wiki_sync.sh`): `docs/Game-Status.md` tracks every Type X / X2 game,
each supported game has a `docs/Game-*.md` page, and `docs/Progress-Log.md`
gets an entry per milestone. Update them with the README.

## Hard rules

* **No Python.** Anywhere in the repo, helper scripts included (shell is fine
  for throwaway scripts). The recompiler and tools are C++20, the runtime C;
  the launcher (`launcher/`) is Tauri: Rust and TypeScript.
* **Nothing copyrighted in git.** Game executables, assets, dumps, generated
  code (`games/*/gen/`, gitignored) and screenshots of games never get
  committed. Only our own source and docs.
* **Commit after each verified milestone and push right after** (origin on
  GitHub; the maintainer wants the remote current). End commit messages with
  the co-author line the session gives you.
* **Never write into the user's game folders** (debug dumps, logs, test
  files). Use `$TMPDIR` / `/tmp` or the session scratchpad. The games' own
  saves (`sv/`, `Ranking/`, ...) are the exception: the game writes them.
* **Run automated tests without audio**: `SDL_AUDIO_DRIVER=dummy`, plus
  `X2_NOPRESENT=1` for headless runs.
* **Keep `README.md` current** with every user-visible change: game status,
  new `X2_*` switches (debugging table), known gaps. This file too, when a
  rule, game status or workflow changes, and `docs/` (game status, the
  game's page, progress log); then run `tools/wiki_sync.sh`.

## Layout

* `recompiler/src/`: `discover.cpp` (recursive-descent function discovery),
  `lift.cpp` (x86 → C, native signature matches), `main.cpp`. Output per game
  in `games/<id>/gen/` (+ `report.txt` with unsupported instructions).
* `runtime/src/`: `loader.c` (PE + import binding; `hle_modules[]` maps DLL
  names to HLE tables), `thread.c`, `hle/*.c` (one file per DLL family),
  `gfx/` (`gfx_record.c` recording shared by the backends, `gfx_gl*.c` GL and
  `gfx_metal.m` Metal and `gfx_vk.c` Vulkan backends, `gfx_texconv.c` DXT / 16-bit
  conversions, `sm3_glsl.c` bytecode → GLSL / MSL / Vulkan GLSL, `hlsl_glsl.c`
  HLSL → GLSL, `d3dasm.c` shader assembly, `ffp.c` fixed-function pipeline as
  generated shaders), `jvs.c` + `padmap.c` (inputs),
  `platform_sdl.c` / `platform_libretro.c` (front ends), `prof.c` (profiler).
* `runtime/CMakeLists.txt`: one target per `games/*/gen`; the game's label
  is set in the `if(game STREQUAL ...)` chain.
* `tools/`: `arc`, `kofarc`, `lifttest` (Unicorn differential tests of the
  lifter), `retro_host`, Android packaging, ES-DE and libretro files.
* `launcher/`: the desktop app (Tauri 2, React, Tailwind, shadcn/ui).
  `src-tauri/src/`: `games.rs` (supported games, identified by PE image size +
  entry; add new games there too), `recomp.rs` (build jobs: x2recomp, the game
  library for `x2-player`, CMake exports), `toolchain.rs` (pinned tool
  downloads), `kit.rs` (finds x2recomp / x2-player / sources: checkout or
  bundled `kit/`). `examples/cli.rs` runs the jobs from the console.
  `runtime/include/sdkless/` stands in for the C headers when the downloaded
  clang compiles without an OS SDK; a game library is linked as a bundle against
  `x2-player` on macOS.

## Adding a game

1. `build/recompiler/x2recomp "<game dir>/game.exe" games/<id>/gen`, check
   `report.txt` (data misread as code shows up as `outsb`/`insd`/`aas`:
   harmless).
2. Add the label in `runtime/CMakeLists.txt`, `cmake -S . -B build`,
   `cmake --build build --target x2-<id>`.
3. Run headless from the game folder:
   `SDL_AUDIO_DRIVER=dummy X2_NOPRESENT=1 X2_SHOT=300,600 X2_SHOT_PATH=$TMPDIR/s.png build/x2-<id> .`
   `-vv` lists unimplemented imports ("unimplemented import DLL!Name"); the
   first call to one is fatal and names the caller.
4. Implement missing imports in the matching `runtime/src/hle/*.c` table.
   A new DLL version (e.g. `d3dx9_31.dll`) is an alias in `hle_modules[]`
   (`loader.c`) and `fake_modules[]` (`hle/k32_misc.c`).
5. Find the cabinet button layout in the game's test menu INPUT TEST
   (`X2_INPUT=900:t,910:-,...` scripts the test switch and buttons) and
   document the `X2_PAD` line in the README.
6. Regression-check the other games (attract mode at least) before
   committing.
7. Add it to the launcher: `launcher/src-tauri/src/games.rs` (size and entry
   are the 2nd and 3rd values of `x2_image` in `gen/table.c`), and to
   `src/lib/mockApi.ts` for the browser preview.

## Debugging methods that worked

* Regenerate every game's `gen/` after any recompiler change.
* Stack imbalance / wrong calling convention: instrument the generated C
  with esp checks per call.
* Lifter changes: `tools/lifttest` against Unicorn.
* EFLAGS are C locals: callees whose flags are read by the caller (MSVC CRT
  x87 helpers) are found by `Lifter::analyze_flag_exports`; symptom of a
  miss is procedurally generated textures turning into hard 0/255 grids.
  The reverse (a function reading flags its caller set: MSVC `/RTC`
  `_RTC_CheckEsp`, mid-function entry points) is handled by flag importers in
  the same pass. An `/RTC` build failing a check ends in `failwithmessage` →
  DebugBreak; look at what the check compares before blaming the HLE.
* Before guessing at an I/O protocol, follow the game's own code: which
  thread writes the serial port, what it waits for (BlazBlue CT's I/O thread
  suspends itself every frame and the main thread resumes it), and what the
  reply parser accepts.
* ARM memory ordering: guests rely on x86 TSO (spinlocks released with plain
  stores); see `X2_TSO` in the README.
* GPU / pacing work on Android: measure with `X2_AB` + `X2_AB_TEST` (both
  variants in one run: the games pick random stages, separate runs are not
  comparable), `X2_GPUTIME`, `X2_PACELOG`, `X2_PROF`, `X2_CSPROF`.
* Tooling caution: `perl -0pi s|..|..|` with `|` or `.` in the pattern has
  inserted text at the top of files (several times); use an editor tool, or
  `sed` on single lines, and check `git diff` afterwards. The interactive
  shell's `grep` can print nothing on large binary-laced logs: use
  `/usr/bin/grep -a`.
* Stalled worker threads (no sound, a loader that never finishes): `X2_BT`
  backtraces, then resolve the import they sit in via `gen/table.c`.
  Windows semantics matter exactly: self-suspend + ResumeThread handshakes
  (BlazBlue CT), `PulseEvent` releasing current waiters (KOF '98 UM's sound).
* Geometry missing or misplaced: log a few vertices and the constants the
  vertex shader reads. Pre-transformed (XYZRHW) vertices bypass the vertex
  shader in D3D9 even when one is bound.
* Screenshot comparisons across builds: run the same build twice first;
  attract-mode frames with movies or timed fades are not deterministic.
* Never pipe `x2recomp` into `head`: SIGPIPE kills it halfway and leaves a
  `gen/` whose `funcs.h` and `table.c` disagree (undeclared `f_xxxxxxxx`).
* A renderer backend against GL: the same regression frames with both
  (`X2_RENDERER=gl` against the default: Metal on macOS, Vulkan on Android),
  compared pixel by pixel; movie and fade frames vary between runs. When one pass goes wrong,
  `X2_DUMPTEX=<id>@<frame>` its inputs and output on both, then reproduce the
  shader in a small offline harness. Metal compiler warnings are errors in
  `shadertest`: a macro once rewrote `[[texture(n)]]` attributes into ignored
  ones, which silently renumbered texture slots.
* Shader translator changes: dump every shader of every game once
  (`X2_SHADER_DUMP=<dir>` while running them), then `build/tools/shadertest`
  translates the whole set offline: GLSL output must stay byte-identical
  (`diff -r` against a run of the old translator), MSL must pass the Metal
  compiler. Faster and stricter than screenshots.
* Compiled D3DX effects: `build/tools/fxdump [-v] game.exe` lists every
  embedded effect (parameters, passes, states, shader constant tables,
  preshaders). Shader bytecode may contain comments with their own END token
  (preshaders), so never scan for END without skipping comments.
* Code reached only through tables the scan misses shows up as "jump to
  unknown guest address": find who stores the address (e.g. MSVC CRT x87
  `_trandisp` descriptors, handled in `discover.cpp`), teach discovery the
  pattern, or pass `--entry 0xADDR`.
* Black screen with draws happening: check `X2_DRAWLOG=<frame>` for an empty
  vertex shader body, the target pixel after each draw, and whether the game
  copies its scene with `StretchRect`.
* Load hitches on the tablet: `X2_PACELOG=1` shows "guest work max" (game
  thread) vs "replay max" (renderer); `X2_PROF=2` plus
  `profsym`-style symbolisation against `build-android/*.so.sym` names the hot
  functions. Hot guest routines can become natives (`lift.cpp` native_sigs,
  `natives.c`); verify them with pixel-identical screenshots of a
  deterministic screen (title, character select).
* A "host fault ... at 0x0" in a Java thread (e.g. `queued-work-looper`) on
  Android is ART's implicit null check, not a crash: the handler in `boot.c`
  only reports faults whose pc is in our module or a guest thread.
* The tablet runs throttled while locked or asleep (`mWakefulness=Dozing`):
  check `dumpsys window | grep isKeyguardShowing` before measuring, and raise
  `settings system screen_off_timeout` for long runs (restore it after).
* `X2_INPUT` drives player 2 with upper-case keys (C S U D L R A B X Y), e.g.
  to test versus mode.

## Android test device

The maintainer tests on an Apple M3 Pro Mac and a Samsung Galaxy Tab A11
(SM-X130, 8 GB / 128 GB: Helio G99, Mali-G57 MC2, no
S3TC, 2 big + 6 little cores, Android 16) with RetroArch
(`com.retroarch.aarch64`, video driver `gl` with driver switching on: our core
switches it to `vulkan`, the default renderer on Android) and ES-DE
(`org.es_de.frontend`). Games live in
`/storage/emulated/0/ROMs/arcade/<Game>.x2/`.

* adb is over Wi-Fi (wireless debugging); the port changes, ask the user
  for `ip:port` when `adb devices` is empty.
* Installing the core: push `build-android/typex2_libretro_android.so` to
  `/storage/emulated/0/RetroArch/downloads/`, then in RetroArch tap
  Load Core → Install or Restore a Core → the file (taps work, key events
  don't). Force-stop RetroArch and bring ES-DE back afterwards.
* Screenshots are black while the device is locked
  (`dumpsys window | grep isKeyguardShowing`).
* The user plays the test and says "check": then read `adb logcat -d`
  (tag `x2`). Put temporary switches (`X2_PACELOG=1`, ...) in the game's
  `x2.env` and restore it afterwards (normally `X2_FPS=1` + the `X2_PAD`
  line).
* The user's stick is 8-button with L1 / L2 leading each row; 6-button games
  (SSF4) use `X2_PAD=l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456`;
  BlazBlue `X2_PAD=l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234`.

## Style and lint

* `.clang-format` (LLVM base, 4 spaces, 120 columns) applies to new and
  changed lines only: `tools/lint.sh format` checks them, `tools/lint.sh fix`
  applies it. Don't reformat untouched code (the existing code is denser than
  any preset; a mass reformat would bury history).
* `.clang-tidy` holds bug-finding checks only (`bugprone`, `clang-analyzer`,
  `performance`): `tools/lint.sh tidy [files]` after `cmake -S . -B build`.
  New code should add no findings.
* Builds use `-Wall -Wextra`; new code should add no warnings. Generated
  code (`games/*/gen`) is built with `-w` and never linted or formatted.
* `.editorconfig`: UTF-8, LF, 4 spaces (2 in CMake / Markdown / XML).
* Match the surrounding code: C11 runtime, 4-space indent, short comments
  that say why. HLE functions are `static void Name(X2Cpu *c)` reading
  `ARG(n)` and returning with `RET(v)`, listed with their stdcall argument
  count in the module's `X2Hle` table.
* Log once per unusual event with `x2_log`; verbose detail behind
  `X2_TRACE` / `-vv`.
* New debugging switches are environment variables named `X2_*`, read once,
  and documented in the README table.
