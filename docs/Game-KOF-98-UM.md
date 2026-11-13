# The King of Fighters '98 Ultimate Match

| | |
|---|---|
| Board | Type X (2008, SNK Playmore): the first Type X game in the project |
| `games/` id | `kof98um` |
| Status | **Playable** on macOS: attract mode, coin / start, team select and fights with full stages and sound. Android not tested yet. |
| Executable | `launcher.exe`. The runtime looks for `game.exe`, then `launcher.exe`. |

## Controls

Same panel and default layout as [KOF XII](Game-KOF-XII.md). The game's INPUT
TEST confirms A B C D on cabinet SHOT1, SHOT4, SHOT2 and SHOT5. The test menu
opens with the test button (F2, or hold Select).

## What it needed

- **Fixed-function graphics.** The game draws partly with Direct3D's
  fixed-function pipeline (`SetFVF` and texture stage states, no shaders).
  `gfx/ffp.c` generates the equivalent vs_2_0 / ps_2_0 shaders, which then
  take the normal shader path. See [Architecture](Architecture.md).
- **Pre-transformed vertices skip the vertex shader.** The stages are drawn
  with `SetFVF(XYZRHW | DIFFUSE | TEX1)` while the game's own vertex shader is
  still bound. In D3D9, pre-transformed (XYZRHW) vertices bypass vertex
  processing, so the shader's matrix, which maps a 320-wide space, must not
  apply to these 640×480 pixel positions.
- **Palettized graphics.** Sprites and stage tiles are L8 index textures,
  looked up in a 16×256 A4R4G4B4 palette by the game's pixel shaders.
- **Short-lived threads.** Every sound timer is a one-shot `timeSetEvent`
  re-armed each 16 ms, and each one is a thread. Thread stacks are now freed
  when a thread ends; before, the address space ran out after about 30
  seconds.
- **PulseEvent.** The sound threads wait for the timer's `PulseEvent`. The
  old set-yield-reset version starved them, so there was no sound. Waiters
  now compare a pulse count.
- **Windows bits.** `PostMessageA`, `wsprintfA`, `GetMessageA`, painting and
  dialog stubs (an About box behind a menu nobody can open),
  `SetCurrentDirectoryA`, the `d3dx9d_36.dll` debug D3DX alias,
  `D3DXGetShaderVersion`.

## Known issues

- Not yet tested on the Android tablet.
- The game re-uploads its palette, all nine mip levels, every frame. That's
  harmless on macOS; its cost on the tablet is still to be measured.
