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
  The stb_truetype backend finds a CJK font, for Japanese text.
- English by default: the recompiler patches the owner settings' factory
  default (`0x43faf0`) from Japanese to English. A language chosen in the
  test menu (SYSTEM SETTINGS → LANGUAGE) is saved and still wins; a copy that
  already saved Japanese keeps it until changed there.

## Known issues

- None known.
