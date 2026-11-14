# Debugging

The `X2_*` switches are at the [end of this page](#switches); before them,
the methods that found real bugs.

## Crashes and wrong behaviour

| Symptom | Where to look |
|---|---|
| "unimplemented import DLL!Name" | Implement it in the matching `hle/*.c` table (`-vv` lists them all at load). |
| "jump to unknown guest address" | Code reached only through a table discovery missed. Find who stores the address, teach `discover.cpp` the pattern, or pass `--entry 0xADDR`. |
| Stack imbalance / wrong calling convention | Instrument the generated C with esp checks around each call. |
| Procedural textures turn into 0/255 grids | A callee whose flags the caller reads was missed by `analyze_flag_exports` (MSVC CRT x87 helpers). |
| "the game called DebugBreak" | Often an MSVC `/RTC` check failing (`_RTC_CheckEsp`, `failwithmessage`). Look at what the check compares before blaming the HLE. |
| "guest memory fault at guest address 0000000x" | A guest pointer that is really an index or handle. The fault handler prints the host backtrace; symbolise it. On Android, use the unstripped `.sym` file and the base address implied by `on_fault`'s frame. |
| I/O ERROR / no inputs | Follow the game's own I/O thread: what it writes, what it waits for, what its reply parser accepts. |
| Spinlock hangs on ARM only | x86 memory ordering assumptions; see `X2_TSO`. |
| A worker thread never wakes (no sound, a stalled loader) | Take guest backtraces (`X2_BT=<secs>`) and resolve the import it waits in (`table.c`). Self-suspend / resume handshakes and `PulseEvent` timers need exact Windows semantics. |
| "cannot allocate guest stack" after a while | Threads are created and never reclaimed (one-shot `timeSetEvent` timers are threads). Stacks are freed when a thread ends; look for other per-thread allocations. |
| Faults "at 0x0" in a Java thread on Android | ART's implicit null checks, not ours. The handler only reports faults in our module or on guest threads. |

## Graphics

- **Black screen while draws happen.** Use `X2_DRAWLOG=<frame>` and look for
  an empty vertex shader body. Check the target pixel after each draw, and
  whether the game copies its scene with `StretchRect`.
- **Inspecting shaders and textures.** `X2_SHADERLOG=<id>` prints a
  translated shader, `X2_DUMPTEX=<id>` dumps a texture, and F9 logs every
  draw of the next frame.
- **Compiled effects.** `build/tools/fxdump [-v] game.exe` lists every
  embedded D3DX effect: parameters, passes, states, constant tables and
  preshaders.
- **Scanning shader bytecode.** Bytecode can hold comments with their own
  END token (preshaders), so always skip comments.
- **Geometry in the wrong place or missing.** Log a few vertices of the draw
  (position, colour, texture coordinates) and the shader constants the
  vertex shader reads. Positions like -0.5, 15.5, 31.5 are D3D9
  pre-transformed screen coordinates, which bypass the vertex shader.
- **What a texture really holds.** `X2_DUMPTEX=<id>@<frame>` dumps it, and
  `X2_UPLOADLOG=1` lists every upload with its rectangle.

## Recompiler

- **Lifter tests.** Check lifter changes with `tools/lifttest`, which
  compares against Unicorn.
- **Regenerate everything.** After any recompiler change, regenerate every
  game, then compare deterministic screens pixel by pixel.
- **Natives.** A native replacement must give pixel-identical screenshots
  against the lifted original.

## Performance

See [Android and RetroArch](Android-and-RetroArch.md#measuring). Hot guest
routines show up as `f_xxxxxxxx` in `X2_PROF` output. The game's own
frame-limiter spin can look like work: it shows as time in `x2_time_ns`
called from a QueryPerformanceCounter loop.

## Tooling pitfalls

- **`perl -0pi`.** With `|` or `.` in the pattern it has inserted text at the
  top of files. Use an editor, or `sed` on single lines, and check
  `git diff`.
- **Never pipe `x2recomp` into `head`.** SIGPIPE stops it halfway and leaves
  a broken `gen/`.
- **Searching big logs.** The interactive shell's `grep` can silently print
  nothing on large logs with binary bytes in them; use `/usr/bin/grep -a`.
- **Comparing screenshots across builds.** Attract-mode frames with movies or
  timed fades vary from run to run. Take two runs of the same build first, and
  compare only the frames that match between them.

## Switches

Environment variables, read once at start. On Android put them in the game folder's
`x2.env` (one `KEY=VALUE` per line); logs go to logcat with the tag `x2`.

| Variable | Effect |
|---|---|
| `X2_LOG=0..3` | log level (same as -v/-vv/-vvv) |
| `X2_SHOT=f1,f2,...` `X2_SHOT_PATH=x.png` `X2_SHOT_EXIT=1` | save frames as PNG |
| `X2_SHOT_WINDOW=1` | screenshots capture the window (with overlays; GL only) |
| `X2_FPS=1` | start with the FPS overlay on |
| `X2_TOUCH=0/1` | on-screen touch pad off / on |
| `X2_PAD=south=b,east=ab,...` | gamepad buttons (south east west north l1 r1 l2 r2 l3 r3) → cabinet buttons (any of a b c d = KOF's A-D, 1-6 = cabinet SHOT1-6, t test, v service, `-` for none; each game has a default); see `runtime/src/padmap.h` |
| `X2_PADLOG=1` | RetroArch core: log each change of the RetroPad buttons the frontend reports and the cabinet buttons they press (finds remaps that move buttons) |
| `X2_INPUT="600:c,606:-,700:s"` | scripted input (c coin, s start, udlr, a b x y 5 6 = cabinet SHOT1-6 (KOF: A C - B D -), t test, v service, p F9 capture; upper case C S U D L R A B X Y = player 2) |
| `X2_DRAWLOG=<frame>[-<last>]` | log every draw of those frames (GL: also `X2_DRAWLOG_VERTS=<n>`, the first n vertices of each draw as floats and hex) |
| `X2_SHADERLOG=<id>` / `X2_DUMPTEX=<id>[@frame]` `X2_DUMPDIR=dir` | print a translated shader / dump a texture (`tex<id>-f<frame>.png`) |
| `X2_SHADER_DUMP=<dir>` | save every shader the game creates (`<hash>.vs.tok` / `.ps.tok`) for `build/tools/shadertest <glsl\|essl\|msl> <dir> <out>`, which translates them all offline (msl: each checked by the Metal compiler). Game data: never commit them |
| `X2_GLPROF=1` / `X2_UPLOADLOG=1` | replay time per command type / texture+buffer upload log |
| `X2_PACELOG=1` | frame pacing (wake intervals, guest work per frame) every 120 frames; Vulkan also logs any replayed frame over 100 ms with its time by command type, upload MB and ring flushes |
| `X2_DXT_GPU=0` / `X2_LAZY_UPLOAD=0` | Vulkan: decode DXT textures on the CPU again (default: on the GPU, for GPUs without S3TC) / upload every texture at once (default: uploads to textures no draw has read yet wait, a few MB a frame, until something reads them) |
| `X2_GPUTIME=1` / `=2` | GPU time per frame / per render-target pass (timer queries; Metal: `=1` only, from command buffer times) |
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
| `X2_RENDERER=gl` / `metal` / `vulkan` | the renderer. Android: Vulkan by default (`gfx/gfx_vk.c`, Vulkan 1.3, shaders compiled by glslang; the app, and the RetroArch core through RetroArch's Vulkan driver, which it switches to by itself when driver switching is on); `gl` selects OpenGL ES (the app also falls back to it without Vulkan 1.3). macOS: Metal by default (`gfx/gfx_metal.m`, standalone app; GL if there is no Metal device), `gl` for OpenGL 3.3. Vulkan: `X2_GPUTIME=1` (GPU ms per frame), `X2_PS_MEDIUMP=0`, `X2_DXT565=0` as on GL. Vulkan and Metal draw the FPS and touch overlays too. Metal: the deterministic regression frames match GL pixel for pixel. `X2_DRAWLOG`, `X2_DUMPTEX` (BGRA8 / R8), `X2_SHADERLOG` (prints MSL) and `X2_UPLOADLOG` work with it |
| `X2_VSYNC=1` / `X2_NOPRESENT=1` | vsynced presentation / never present (headless runs, hidden window; add `SDL_AUDIO_DRIVER=dummy` for silence) |
| `X2_BB_SCALE=n` | render the backbuffer at n× (default: the largest multiple fitting 1280×1024, so 640×480 games get 2×) |
| `X2_NO_S3TC=1` | decode DXT textures on the CPU (default on GPUs without S3TC) |
| `X2_FONT=file.ttc` | font for GDI text where CoreText is unavailable (default: Noto Sans CJK) |
| `X2_AUDIOLOG=1` | audio peak level every 2 s |
| CMake `-DX2_FN_TRACE=ON` / `-DX2_WATCH=ON` + `X2_WATCH=lo-hi` | function-entry rings / store watchpoints |
| CMake `-DX2_TSO=0/1/2` | guest memory ordering on ARM: none / full TSO / release stores only (Android default) |
| CMake `-DX2_GEN_OPT=n` | optimization level of generated code (Android default 2, desktop 1) |

