# The King of Fighters XII

| | |
|---|---|
| Board | Type X2 (2009, SNK Playmore) |
| `games/` id | `kof12` |
| Status | **Playable** on macOS, the Android app and RetroArch on Android |
| Executable | `game.exe` (data in `gamediskimg.arc`, `media/`) |

## Controls

Four buttons, A B C D. On the cabinet they sit on SHOT1 (A), SHOT4 (B),
SHOT2 (C) and SHOT5 (D), not SHOT1-4, as KOF XII's I/O test shows. The
default pad is FinalBurn Neo's Neo Geo "Modern" layout, so FBNeo RetroArch
remaps work unchanged. See [Controls](Controls.md).

## What it needed

The first game of the project, so most of the runtime started here:
- Win32, D3D9, DirectSound, the JVS board on COM2 and the GL renderer.
- The "JVS Libraly 1.06" in this game sends `01 02` to node 0 at the start of
  every poll and waits for an answer (`jvs.c`).
- zlib 1.2.3 `inflate_fast` is replaced by a native routine (loading speed on
  Android).
- `tools/arc` unpacks `gamediskimg.arc` for inspection; the game reads the
  archive itself.

## Known issues

- On macOS, the left credit counter shows "0" where the right one shows
  "00". With the stb_truetype font backend both show "00", so the
  difference comes from CoreText's metrics.
