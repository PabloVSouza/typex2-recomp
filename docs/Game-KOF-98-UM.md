# The King of Fighters '98 Ultimate Match

| | |
|---|---|
| Board | Type X (2008, SNK Playmore) |
| `games/` id | none yet |
| Status | **Planned** |
| Executable | `launcher.exe` (PE32, 25 MB image) |

## What we know

- **Imports.** `d3d9.dll`, `d3dx9d_36.dll` (the debug D3DX build),
  `DSOUND.dll`, `WINMM.dll`, `GDI32.dll`, `USER32.dll`, `KERNEL32.dll`. All
  these families are already implemented; `d3dx9d_36` should be an alias in
  `hle_modules[]`.
- **Extra files.** The folder also holds `dinput8.dll`, `d3d9d.dll` and the
  MSVC 8 runtime DLLs. The executable doesn't import them directly. Find out
  what loads them before implementing anything.
- **First Type X game.** I/O may not be JVS on COM2 as on Type X2.

## Plan

Follow [Porting a game](Porting-a-Game.md). Once it boots, the first thing to
check is how it reads its inputs.
