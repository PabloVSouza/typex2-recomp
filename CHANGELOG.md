# Changelog

Each release's section here becomes its GitHub release notes. The day-by-day
engineering history is in [docs/Progress-Log.md](docs/Progress-Log.md).

Versions follow [semantic versioning](https://semver.org); before 1.0 a minor
version may change how things work. Releases carry this project's code only:
games are recompiled on your computer from your own files.

## Unreleased

- **Launcher:** tells you when a new version is out (checks GitHub's
  releases at start and every six hours; off in About → Updates), with the
  release notes and a link to the downloads.
- **KOF XII:** both credit counters show "00" on macOS (the left one showed
  "0"), and bold text is bold on Android (it came out in the regular weight).
- **BlazBlue Calamity Trigger, Android:** a shorter pause at fight start (the
  renderer's part went from 0.25 s to under 0.1 s, and the game's loading
  frame is faster); loading screens elsewhere benefit too.
- **RetroArch:** Reset restarts the game (with Vulkan, the default renderer).

## 0.1.0 - 2026-11-15

The first release.

**Games** (Taito Type X / Type X2):

- The King of Fighters XII, XIII and XIII Climax
- Super Street Fighter IV Arcade Edition
- BlazBlue: Calamity Trigger and Continuum Shift
- The King of Fighters '98 Ultimate Match
- The King of Fighters: Maximum Impact Regulation A

**Platforms:**

- macOS (Apple Silicon): play in the launcher, rendered with Metal.
- Android: a RetroArch core with your games (switch games without
  restarting RetroArch), or a standalone app ES-DE can launch. Vulkan, with
  OpenGL ES as a fallback.
- Linux and Windows: the launcher builds the Android exports; playing there
  is untested, so reports are welcome.

**Launcher:** add your game folders (or scan a folder of them), recompile
them on your computer, play, or export for Android. Build tools download on
demand. In 9 languages.

**Controls:** gamepads and arcade sticks with per-game layouts, remapped by
pressing the buttons; touch controls on Android.
