# Architecture

```
game.exe ──x2recomp──► games/<id>/gen/*.c ──clang──► x2gen_<id> (objects) ─┬─► x2-<id> (desktop executable)
                                                                           ├─► libx2_<id>.so (Android app)
runtime/ (C): memory, threads, Win32/D3D9/DSound/DirectShow,               └─► typex2_libretro (all games)
              JVS I/O, GL renderer; SDL3 or libretro platform layer
```

## Recompiler (`recompiler/`, C++20, capstone)

1. **Discovery** (`discover.cpp`). Recursive descent from the entry point,
   exports, relocated code pointers and known table patterns finds every
   function.
   - MSVC CRT `_trandisp` descriptors become entry points.
   - Gaps between known functions are tried as candidates, once each.
   - Code that discovery still misses shows up at run time as "jump to unknown
     guest address". Pass `--entry 0xADDR`, or teach discovery the pattern.
2. **Lifting** (`lift.cpp`). Each x86 function becomes one C function.
   - **Registers and flags.** General registers and EFLAGS are C locals; the
     compiler removes dead flag computations.
   - **FPU and vectors.** x87 is modelled with doubles; SSE and MMX use unions.
   - **Calls.** Direct calls are C calls; indirect calls go through the
     runtime's dispatcher.
3. **Flag analysis** (`Lifter::analyze_flag_exports`):
   - *Flag exporters* are callees whose flags the caller reads, such as MSVC
     CRT x87 helpers.
   - *Flag importers* are functions that read flags their caller set, such as
     MSVC `/RTC`'s `_RTC_CheckEsp`. Their callers spill flags before the call,
     and the callee reloads them on entry.
4. **Natives and patches.**
   - Hot library routines, recognised by their bytes, are replaced with C
     versions in `runtime/src/natives.c`: zlib `inflate_fast`, a byte fill,
     and both BlazBlue sprite RLE expanders.
   - Byte patches fix behaviour that is wrong under the runtime, such as
     SSF4's frame-limiter spin.
   - `--no-natives` lifts the original code instead.
5. **Output** goes to `games/<id>/gen/`: `funcs_*.c`, `funcs.h`, `table.c`,
   and `report.txt` listing unsupported instructions. Data misread as code
   shows up there as `outsb` / `insd` / `aas`, which is harmless.

`tools/lifttest` runs the lifter against Unicorn as a differential test.

## Runtime (`runtime/`, C11)

| Part | Files |
|---|---|
| Guest memory: 4 GiB reserved, PE loading, import binding | `mem.c`, `loader.c` (`hle_modules[]` maps DLL names to tables) |
| Threads, sync, the dispatcher for indirect calls | `thread.c`, `dispatch.c`, `hle/k32_sync.c` |
| Win32 | `hle/k32_*.c`, `hle/user32.c`, `hle/gdi32.c`, ... |
| Direct3D 9, D3DX, effects | `hle/d3d9.c`, `hle/d3dx9.c`, `hle/d3dx_effect.c`, `gfx/fx.c` |
| Shader translation | `gfx/sm3_glsl.c` (bytecode → GLSL), `gfx/d3dasm.c` (assembly, ps_1_x → ps_2_0), `gfx/hlsl_glsl.c` (HLSL source) |
| Fixed-function pipeline | `gfx/ffp.c`: FVF → vertex declaration, vertex format and texture stage states → generated vs_2_0 / ps_2_0 bytecode (cached per state in `hle/d3d9.c`); its constants live in reserved registers (c248-c252, ps c222) that the game's own values are restored to before its next shader draw |
| Renderer | `gfx/gfx_gl*.c`: commands recorded per frame by the guest, replayed on the platform thread |
| Sound, movies | `hle/dsound.c`, `hle/dshow.c`, `video_ffmpeg.c` |
| Inputs | `jvs.c` (the board), `padmap.c` (pads, `X2_PAD`, `X2_INPUT`) |
| Front ends | `platform_sdl.c` (standalone), `platform_libretro.c` (core), `boot.c` (shared start-up) |
| Profiling | `prof.c` (`X2_PROF`), pacing / GPU timers in the platform layers |

HLE functions follow one pattern:
- signature `static void Name(X2Cpu *c)`;
- arguments read with `ARG(n)`, the result returned with `RET(v)`;
- listed with their stdcall argument count in the module's `X2Hle` table.

## Threads and frames

- **Guest threads.** Every guest thread is a host thread, and they run in
  parallel.
- **Frames.** The game's main thread records each frame. In the standalone
  app the platform thread replays it with the renderer (Metal, Vulkan or GL). In the core, `retro_run` both
  replays and clocks the guest, with one frame in flight: the guest computes
  frame N+1 while RetroArch shows frame N.
- **Ending a session.** Guest threads can't be torn down. On libretro unload
  they park at their next import call, so one RetroArch session runs one game.

## The libretro core

- **Linking.** Every game's generated code is partially linked into one
  object with localised symbols. A per-game `X2GameTables` descriptor names
  the game.
- **Choosing the game.** The core picks the game from the PE header of the
  `game.exe` it is given.

## Limits

- **Structured exceptions** (`RaiseException`, C++ `throw`) are not
  dispatched to the guest.
- **Memory ordering.** Guest threads run truly parallel. On ARM, shared
  (non-stack) guest stores are release stores (and loads acquire, with
  `X2_TSO=1`), so code relying on x86's ordering (the games' spinlocks) stays
  correct.
- **GDI text** on Android uses stb_truetype: heavy weights are emboldened by
  one pixel only, so they look lighter than on macOS (CoreText).
- **D3DX effects:** `Get*Array`, parameter blocks and `CloneEffect` are not
  implemented, and stop with a named error if a game calls them.

Per-game issues are on each game's page ([Game status](Game-Status.md)).
