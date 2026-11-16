# Progress log

Milestones, newest first. `git log` has the details.

## 2026-11-16: The launcher is the Android app; game packs

- **What:** the Android app is the launcher built with Tauri
  (`launcher/src-tauri/gen/android`, package `com.typex2.recomp`, debug
  builds `.dev`). SDL's activity runs games in a `:game` process with
  `libmain.so` and `libSDL3.so` from the new game-less `x2app` target. The
  app holds no game code: `libmain` loads `files/packs/x2-<size>-<entry>.so`,
  named after the game's PE identity, built by the `packs` target (the
  launcher's "Game packs for the Android app" export) and pushed by Send to
  device into `Android/data/<app>/files/packs`, which the app moves into
  private storage. The old all-in-one APK (`tools/android` packaging, a Java
  settings screen tried the same day) is gone.
- **On Android** (`android.rs`): the library is ES-DE's `ROMs/arcade`
  (existing `x2.env` lines become the settings), Play writes `x2.env` and
  starts the game activity through JNI, Export and Build tools are hidden,
  and the window is immersive with our icon on the splash.
- **Feasibility first:** a game library dlopened from the app's files
  directory (writable, not the APK) runs KOF XII at 60 fps on Android 16.
- **Tested on the tablet:** all eight games found; KOF XII played from the
  app and from an ES-DE-style intent; its `x2.env` written unchanged.
- **Unsupported games** are listed by folder name (`other-<size>-<entry>`
  ids), on both platforms.

## 2026-11-16: Targeted wakeups; KOF XIII Climax is stage-bound on Android

- **Found:** in a Climax fight on the tablet, `X2_PROF` showed its 13 job
  threads in `wait_many` at ~60% of a core each: every wait shared one
  condition variable, so each SetEvent / ReleaseSemaphore woke every waiting
  thread to re-check and sleep again.
- **Fix:** a blocked thread waits on its own condition variable, listed with
  its handles; a change wakes only the threads waiting on that object
  (`X2_WAKE_ALL=1` restores the old behaviour for comparisons). No game
  regressed: BlazBlue CS 15.2, CT 14.9, KOF '98 UM 14.3, KOF XII 17.2, KOF
  XIII 14.4, KOF MI 14.6 late frames per 120 in fights.
- **Climax is not fixed by it.** A first comparison (46-74 late frames per
  120 before, 13-17 after) put a heavy stage against the light green one.
  Climax picks stages at random, and identified by screenshot: the green
  stage holds 60 fps (13.8-14.5 late per 120); the temple, street, cage and
  Japanese-temple stages run at about 50 (39-62 late, game thread
  16.5-18.2 ms) with either kind of wakeup (temple: 61.8 broadcast, 59.1
  targeted). The 0.1.1 notes claimed otherwise and were corrected.
- **SSF4** also varies with the stage (game thread 14-15.5 ms): 15-28 late
  frames per 120, about the same with either kind of wakeup.
- **Lesson:** games that pick stages at random need the stage identified
  (screenshot) before two runs are compared.

## 2026-11-15: The guest's clock advances a frame per Present

- **Found in real fights on the tablet** (scripted to the fight and checked
  with screenshots): BlazBlue CS had 21-41 late frames per 120, CT 31-40,
  though output averaged 60 fps. The game thread's frames sat at 16.7-17.9 ms
  even on menus: like SSF4 and KOF MI, the BlazBlues spin on their own timer
  until 1/60 s has passed since their last frame, and that limiter beat
  against the runtime's pacing (the libretro frontend's clock, or the
  player's own 60 Hz): whenever the spin ended just after a tick, the frame
  missed it.
- **Fix, for every game:** the guest's clocks (QueryPerformanceCounter,
  timeGetTime, GetTickCount) advance at least 1/60 s from one Present to the
  next. When Present returns, a frame's worth has already passed for the
  game, its spin ends at once, and the runtime alone sets the pace. Over time
  the clock stays at real speed (SSF4's round timer drops 5 s per 300 frames).
  `X2_FRAME_CLOCK=0` turns it off.
- **Measured in fights after it:** BlazBlue CS 13-18 late frames per 120, CT
  15, KOF MI 14.6, KOF '98 UM 13-23, SSF4 14-16, the level of RetroArch's own
  jitter; the BlazBlues' game-thread frames went from 16.7-17.9 ms (spinning)
  to 10-14 ms (work).
- **Scripts:** KOF MI needs three characters with their colours and the order
  (A, B, C) to reach a fight; BlazBlue CS's presses land on its button
  configuration panel, and the select timer picks for it.

## 2026-11-15: Known issues: fonts and BlazBlue CT's fight start

- **KOF XII's credit counter (macOS):** the counters are laid out for
  FOT-Matisse's digits; Hiragino Sans W8's (0.77 em) pushed the left
  counter's second digit out. Ultra-bold faces now map to W7 (0.74 em).
- **Bold text on Android:** bold faces were only emboldened as bitmaps, and
  KOF XII asks for outlines (`GGO_NATIVE`), so its heavy text came out in the
  regular weight. Outlines now grow as FreeType's `FT_Outline_Embolden`
  does. Checked on macOS with the stb backend and Hiragino W3, then on the
  tablet. (The note blamed KOF XIII, which draws no GDI text.)
- **BlazBlue CT's fight start, measured on the tablet** with `X2_PACELOG`'s
  slow-replay breakdown and `X2_PROF`:
  - `vkAllocateMemory` took ~25 ms per 64 MB block, five blocks in one replay
    (128 of 140 ms of texture creation). The recorder now announces textures
    as the game creates them (`gfx_tex_create_hook`), and a thread allocates
    their blocks while the game is still loading.
  - Queued (lazy) uploads copied their data, 100+ MB, into fresh allocations;
    they now point into their frame, kept alive by `Frame.refs` as in GL.
  - On the game's side, ~230 ms went to growing the frame's command buffer:
    with the instance heap a large `realloc` copied. It now uses `mremap`, and
    large command buffers are kept for the next load (`MADV_FREE` past
    64 MB). File opens no longer `access()` first (a FUSE round trip on
    Android's shared storage).
  - Result: the renderer's worst frame at fight start went from 226-252 ms
    to 31-92 ms. The game's loading frame takes 0.3-0.6 s depending on the
    stage, now mostly the game's own work.
- **RetroArch Reset (Vulkan):** the game restarts in a fresh instance, as a
  switch does. RetroArch keeps its device across a reset, so the running
  instance destroys its objects and frees their memory (`gfx_vk_release`),
  the new one takes the context over (`retro_x2_takeover`) and the old one
  forwards RetroArch's context callbacks to it. Tested on the tablet: KOF XII
  reset twice, then SSF4 reset once; memory stays level and closing gives it
  back. OpenGL still says Reset isn't supported.
- **KOF MI on Android:** a scripted run (`X2_INPUT`, as far as the pre-fight
  scene: it doesn't pick three characters and their order) ran at 57-60 fps with
  a third of the frames late: the main loop paced itself by `timeGetTime`
  against the display's clock. A byte patch runs one frame per pass; late
  frames went from 25-51 to 12-17 per 120. `mmioOpenA` now reads from
  memory (the game opens a WAV per sound played; each open is a FUSE round
  trip). KOF '98 UM's standalone app ran a scripted fight at 60 fps.
  A full KOF MI script (three characters with their colours, the order with
  A B C) then reached real fights: 59.5-60.5 fps, 13-17 late frames per 120;
  loads of ~1 s at the fight start and 0.3-0.5 s per next team member, mostly
  the game's own.
- **Left (fixed 2026-11-16):** a full upload ring waited for the GPU (76-88 ms once, around
  character select); spilling into a second staging buffer would avoid it.

## 2026-11-15: RetroArch switches games without a restart

Closing a game in RetroArch and loading another used to be refused ("restart
RetroArch"): guest threads cannot be stopped and the runtime's state is
global, so a second game could not boot in the same library instance.

- **One instance per game:** the core RetroArch loaded runs the first game;
  each later game runs in a fresh instance of the same file
  (`android_dlopen_ext` with `ANDROID_DLEXT_FORCE_LOAD` on Android, a
  temporary copy elsewhere), with its own globals. The `retro_*` entry points
  forward to it. The environment is restored before each game, so one
  game's `x2.env` does not carry over.
- **What a closed game gives back:** its 4 GiB guest space is remapped
  inaccessible (pages freed), and on ELF platforms its large heap blocks
  too: the core links with `--wrap=malloc,...` (`runtime/src/heap.c`), blocks
  of 256 KiB or more are mapped one by one and released at unload. Parked
  threads that wake up later fault on the released memory and stay parked
  (threads started with `x2_spawn` are told apart from the frontend's).
- **Measured on the tablet** (KOF XII, BlazBlue CT, SSF4, KOF XII,
  BlazBlue CT, SSF4 in one session): 67-80 MB released per game, 4-19 MB of
  small blocks stays; RetroArch was at 320 MB after the sixth game, against
  750-1100 MB while one plays.
- **Launched by ES-DE**, RetroArch ends its process when the game closes
  (and with `quit_on_close_content`), so that path always started fresh.
- **Testing:** `retro_host --next <content>` closes and reopens the core
  between contents as RetroArch does.

## 2026-11-14: First-play shader stalls: Metal batches, Android app cache

First plays on the Mac stalled for up to 2 s at a time: Metal compiled each
shader at the first draw that used it, and Apple's compiler takes 100-450 ms
per shader (macOS caches compiled libraries by their exact source, so a first
play compiles every one). Measured over 75 s of SSF4's attract mode with an empty cache
(`X2_MTL_COLD=1`, `X2_PACELOG=1`):

| | stall time | stalls ≥ 200 ms |
|---|---|---|
| before | 39.7 s | 60 |
| unused texture overloads dropped | 19.3 s | 72 (shorter) |
| batches at creation | 9.2 s, 8.4 s of it in the first 10 s | 7, all at boot |

- **Smaller shaders:** the MSL prelude carried sampling overloads for every
  texture type; instantiating them was 300 ms of a 450 ms compile.
  `sh_msl_prune` keeps those a shader can call (GLSL / Vulkan output
  unchanged; all 709 shaders of the eight games pass the Metal compiler).
- **Batches:** shaders are compiled as the game creates them, on its loading
  screens, 64 to a library (each in its own namespace, pixel shaders also
  with the alpha test), on two background queues. 100 shaders compile in
  0.8 s instead of 24. Games create shaders in the same bursts every run, so
  later runs hit macOS's cache: SSF4's boot stalls went from 8.3 s to 0.1 s
  on the second run. A draw needing a shader of an open batch compiles it at
  once.
- **Left:** new pipelines still cost 5-45 ms each (about 20 when a fight
  scene first appears, 250 ms in all, even with macOS's cache).

**Android (Vulkan):** the standalone app kept no shader or pipeline cache:
its cache folder came from `HOME`, which Android does not set. It now uses
the app's files folder: SSF4's second run went from 2.1 s of pipeline
stalls in 80 s to 0.1 s (the RetroArch core had its cache already). On a
first play, a draw missing its pipeline now has the rest of its frame
scanned and every missing pipeline built together (keys predicted from the
recorded draws: all 65 of the first scenes' were used). The Mali-G57
driver compiles them one at a time even on four threads with caches of
their own, so a first play still costs about 1.2 s at boot and 0.5 s when
a new scene first appears.

## 2026-11-14: CI, Linux and Windows builds, versioned releases

- **CI** (`.github/workflows/ci.yml`): the recompiler, tools, runtime and
  x2-player build on Ubuntu 24.04, macOS and Windows (MSYS2 CLANG64) on every
  push; the version must be the same in every file.
- **Linux and Windows standalone:** Vulkan by default (glslang fetched as on
  Android), GL as the fallback; SDL3 built from the pinned release where the
  system has none.
- **Windows port:** the guest space with VirtualAlloc, faults through a
  vectored handler, game libraries with LoadLibrary against x2-player's import
  library, GL 2+ through pointers (opengl32 has 1.1 only), POSIX stand-ins
  (`compat_win`: pread/pwrite at an offset, time functions, fnmatch, disk
  space), binary file mode by default. The launcher downloads llvm-mingw as
  its compiler there. Built in CI; not yet run with a game.
- **Releases:** `tools/version.sh` keeps the version in step; `CHANGELOG.md`
  gives each release its notes; a `v*` tag builds the launcher with its kit for
  the three systems (`.github/workflows/release.yml`) and publishes it.

## 2026-11-14: DXT on the GPU and lazy uploads (Vulkan)

- **Cause of BlazBlue CT's fight-start pause:** the fight loads ~900 textures,
  over 100 MB, in one frame. The tablet has no S3TC, so DXT was decoded on the
  CPU, and the burst overflowed the upload ring, which then waited for the GPU.
- **DXT decoded on the GPU:** the blocks go up as they are and one pass per
  upload decodes them into the mip level with gfx_dxt_decode's integer maths:
  BlazBlue CT and KOF XIII frames are pixel-identical to CPU decoding.
- **Lazy uploads:** uploads to textures no draw has read yet wait in a queue;
  a texture's own wait is cleared when a draw or blit reads it, and the rest
  trickle up 4 MB a frame.
- **Result:** the worst replay stall at fight start went from 0.5-0.6 s to
  about 0.25 s. Still a pause, smaller.
- **Blinking floors (fixed):** on some BlazBlue CT stages a walkway vanished
  every other frame with Vulkan on the tablet. The game issued the same draws
  every frame; a render pass resuming a target that the previous pass had
  stored, with no layout change between, had no barrier, and Mali loaded the
  tiles before they were written. Passes now wait for the earlier stores in
  that case: on Arakune's stage a recording went from 97 blinks to none, and
  SSF4's GPU time is unchanged.

## 2026-11-14: No more shader hitches on Vulkan

- **Cause:** pipelines are made at the first draw that needs them. With warm
  caches the driver's part was fast, but reading each shader's SPIR-V from its
  own file on Android's shared storage cost milliseconds per file, and shaders
  never seen on the device were compiled by glslang right then (up to 280 ms):
  the hiccups on new hit effects. SSF4, 150 s of attract mode: 95 pipelines
  over 2 ms, the slowest 291 ms.
- **Fix:** SPIR-V lives in one pack file read at start (`vk-spirv.pack`, older
  per-shader files moved in once), and every shader's common variant is
  compiled when the game creates it, during loading. Same run: no pipeline over
  2 ms; the vertex variant for integer inputs and alpha-tested pixel variants
  are still made at first use.
- **SSF4 on Vulkan:** 10 minutes of attract fights average about 12 ms of GPU
  time a frame on the Galaxy Tab A11 and hold 60 fps; the 50-55 fps stages were
  OpenGL's.
- **Debugging:** the Vulkan stats line counts new pipelines and their time;
  any over 2 ms is logged with its shader split.

## 2026-11-13: Metal is the default on macOS

- **Default renderer:** the standalone app renders with Metal on macOS;
  `X2_RENDERER=gl` selects OpenGL, which is also the fallback without a Metal
  device. All eight games' regression frames match GL (movie and fade frames
  vary from run to run on either renderer).
- **Overlays:** Metal draws the FPS counter and touch controls too.
- **Touch buttons per game:** SSF4 gets six (LP MP HP over LK MK HK) and
  BlazBlue's A-D press its own buttons (they pressed KOF's, so B was D).
- **Tools on every backend:** `X2_SHADER_DUMP` moved into the shared recorder;
  `X2_GPUTIME=1` reports Metal's GPU time per frame.

## 2026-11-13: Vulkan is the default on Android; KOF MI on the tablet

- **Default renderer:** the RetroArch core and the standalone app render with
  Vulkan on Android; `X2_RENDERER=gl` in `x2.env` selects OpenGL ES. The app
  falls back to OpenGL by itself on devices without Vulkan 1.3.
- **Sweep (RetroArch on the tablet, GL against Vulkan):** every game runs at
  60 fps on both with no errors. GPU time per frame is close to GL's in the
  light games (BlazBlue CT, KOF XII, XIII Climax, '98 UM) and 18% lower in
  SSF4's fights.
- **KOF Maximum Impact Regulation A on Android:** the opening movie and
  attract mode run at 60 fps in RetroArch and the app, on Vulkan and on GL.
  Fights not played yet.
- **Fix:** CPU uploads into a BGRA render target (KOF MI's movie overlay) had
  red and blue swapped on Vulkan, where render targets are RGBA8.
- **Overlays on Vulkan:** the app's touch controls and FPS counter are drawn
  by the Vulkan present too (they were GL-only), so the app keeps them with
  Vulkan as the default.

## 2026-11-13: Vulkan backend on Android

- **`gfx_vk.c`** (`X2_RENDERER=vulkan`, Android standalone app), developed on the
  Galaxy Tab A11 (Mali-G57, Vulkan 1.4 driver):
  - dynamic rendering, push descriptors, dynamic depth / stencil / cull state;
  - shaders: a Vulkan GLSL output of both translators, compiled to SPIR-V on
    the device by glslang;
  - clears of a whole target become load ops; passes without depth tests
    leave the depth buffer alone; barriers name the exact stages.
- **Correctness:** KOF '98 UM's frames are pixel-identical to GL on the Mac;
  KOF XIII's to GL on the tablet. SSF4, KOF XII and BlazBlue CT match GL apart
  from timing (fades, animation).
- **In RetroArch:** the core negotiates its Vulkan device with RetroArch (the
  features the backend needs) and hands each frame over with `set_image`;
  `X2_RENDERER=vulkan` in the game's `x2.env` selects it. SSF4 runs 59-60 fps
  except a few fight scenes (GPU 16.6 ms against GL's 13 ms); KOF '98 UM 60 fps.
- **Speed (SSF4 on the tablet, RetroArch):** passes overlap now (a sampled texture
  only holds back pixel shaders), render targets are RGBA8 like GL's. Fights:
  10.5 ms of GPU time per frame against GL's 13 ms (the intro 10 against 11.6),
  60 fps throughout. `X2_GPUTIME=2` gives GPU time per render target on Vulkan
  too.
- **Caches:** compiled SPIR-V and the pipeline cache are kept in the shader cache
  folder (with GL's program binaries). Compiling during BlazBlue CS's opening
  movie had starved its decoder of CPU time (blocky frames on the first run).
- **Fair comparison:** fights are random stages, so GL and Vulkan are compared on
  runs whose scene sequences match: fights 10.4 ms against 12.7 (18% less GPU
  time), the intro 10.2 against 11.4; light UI scenes 2.0 against 1.66.

## 2026-11-13: Metal backend

- **Shaders in Metal Shading Language:** both D3D shader translators also
  write MSL. All 705 shaders the eight games create in the regression runs
  compile with the Metal compiler, warnings included; the GLSL output is
  byte-identical to before.
- **Renderer split:** recording (`gfx_record.c`) is separate from the GL
  replay, so other backends replay the same frames.
- **`gfx_metal.m`** (`X2_RENDERER=metal`, macOS standalone): every
  deterministic regression frame of the eight games is pixel-identical to GL.
  The frames that differ are movies and animations caught at different
  moments.
- **Next:** Metal by default on macOS after more play-testing, then the
  Vulkan backend for Android, Windows and Linux.

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
