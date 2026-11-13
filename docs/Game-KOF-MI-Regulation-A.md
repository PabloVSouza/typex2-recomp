# The King of Fighters: Maximum Impact Regulation A

| | |
|---|---|
| Board | Type X2 (2007, SNK Playmore) |
| `games/` id | `kofmira` |
| Status | **In progress**: starts up to renderer creation, then needs Direct3D 8 |
| Executable | `game.exe` (PE32, 28 MB image; data in `diskimg/`) |

## Done

- **Recompiled:** 20,464 functions, 96% of the code.
- **C runtime as a DLL.** The game links `MSVCR80.dll` instead of carrying
  its C runtime inside the executable. `hle/msvcrt.c` implements the 93
  functions it imports:
  - memory and strings;
  - maths, including the x87-register `_CI*` helpers;
  - `stdio` files;
  - `printf` / `sscanf` over guest arguments;
  - `qsort` with the guest's comparator;
  - threads and CRT start-up.
  `_acmdln` and `_adjust_fdiv` are variables, which the loader now binds as
  data imports.
- **Multimedia file I/O:** `mmio*` (`hle/mmio.c`) reads its WAV files the way
  the DirectX SDK's `CWaveFile` does, including the buffered
  `mmioGetInfo` / `mmioAdvance` access.
- **Windows bits:** window and GDI stubs (debug text on the window DC),
  `lstr*`, `MoveFileExA`, `GetSystemMetrics`, `joyGetNumDevs`,
  `D3DXCreateTextureFromFileA`.
- **Start-up:** the game opens its JVS port, starts its threads and creates
  its window.

## Next: Direct3D 8

The game renders with Direct3D 8 (`Direct3DCreate8(220)`). Its Direct3D 9
init exists but is never called. The plan is a D3D8 layer over the existing
D3D9 implementation:
- the IDirect3D8 / IDirect3DDevice8 vtables;
- vertex shaders with D3D8 declaration tokens;
- sampler states that live among the texture stage states;
- `SetStreamSource` without an offset, and `SetIndices` with a base vertex;
- `CopyRects`;
- ps_1_x pixel shaders, which the translator already handles.

## Not supported in the C runtime

- `longjmp` (it stops with a named error).
- C++ / SEH exception dispatch.
