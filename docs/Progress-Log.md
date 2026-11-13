# Progress log

Milestones, newest first. `git log` has the details.

## 2026-11-13: The launcher

- **Desktop app** (`launcher/`, Tauri):
  - a library of game folders, recognised by their executables;
  - per-game settings;
  - Play / Stop with the game's output in a log.
- **Builds on the user's computer:**
  - games are recompiled into a library that `x2-player` loads;
  - RetroArch cores and the Android app are exported through the CMake build.
- **Build tools downloaded on demand** (clang, CMake, Ninja, the Android NDK)
  into the launcher's folder, checked against their published checksums.
  The downloaded clang needs no OS SDK (`runtime/include/sdkless/`).
- **Verified on macOS:** a KOF '98 UM library build, pixel-identical to the
  CMake build, and an Android core export using only downloaded tools.

## 2026-11-13: KOF Maximum Impact Regulation A playable

- **Direct3D 8 layer** over the D3D9 implementation (declaration-token vertex
  shaders, sampler states via texture stage states, state blocks).
- **DirectDraw YUY2 overlay** for the opening movie (`hle/ddraw.c`).
- **Recompiler:** extra code sections are discovered; MMX / SSE2 saturating,
  averaging, multiply-add and pack instructions; 16-bit `pushf` / `popf`.
- **Runtime:** drive-relative paths; released surfaces are freed.

## 2026-11-13: KOF Maximum Impact Regulation A started

- **C runtime as a DLL:** `MSVCR80.dll`'s 93 imports implemented
  (`hle/msvcrt.c`). The loader binds data imports such as `_acmdln`.
- **Other Windows pieces:** `mmio*` file I/O for its WAVs (`hle/mmio.c`),
  window / GDI / `lstr*` stubs, `D3DXCreateTextureFromFileA`.
- **Status:** the game starts its threads, opens JVS and creates its window,
  then needs Direct3D 8, its renderer. That layer is next.

## 2026-11-13: KOF '98 Ultimate Match, the first Type X game

- **Running on macOS:** attract mode, team select and fights with full
  stages and sound.
- **Fixed-function pipeline:** generated vs_2_0 / ps_2_0 shaders for
  `SetFVF` draws and texture stage states (`gfx/ffp.c`). Draws with a vertex
  shader but no pixel shader, which were skipped before, now render too.
- **Pre-transformed (XYZRHW) vertices** skip the bound vertex shader, as in
  D3D9.
- **Threads and sync:** thread stacks are freed when a thread ends, and
  `PulseEvent` releases the threads waiting at that moment.
- **Executable:** found as `game.exe` or `launcher.exe`. The 16:9 crop only
  applies to KOF XIII Climax, so true 4:3 games keep their full frame.
- **On the tablet:** KOF '98 UM at 60 fps in RetroArch, five minutes without
  a stall. That needed reusable timer threads: one-shot timers exhausted the
  handle table. BlazBlue CT's crash fix is confirmed over three full scripted
  fights; its load pauses are now 0.3-0.5 s.

## 2026-11-13: BlazBlue CT on Android

- **On the tablet:** 60 fps in attract mode and fights in RetroArch. Load
  pauses of 0.3-0.7 s at character select and fight start remain.
- **Sprite decoder:** CT's own sprite RLE expander became a native;
  pixel-identical.
- **Effects:** D3DX texture parameters start NULL. CT drew before setting
  one, which crashed on the tablet at fight start. The fix still needs a
  tablet check.
- **Docs:** this documentation set, with status for the whole Type X and
  Type X2 libraries.

## 2026-11-08: BlazBlue Calamity Trigger

- **Running on macOS:** attract mode, character select and fights.
- **Recompiler:** flag importers for MSVC `/RTC` checks; discovery no longer
  loops on rejected gaps.
- **I/O board:** JVS answers CT's I/O-board controller handshake.
- **Threads:** a thread that suspends itself now blocks.

## 2026-11-08: BlazBlue Continuum Shift

- **Graphics:**
  - compiled D3DX effects (fx_2_0 parser, preshader VM, `ID3DXEffect`);
  - ps_1_x shaders;
  - `StretchRect` as a GPU blit.
- **Opening movie:** through the game's own DirectShow renderer filter.
- **Controls:** per-game 8-button layouts and player-2 scripted input.
- **Android:** sprite RLE native, BGRA swizzle uploads and `UpdateTexture`
  from the source: the worst pause went from ~430 to ~230 ms. Quit is clean.
- **Project setup:** lint configuration (`.clang-format`, `.clang-tidy`,
  `tools/lint.sh`), `CLAUDE.md` / `AGENTS.md`.

## 2026-11-07: Super Street Fighter IV Arcade Edition

- **What SSF4 needed:** D3D9 shader assembly, multiple render targets,
  Winsock stubs, and a frame-limiter patch.
- **Android speed-ups:**
  - one frame in flight;
  - alpha-test shader variants;
  - fp16 pixel shaders;
  - RGB565 for DXT1;
  - thread priorities.
- **Profiling switches:** `X2_PROF`, `X2_CSPROF`, `X2_GPUTIME`, `X2_AB`.

## 2026-11-06: RetroArch core, Android, KOF XIII Climax

- **RetroArch core:** a libretro core holding every game, a shared boot path,
  and the `retro_host` test frontend.
- **Android:** the stb_truetype font backend, a touch overlay and ES-DE
  integration.
- **Climax:**
  - flags carried out of MSVC CRT x87 helpers (fixed `sin`);
  - 2× backbuffer with a 16:9 crop.
- **Controls:**
  - KOF button numbering checked in the I/O test;
  - FBNeo-compatible pad layout;
  - hold Select for the test menu.
- **Load pauses:**
  - deferred texture uploads;
  - natives for zlib and a byte-fill loop.

## 2026-11-05: First release

- **Recompiler:** discovery and the x86 → C lifter.
- **Runtime:**
  - Win32, D3D9 and DirectSound;
  - DirectShow movies through FFmpeg;
  - the JVS board;
  - the GL renderer.
- **Games:** KOF XII runs attract mode and gameplay at 60 fps on macOS; KOF
  XIII Climax in bring-up.
