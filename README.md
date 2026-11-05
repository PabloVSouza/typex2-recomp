# typex2-recomp

Static recompilation of Taito Type X2 games (SNK's KOF XII / XIII) to native
code, so they run on macOS, Android and other platforms. ES-DE is the target
frontend; a RetroArch/libretro core is the long-term goal.

**Status:** KOF XII boots, runs its full attract loop (logos, title, demo
battles) and is playable from coin-in to character select. Rendering is
D3D9 → OpenGL 3.3 (D3D9 shader bytecode and runtime HLSL both translated to GLSL).

## How it works

```
game.exe ──x2recomp──► games/kof12/gen/*.c ──clang──┐
                                                    ├─► x2-kof12 (native)
runtime/ (C): memory, threads, Win32/D3D9/DSound,  ─┘
              JVS I/O, GL renderer, SDL3 platform
```

* **recompiler/** (C++20, capstone): recursive-descent discovery of every
  function, then x86 → C lifting, one C function per guest function. GPRs and
  flags are C locals (dead flags vanish under clang); x87 is modelled with
  doubles; SSE/MMX on unions.
* **runtime/**: 4 GiB guest address space, dispatcher for indirect calls,
  host implementations of every import (kernel32, user32, gdi32, advapi32
  crypto, ole32), Direct3D 9 / D3DX / DirectSound / DirectShow as host COM
  objects, an emulated JVS I/O board on COM2, and an SDL3 + OpenGL platform.
  The guest runs on its own threads; the host main thread only replays the
  recorded frames, so a libretro `retro_run()` can drive it later.
* **tools/**: `arc` (unpack `gamediskimg.arc`), `kofarc` (list/extract KOF XIII `media.kof`, decrypting TXAC entries with the game's `TypeXmulti/txac` key), `lifttest` (differential test
  of the lifter against Unicorn on real instructions from the game).

## Build

```sh
brew install capstone sdl3            # unicorn too, for lifttest
cmake -S . -B build -G Ninja
cmake --build build --target x2recomp
build/recompiler/x2recomp "<game dir>/game.exe" games/kof12/gen
cmake -S . -B build && cmake --build build
```

## Run

```sh
build/x2-kof12 "<KOF XII folder>"        # add -f for fullscreen, -v/-vv/-vvv for logs
```

| Keys | |
|---|---|
| arrows / Z X C V | move / A B C D |
| 1, 2 | start P1, P2 |
| 5, 6 | coin P1, P2 |
| F2 / F1 | test / service |
| F10 | FPS overlay (game fps, display fps, guest CPU ms per frame) |
| F11 / F12 / Esc | fullscreen / screenshot / quit |

Gamepads map to players 1–2; Back = coin, Back+Start quits (for ES-DE).

## Debugging switches (environment variables)

| Variable | Effect |
|---|---|
| `X2_SHOT=f1,f2,...` `X2_SHOT_PATH=x.png` `X2_SHOT_EXIT=1` | save frames as PNG |
| `X2_SHOT_WINDOW=1` | screenshots capture the window (with the overlay) |
| `X2_FPS=1` | start with the FPS overlay on |
| `X2_INPUT="600:c,606:-,700:s"` | scripted input (c coin, s start, udlr, abxy) |
| `X2_DRAWLOG=<frame>` | log every draw of one frame |
| `X2_SHADERLOG=<id>` / `X2_DUMPTEX=<id>` | print a translated shader / dump a texture |
| `X2_BT=<secs>` `X2_PEEK=addr,...` | periodic guest backtraces / memory peeks |
| `X2_TRACE_READS=1` | log file reads (pipe offsets into `arc locate`) |
| `X2_CALL="addr:args"` (+ `X2_POKE`, `X2_CALL_DELAY`) | call a guest function |
| `X2_GIL=1` | serialize guest threads |
| `X2_VSYNC=1` | vsynced presentation |
| CMake `-DX2_FN_TRACE=ON` / `-DX2_WATCH=ON` + `X2_WATCH=lo-hi` | function-entry rings / store watchpoints |

## Known gaps

* Structured exceptions (`RaiseException` / C++ `throw`) are not dispatched.
* Guest threads run truly parallel. On ARM, shared (non-stack) guest loads
  and stores are acquire/release so x86 TSO-dependent code (the game's
  spinlocks) stays correct (`-DX2_TSO=0` turns this off).
* KOF XII: the left credit counter shows "0" where the right shows "00"
  (Scaleform text; cause unknown).
* KOF XIII Climax: boots to D3D device creation; the TtxAppCrypt pipe service
  (TXAC script decryption) is implemented in `runtime/src/txac.c` but not yet
  wired into CreateFile/CryptImportKey.
* Android / libretro builds are not wired up yet (the code is structured for them).
