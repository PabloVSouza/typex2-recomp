# typex2-recomp documentation

typex2-recomp turns Taito Type X2 arcade games, which are 32-bit x86 Windows
XP programs, into native programs for macOS and Android. It is static
recompilation, not emulation:
- `x2recomp` translates the game's `game.exe` into C, once, ahead of time.
- A C runtime provides what the game expects from Windows: memory, threads,
  Win32, Direct3D 9, D3DX, DirectSound, DirectShow and the arcade I/O board.

Each game then runs in one of two ways:
- a standalone app (SDL3; launched by ES-DE on Android);
- a RetroArch core that holds every supported game.

## Pages

| Page | What it covers |
|---|---|
| [Game status](Game-Status.md) | Status of every Type X / X+ and Type X2 game, and the next candidates |
| [Hardware](Hardware.md) | The Type X and Type X2 boards, and how the runtime stands in for them |
| [Architecture](Architecture.md) | Recompiler and runtime internals |
| [Porting a game](Porting-a-Game.md) | Steps to bring up a new game |
| [Controls](Controls.md) | Keyboard, pads, `X2_PAD` / `X2_INPUT`, each game's layout |
| [Android and RetroArch](Android-and-RetroArch.md) | Building, installing, ES-DE, performance work |
| [Debugging](Debugging.md) | Methods that found past bugs, and the debugging switches |
| [Progress log](Progress-Log.md) | What changed, milestone by milestone |

Game pages:
- [KOF XII](Game-KOF-XII.md)
- [KOF XIII](Game-KOF-XIII.md)
- [KOF XIII Climax](Game-KOF-XIII-Climax.md)
- [SSF4 AE](Game-SSF4-AE.md)
- [BlazBlue CS](Game-BlazBlue-CS.md)
- [BlazBlue CT](Game-BlazBlue-CT.md)
- [KOF '98 UM](Game-KOF-98-UM.md)
- In progress: [KOF MI Regulation A](Game-KOF-MI-Regulation-A.md)

## Quick start

Build steps, run commands and the full list of `X2_*` switches are in the
repository's [README](../README.md). Rules for contributors and coding agents
are in [CLAUDE.md](../CLAUDE.md).

## Legal

The repository holds only the project's own code and documentation. You need
your own copy of each game. Game executables, assets, generated C code
(`games/*/gen/`) and screenshots are never committed.
