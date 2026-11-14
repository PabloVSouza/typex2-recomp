# Changelog

Each release's section here becomes its GitHub release notes. The day-by-day
engineering history is in [docs/Progress-Log.md](docs/Progress-Log.md).

Versions follow [semantic versioning](https://semver.org); before 1.0 a minor
version may change how things work. Releases carry this project's code only:
games are recompiled on your computer from your own files.

## 0.1.0 - 2026-11-14

The first release.

**Games** (Taito Type X / Type X2):

- The King of Fighters XII, XIII and XIII Climax
- Super Street Fighter IV Arcade Edition
- BlazBlue: Calamity Trigger and Continuum Shift
- The King of Fighters '98 Ultimate Match
- The King of Fighters Maximum Impact Regulation A (Android: attract mode only
  tested so far)

**Platforms:**

- macOS (Apple Silicon): games play in the launcher, rendered with Metal.
- Android: a RetroArch core holding the chosen games, or a standalone app
  that ES-DE can launch, rendered with Vulkan (OpenGL ES as a fallback).
  Built by the launcher from your games.
- Linux and Windows: the launcher runs and builds the Android exports; playing
  games on these systems is new in this release and not yet tested with games
  (Vulkan, with OpenGL as a fallback).

**Launcher:** add the folders of the games you own, recompile them, play them
on this computer, or export the Android core or app. Build tools (clang,
CMake, Ninja, the Android NDK) are downloaded on demand into the launcher's own
folder. In English, Spanish, French, Portuguese (Brazil and Portugal), German,
Japanese, Korean and Simplified Chinese.

**Controls:** gamepads and arcade sticks with per-game layouts (`X2_PAD`), remapped in the launcher by pressing the buttons,
on-screen touch controls on Android with each game's buttons.

**Known issues:** BlazBlue Calamity Trigger pauses for about 0.25 s at fight
start on slower Android devices. RetroArch must be restarted to switch games.
No save states.
