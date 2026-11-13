# The King of Fighters: Maximum Impact Regulation A

| | |
|---|---|
| Board | Type X2 (2007, SNK Playmore) |
| `games/` id | none yet |
| Status | **Planned** |
| Executable | `game.exe` (PE32, 28 MB image; data in `diskimg/`) |

## What we know

Imports:

| DLL | Status |
|---|---|
| `d3d9.dll`, `d3dx9_29.dll` | supported (new D3DX alias) |
| `d3d8.dll` | new: Direct3D 8 |
| `DDRAW.dll` | partly there |
| `DINPUT8.dll` | partly there |
| `DSOUND.dll`, `WINMM.dll`, `ole32.dll` | supported |
| `MSVCR80.dll` | new |

The MSVC 8 C runtime is a DLL here, not linked into the executable as in
the other games. Two options:
- implement its exports as HLE functions;
- recompile the DLL alongside the game.

The folder also ships `d3dx9_24.dll`, which may be loaded at run time.

## Plan

Find out what it uses `d3d8.dll` for: possibly just a capability probe.
After that, the biggest piece of work is the CRT DLL.
