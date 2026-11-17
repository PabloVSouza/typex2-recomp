# Samurai Shodown: Edge of Destiny

Samurai Shodown: Edge of Destiny (サムライスピリッツ閃, *Samurai Spirits
Sen*), SNK Playmore, 2008, Taito Type X2. `games/sse`.

**Status: boots** (macOS). Attract mode with its movies, coin and start,
character select, the story screens and fights all run, in the game's HD
mode (1280×720) at 60 fps. The 3D scene of a fight renders far too dark (see
[Known issues](#known-issues)), so it is not playable yet. Not on Android
yet.

## Files

The game folder needs `game.exe`: the runtime recompiles it, not
`launcher.exe`. The meshes and textures are in `Bin/` (`.K2MP` archives), the
compiled effects in `Shader/`, the movies in `Bin/MOVIE` (WMV; `J_SD` holds
the SD versions).

## What it needed

- **HD mode.** The game reads system switch bit 0 of the I/O board at
  start-up as the cabinet's monitor setting: set, it runs at 1280×720 with
  the HD movies; clear, at 640×480. The emulated board sets it for this
  game, and the display modes offered include 1280×720.
- **The I/O board start-up.** Common copies of `game.exe` have the call that
  sets up the board's polling (0x40117a in `main`) and the wait for its first
  reply replaced with NOPs, so `main` read the switches before any poll and
  the game always ran in SD. The recompiler puts the original instructions
  back when it recognises those bytes (`kPatches` in `lift.cpp`).
- **`.X` meshes.** `D3DXLoadMeshFromXInMemory` reads binary `.X` files
  (templates, normals, texture coordinates, vertex colours, materials,
  `DeclData`) into an `ID3DXMesh` (`hle/d3dx_mesh.c`).
- **Direct3D enumeration.** The game's DXUT-style enumeration drops a device
  whose caps have `D3DPMISCCAPS_NULLREFERENCE`; the caps no longer set it,
  and only the HAL device exists.
- **DirectInput.** `joyGetNumDevs` returns 16 (the joysticks the driver
  supports), as Windows does; 0 failed the game's DirectInput set-up.
- **DirectSound notifications.** The game asks a buffer for
  `IDirectSoundNotify`, releases it, and asks again: the notify interface
  now shares the buffer's reference count, as in DirectSound, instead of
  being freed and handed out again.
- **Swap chains.** `GetSwapChain` / `GetNumberOfSwapChains`: the device's one
  chain.
- **Shadow maps.** The fight scene samples a depth texture, which on the
  Type X2's NVIDIA GPU returns a filtered depth comparison. Metal does the
  same (`sample_compare` in a shader variant per sampler bound to a depth
  texture). Vulkan does not yet.
- Smaller HLE: an empty registry (keys not found), `GetAdaptersInfo`, window
  placement and hot keys, memory DCs, D3DX maths (`D3DXMatrixLookAtRH`,
  `D3DXMatrixPerspectiveFovRH`, `D3DXMatrixAffineTransformation`,
  quaternions, `D3DXVec3Hermite`...), `D3DXFillTexture`, `D3DXFrameFind`,
  `D3DXCreateEffectFromFileA`, the non-Ex texture loaders.

The game opens `Dic32.dll`, a smart-card reader client, only from code that
this `game.exe` does not run.

## Known issues

- **The 3D scene of a fight is nearly black.** The stage and characters
  draw, but the main pass's result is very dark; the HUD, the 2D screens and
  the movies look right. Under investigation.
- **Frame pacing:** the game's own limiter measures its frame from
  `Present`'s return and sleeps the rest of 1/60 s, so a frame takes a
  little over 16.7 ms (about one late frame in 120).
- The button layout (`X2_PAD` line) is not worked out yet.
