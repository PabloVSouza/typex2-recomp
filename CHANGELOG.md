# Changelog

Each release's section here becomes its GitHub release notes. The day-by-day
engineering history is in [docs/Progress-Log.md](docs/Progress-Log.md).

Versions follow [semantic versioning](https://semver.org); before 1.0 a minor
version may change how things work. Releases carry this project's code only:
games are recompiled on your computer from your own files.

## Unreleased

- **Android:** games run faster. Game code is now built with link-time
  optimisation, and calls between its parts are cheaper. **The King of
  Fighters XIII Climax** holds 60 fps in fights on every stage tested on
  the test tablet; before, most stages ran at about 50.
- **Save states in RetroArch:** Load State works with RetroArch's "Undo Load
  State" backup (on by default), which the core used to refuse silently.
  After a game's movie, saving and loading no longer pause the game for a
  second.

## 0.3.0 - 2026-11-16

Save states, and KOF XIII in English.

- **Save states**, in the standalone app (F5 saves, F7 loads) and in
  RetroArch. A state loads on the screen it was saved on, such as the same
  fight, until the game is closed. States hold the game's whole memory, so
  they are large (55-550 MB); see [Save states](docs/Save-States.md).
- **The King of Fighters XIII** starts in English. A language you saved in
  the test menu still applies.

## 0.2.1 - 2026-11-16

The license, and two fixes for the Android app.

- **License:** the project is now GPL-3.0-or-later. The libraries the builds
  include are listed with their licenses in THIRD-PARTY.md.
- **Android app:** game packs now go to `TypeX2/packs` on the device's
  internal storage, where Send to device and file managers can both put
  them (Android hides `Android/data` from file managers). Packs sent before
  the app had ever been opened were not installed.
- **Android app:** the update check works (it failed silently there).

## 0.2.0 - 2026-11-16

The launcher becomes the Android app, and games' code travels as packs.

**Updating the Android app: uninstall the old one first.** The new Type X2
app is signed with the project's release key, so Android refuses to install
it over an older Type X2 app. Your saves and settings are in your game
folders and stay.

1. On the device, uninstall **Type X2**: long-press its icon → App info →
   Uninstall (or Settings → Apps → Type X2 → Uninstall).
2. Install `typex2-android.apk` from this release and open it. Allow "All
   files access" when it asks: it reads your ROMs folder and writes each
   game's `x2.env`.
3. In the launcher on your computer, Export → **Game packs for the Android
   app** with your games, then **Send to device** (USB debugging on). Or
   copy the `x2-*.so` files to `Android/data/com.typex2.recomp/files/packs`
   on the device.
4. Open the app again: it installs the packs and lists your games. ES-DE
   launches them as before.

Later versions of the app update in place.

- **Android app:** the Type X2 app is now the launcher itself, with its
  library, game pages and settings, and comes from the releases page. It
  carries no game code: export **game packs** in the launcher on your
  computer and send them to the device. A new pack (after a runtime fix, or
  another game) needs no new app. The app finds your games in ES-DE's ROMs
  folder, writes each game's settings to its `x2.env` (ES-DE launches use
  them), and runs full screen.
- **Launcher:** games not supported yet are listed too, under their
  folder's name, as *Not supported yet*. The "Also supported" list is gone.
- **Launcher:** sends an Android export straight to a connected device: the
  game packs to the app, or the RetroArch core to RetroArch's downloads, ready
  for *Install or Restore a Core*. Android's platform tools (`adb`) download
  from Build tools when needed.
- **Launcher:** Log → Report a problem copies a report (version, system,
  games, logs; your home folder hidden) and opens a new GitHub issue for it.
  The Log page no longer goes blank in newer web engines.
- **RetroArch:** a closed game now leaves 1-3 MB behind (was 4-20).
- **Android (Vulkan):** BlazBlue Calamity Trigger no longer stalls briefly at
  character select while textures upload.

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
