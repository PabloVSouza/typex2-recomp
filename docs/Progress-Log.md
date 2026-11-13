# Progress log

Milestones, newest first. `git log` has the details.

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
