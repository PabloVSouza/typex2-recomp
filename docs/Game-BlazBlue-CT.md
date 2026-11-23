# BlazBlue: Calamity Trigger

| | |
|---|---|
| Board | Type X2 (2008, Arc System Works) |
| `games/` id | `bbct` |
| Status | **Playable** on macOS, and on Windows (a player's report with 0.6.5: Windows 10, NVIDIA RTX 4060 Ti, [#6](https://github.com/PabloVSouza/typex2-recomp/issues/6)). **Playable, issues** in RetroArch on Android: 60 fps in attract mode and fights, 0.3-0.7 s pauses at character select and fight start. The Android app isn't tested. |
| Executable | `game.exe`: the arcade `launcher.exe` with the save paths changed (`d:\` → `sv\`); the code is identical |

## Controls

Same panel and default layout as [Continuum Shift](Game-BlazBlue-CS.md). The
input test confirms SHOT1-4 = A B C D.

## What it needed

- **I/O board handshake.** Before JVS, the game talks to a controller on the
  I/O board: packets to address 0 with commands 0-8. It takes a status byte
  and one or two report bytes per command. Without an answer it shows
  "I/O ERROR" forever. `jvs.c` answers until the board has a JVS address.
- **A self-suspending I/O thread.** The game's I/O thread suspends itself
  every frame and the main thread resumes it, so `SuspendThread` on the
  calling thread must block.
- **MSVC run-time checks.** This build was compiled with `/RTC`.
  `_RTC_CheckEsp` reads ZF from the code that called it. The recompiler's
  "flag importers" make the call site spill its flags and the callee reload
  them. A failed check ends in DebugBreak, reported as "the game called
  DebugBreak".
- **Discovery.** It no longer loops forever on rejected gap candidates, which
  only this executable triggered.
- **Sprite RLE native.** CT's own version of Continuum Shift's sprite
  decoder, which only knows 4- and 1-byte pixels; pixel-identical.
- **Effects.** Texture parameters start out NULL, as in D3DX. CT sometimes
  begins a pass before its loader has set a texture. That used to pass the
  effect's object index as a texture pointer, and crashed on the tablet at
  fight start.

## Known issues

- On the tablet the fight start still pauses for 0.3-0.6 s, depending on the
  stage: the game loads the fight (~900 textures, over 100 MB) in one frame.
  Most of it is now the game's own work; the renderer adds under 0.1 s (it
  added 0.25 s, on top of a longer loading frame): texture memory is allocated
  while the game loads, queued uploads no longer copy their data, and the
  command buffer grows without copies.
