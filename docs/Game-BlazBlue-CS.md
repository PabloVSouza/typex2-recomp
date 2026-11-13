# BlazBlue: Continuum Shift

| | |
|---|---|
| Board | Type X2 (2009, Arc System Works) |
| `games/` id | `bbcs` |
| Status | **Playable** on macOS and in RetroArch on Android: opening movie, arcade and versus at 60 fps. The Android app isn't tested. |
| Executable | `game.exe` (`d3dx9_31.dll`) |

## Controls

Four buttons, A B C D, on cabinet SHOT1-4. The INPUT DEVICE TEST calls them
BUTTON 1-4.

The default layout follows the arcade "Type B" panel:

| Pad buttons | Game buttons |
|---|---|
| Y X R1 | A B C |
| B | D |
| A | B+C (throw) |
| R2 | A+B (barrier) |
| L1 | A+B+C (rapid cancel) |
| L2 | A+B+C+D |

On an 8-button stick whose rows are L1 Y X R1 / L2 B A R2:

```
X2_PAD=l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234
```

## What it needed

- **Compiled D3DX effects.**
  - An fx_2_0 parser and a preshader VM (`gfx/fx.c`).
  - `ID3DXEffect` driving the device through its own vtable
    (`hle/d3dx_effect.c`).
  - `tools/fxdump` lists every effect embedded in the executable.
- **Shaders.** ps_1_x bytecode is rewritten to ps_2_0 before translation,
  with result-shift modifiers. vs_1/2 shaders declare every legacy output,
  because pixel shaders read unwritten ones.
- **`StretchRect`** as a GPU blit.
- **Opening movie.** The game plays it through its own DirectShow renderer
  filter. The runtime connects its RGB24 output pin to that filter, and a
  guest thread pushes the frames into the filter's input pin.
- **Recompiler.** The MSVC CRT's `_trandisp` handler tables become entry
  points.
- **Load pauses.**
  - A native sprite RLE expander, matched by its bytes and verified
    pixel-identical.
  - BGRA8 textures upload in the game's byte order and swap channels when
    sampled.
  - `UpdateTexture` uploads straight from the source.
  - The worst transition pause went from about 430 ms to about 230 ms.

## Known issues

- D3DX effect `Get*Array`, parameter blocks and `CloneEffect` are missing.
  They stop with a named error if a game calls them; BlazBlue doesn't.
- Effects don't save or restore device state around Begin / End. The game
  doesn't rely on it.
- Loading pauses of up to about 230 ms remain on the tablet, outside fights.
