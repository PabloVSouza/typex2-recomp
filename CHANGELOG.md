# Changelog

Each release's section here becomes its GitHub release notes. The day-by-day
engineering history is in [docs/Progress-Log.md](docs/Progress-Log.md).

Versions follow [semantic versioning](https://semver.org); before 1.0 a minor
version may change how things work. Releases carry this project's code only:
games are recompiled on your computer from your own files.

## 0.6.4 - 2026-11-21

- **Launcher on Windows:** compiling a game no longer fails with
  `'x2/recomp.h' file not found`. The launcher handed the compiler its own
  folder in a form (`\\?\C:\...`) where Windows doesn't read `/` as a folder
  separator.

## 0.6.3 - 2026-11-20

- **Android app:** "Allow access" opens Android's "All files access" page again
  (it failed with "Java exception was thrown" on every device,
  [#4](https://github.com/PabloVSouza/typex2-recomp/issues/4)); where a device
  lacks that page, it opens the list of apps with that access, or the app's info page.

## 0.6.2 - 2026-11-19

- **Vulkan (Android):** SSF4's small text (CREDIT(S), INSERT COIN(S)) no longer shows
  stray marks around the letters: a texture filled a piece at a time starts out
  zeroed, as with OpenGL and Metal, not holding what a freed texture left.
- **RetroArch:** `X2_PADLOG` logs directions too.

## 0.6.1 - 2026-11-19

- **RetroArch on Android:** two buttons pressed together (SSF4's Focus Attack,
  throws) need RetroArch's Input Block Timeout at 2 ms; the docs and known
  issues say how.

- **Launcher on Android:** the controller layout's Assign reads every button
  of controllers Android doesn't know, such as arcade sticks, including L2 and
  R2 when they are triggers. Before, those buttons couldn't be assigned, and
  sometimes no button registered at all.

## 0.6.0 - 2026-11-19

A launcher you can use from the couch: game art, controller navigation and a
full-screen mode.

- **Launcher:** game art on the library cards and the game page (the
  user's own ES-DE media, else the console port's image from libretro's
  thumbnail server, fetched once); the board, year, publisher and status as
  icons; on narrow screens (a tablet held upright) the sidebar becomes an
  icon rail and the content gets the width. Refresh reloads the art.
- **Launcher:** controller navigation: the d-pad or stick moves between
  controls, A plays the focused game, X opens its page, B goes back. While
  a controller is in use the focused control stands out (the others fade)
  and stays in the middle of the page as it scrolls.
- **Launcher:** full-screen mode: the button by the logo slides the menu
  away and the library takes the width. A controller switches to it; left
  from the page's edge or Y shows the menu over the page, and going back to
  the page hides it.
- **Launcher on Android:** runs at the display's fastest refresh rate
  (90 Hz on the test tablet); the games keep 60.

## 0.5.0 - 2026-11-19

Samurai Shodown: Edge of Destiny at 60 fps on Android, OpenGL as a full
fallback renderer, and a better layout on upright screens.

- **Samurai Shodown: Edge of Destiny** on Android runs in the game's own SD
  mode (640×480, 4:3) by default: fights at 60 fps on the test tablet,
  where HD showed 35-40. `X2_HD=1` in the game's `x2.env` chooses HD, and
  `X2_HD=0` SD on a computer.
- **Samurai Shodown: Edge of Destiny:** the floors are lit all over, not
  only in a square around the fighters, with no dark shape moving over the
  background. The game's own frame limiter is
  off (the runtime paces it), so it no longer costs RetroArch frames.
- **OpenGL / OpenGL ES** stays as the renderer for devices without Vulkan
  1.3, with the same features as Vulkan and Metal: shadow maps (Samurai
  Shodown: Edge of Destiny) now render on it too, and RetroArch's Restart
  works with it as with Vulkan.
- **`X2_SCENE_SCALE`** (opt-in, all renderers): draws a game's 3D scene and
  its effects at a fraction of their size, the HUD at full size. Samurai
  Shodown: Edge of Destiny in HD holds 60 fps on the test tablet at `0.75`.
- **Android, held upright:** the picture sits at the top and the touch
  stick and buttons get the space below it, side by side, instead of
  overlapping each other and the picture.
- **Android (Vulkan):** the picture is drawn in one pass. A block of
  coloured garbage beside 4:3 pictures (on Mali GPUs) is gone, and so,
  very likely, are the single black frames that made movies blink.

## 0.4.0 - 2026-11-18

Samurai Shodown: Edge of Destiny starts working, and clearer build errors.

- **Samurai Shodown: Edge of Destiny** (in progress): recompiles and runs
  in its HD mode, from attract mode to fights, with its stages, toon-shaded
  fighters and their shadows, on macOS and Android. It is in the launcher and
  the Android app, with BlazBlue's pad layout (R in place of D). See its
  [page](docs/Game-Samurai-Shodown-EoD.md).
- **Launcher:** when recompiling fails, the notification shows the
  compiler's last lines and its exit code, not only "Compiling ... failed:"
  (the full output is in Log).

## 0.3.1 - 2026-11-17

Faster on Android, and Load State fixed in RetroArch.

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
