# The King of Fighters XIII

| | |
|---|---|
| Board | Type X2 (2010, SNK Playmore) |
| `games/` id | `kof13` |
| Status | **Playable** on macOS, the Android app and RetroArch on Android |
| Executable | `game.exe` (unpacked data folders: `bg/`, `fighter/`, `ui/`, ...) |

## Controls

Same panel as [KOF XII](Game-KOF-XII.md): A B C D on cabinet SHOT1, SHOT4,
SHOT2 and SHOT5 (checked in XIII's order select).

## What it needed

- zlib `inflate_fast` native, shared with XII and Climax.
- GDI text, rendered with CoreText on macOS and stb_truetype on Android.
  The stb_truetype backend finds a CJK font, since the game defaults to
  Japanese.

## Known issues

- The game starts in Japanese. Switch the language in the test menu: F2,
  hold Select / Back on a pad, or TEST on the touch pad.
- With stb_truetype (Android), heavy font weights are emboldened by one
  pixel only, so text looks lighter than on macOS.
