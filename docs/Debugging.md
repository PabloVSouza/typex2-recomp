# Debugging

The full table of `X2_*` switches is in the [README](../README.md#debugging-switches-environment-variables).
This page collects the methods that found real bugs.

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
