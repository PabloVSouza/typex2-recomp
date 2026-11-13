# Super Street Fighter IV Arcade Edition

| | |
|---|---|
| Board | Type X2 (2010, Capcom) |
| `games/` id | `ssf4ae` |
| Status | **Playable** on macOS. **Playable, issues** in RetroArch on Android: a few stages run at 50-55 fps. The Android app isn't tested. |
| Executable | `game.exe` |

## Controls

Six buttons:

| Cabinet button | Move |
|---|---|
| SHOT1 | LP |
| SHOT2 | MP |
| SHOT3 | HP |
| SHOT4 | LK |
| SHOT5 | MK |
| SHOT6 | HK |

Default pad layout:

| Pad buttons | Moves |
|---|---|
| Y X R1 | LP MP HP |
| B A R2 | LK MK HK |
| L1 | all three punches |
| L2 | all three kicks |

On an 8-button stick whose rows are L1 Y X R1 / L2 B A R2, the arcade layout
is:

```
X2_PAD=l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456
```

## What it needed

- **Shaders.** D3D9 shader assembly (`gfx/d3dasm.c`), translated like
  bytecode.
- **Multiple render targets.** Without them the bloom was missing.
- **Network.** Winsock stubs report the network as down. This dump is the
  "stand-alone" build, so link play is off.
- **Frame limiter.** The game spins on `QueryPerformanceCounter` until 1/60 s
  has passed. Against RetroArch's own clock that lost frames, so a byte patch
  in `lift.cpp` skips the spin.
- **Audio thread.** The audio mixer runs at TIME_CRITICAL priority. Honouring
  `SetThreadPriority` puts it on the big cores, so the main thread waits less
  on its locks.
- **Android GPU work.**
  - Alpha test is only compiled into shader variants that need it, which keeps
    early-Z on: fights went from 15.0 to 11.6 ms of GPU time per frame.
  - fp16 pixel shaders.
  - RGB565 for opaque DXT1 textures.

## Known issues

- A few stages are GPU-bound at 50-55 fps on the Mali-G57 tablet.
- Link play is off: this is the stand-alone build.
- The opening movie's separate English audio track is missing from the dump.
