# Changelog

Each release's section here becomes its GitHub release notes. The day-by-day
engineering history is in [docs/Progress-Log.md](docs/Progress-Log.md).

Versions follow [semantic versioning](https://semver.org); before 1.0 a minor
version may change how things work. Releases carry this project's code only:
games are recompiled on your computer from your own files.

## Unreleased

- **Launcher:** sends an Android export straight to a connected device: the
  app installed, or the RetroArch core copied, ready for RetroArch's *Install
  or Restore a Core*. Android's platform tools (`adb`) download from Build
  tools when needed.
- **Launcher:** Log → Report a problem copies a report (version, system,
  games, logs; your home folder hidden) and opens a new GitHub issue for it.
  The Log page no longer goes blank in newer web engines.
- **Android app:** opened from the home screen, it lists your games and sets
  each one's FPS counter, pad layout and touch pad, then plays it; no more
  editing `x2.env` by hand. ES-DE still starts games directly.
- **RetroArch:** a closed game now leaves 1-3 MB behind (was 4-20).

## 0.1.1 - 2026-11-16

Smoother games, RetroArch Reset, and a launcher that tells you about updates.

**Frame pacing:** steadier in every game. The games' own frame
timers no longer fight the runtime's, and their worker threads no longer
wake each other for nothing. BlazBlue's late frames dropped by half or more.

**Android:**

- RetroArch: Reset restarts the game.
- BlazBlue Calamity Trigger: a shorter pause at fight start.
- KOF XII: bold text is bold again.
- Requires Vulkan 1.3 (most devices from 2022 on, with Android 13 or later).
  OpenGL ES is no longer a supported fallback.

**macOS:** KOF XII's left credit counter shows "00" again.

**Launcher:** tells you when a new version is out, with the release notes
and a link to the downloads (About → Updates turns it off).

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
