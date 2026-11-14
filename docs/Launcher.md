# Launcher

The launcher is a desktop app for macOS, Linux and Windows. It turns the games
you own into native builds, without a terminal:

- **for this computer**, to play them from the launcher;
- **for other platforms**: the RetroArch core for Android, the Android app,
  or the RetroArch core for this computer.

It is a Tauri app (Rust back end, React front end) in `launcher/`.

## What it does

| Page | |
|---|---|
| Library | Add game folders. A game is recognised from its `game.exe` / `launcher.exe` (PE image size and entry point), so the folder's name does not matter. |
| Game | Build or rebuild for this computer, Play / Stop, display options (full screen, vsync, FPS counter, render scale), the controller layout (presets per game, or remapped button by button: Assign, then press the controller button; saved as an `X2_PAD` line), log detail and extra `X2_*` switches. |
| Export | Pick a target and games; the result lands in the launcher's `exports/<target>/` folder. |
| Build tools | What building needs, where each tool comes from, downloads and their removal. |
| Log | The game's output and the build tools' output. |
| About | The launcher's language, the installation (version, kit, data folder). |

## How a game is built

1. **Generate:** `x2recomp` turns the executable into C in the launcher's
   data folder (`work/games/<id>/gen`). This step is redone only when the
   executable or the kit changes. The kit's identity is the recompiler
   binary plus `recomp.h`.
2. **For this computer:** the generated C is compiled with the flags CMake
   uses for generated code, then linked into one library (`game.dylib` /
   `.so`). `x2-player`, the runtime built as an executable, loads it with
   `--game-lib`:
   - The library exports one symbol, `x2_game_tables`: the generated tables
     under new names, as the multi-game builds do.
   - On macOS it is linked as a bundle against `x2-player`, so runtime symbols
     (thread-locals included) bind to the player at link time.
   - KOF '98 UM takes about 7 seconds to compile on an M3 Pro, and its frames
     are pixel-identical to the CMake executable's.
3. **Exports:** the project's own CMake build over the generated code
   (`-DX2_GAMES_DIR`, `-DX2_GAMES`). For Android it uses the NDK toolchain.

## Build tools, kept inside the launcher

The launcher downloads pinned versions into its own `tools/` folder, checks
them against their published checksums, and removes them in one click. Builds
then run with only those tools and the OS's base folders on `PATH`.

| Tool | Version | For | Checksum source |
|---|---|---|---|
| LLVM clang + ld64.lld (macOS arm64) | 23.1.4, trimmed | Playing on this computer | SHA-256, pinned |
| CMake | 4.5.0 | Exports | SHA-256 from the GitHub release |
| Ninja | 1.13.2 | Exports | SHA-256 from the GitHub release |
| Android NDK | r30 | Android exports | SHA-1 from Google's SDK repository manifest |

The downloaded clang has no OS SDK:
- Generated code compiles against clang's own headers plus small stand-ins for
  `math.h` and `string.h` (`runtime/include/sdkless/`).
- The C library's functions are looked up when the player loads the game.

Without downloads, tools already installed are used instead (Xcode Command
Line Tools, CMake, Ninja, an NDK found under the Android SDK).

## What a release contains

The app, `x2recomp`, `x2-player`, the libraries those two link, and the
runtime sources that exports build from. `launcher/scripts/stage-kit.sh`
gathers these into `src-tauri/kit`.

A release contains no game code or data. Everything generated from a game
stays in the launcher's data folder, apart from the exports the user makes.

## Status

- **macOS (Apple Silicon):** verified.
  - Building with the downloaded clang: KOF '98 UM.
  - Exporting the Android RetroArch core with the downloaded CMake, Ninja and
    NDK.
- **Linux:** untested.
- **Windows:** untested, and there is no `x2-player` yet, so Windows can
  export but not play.
- **Android app export:** needs the Android SDK's build tools next to the NDK.

## Developing

```sh
cd launcher && npm install
npm run tauri dev       # uses the checkout's build/recompiler/x2recomp and build/x2-player
npm run dev             # the pages in a browser with mock data (#tools, #export, #game:kof13)
cargo run --manifest-path src-tauri/Cargo.toml --example cli -- <data dir> tools llvm
cargo run --manifest-path src-tauri/Cargo.toml --example cli -- <data dir> library "<game dir>"
cargo run --manifest-path src-tauri/Cargo.toml --example cli -- <data dir> export android-retroarch "<game dir>"...
```

Before committing: `npm run check` (types, ESLint, locales) and `npm run build`;
`cargo clippy` in `src-tauri`. CI runs the front-end checks.

### Front end conventions

ESLint (`eslint.config.js`) enforces what it can:

- **One component per file** (`react/no-multi-comp`); shadcn's generated
  `src/components/ui` is exempt.
- **Imports go through the `@/` alias** (`src/`), never relative paths.
- **Every folder has an `index.ts`** exporting its contents. Code outside a
  folder imports the folder (`@/components/common`, `@/hooks`, `@/lib`); files
  inside it import their siblings by full path (`@/pages/game/ActionRow`).
- **A folder holds a feature and its parts:** `pages/<page>/` has the page and
  its rows and sections, `components/sidebar` the sidebar and its items,
  `components/games` the Build / Play button and the status badge.
- **State:** backend data through TanStack Query (`src/hooks`: `useAppState`,
  `useAction` for commands, `useEvents` for the backend's events); client state
  in Zustand stores (`src/stores`: the page shown, the job's progress and the
  logs); component state in `useState`.
- **All text comes from i18n** (`src/i18n/locales/*.json`, react-i18next).
  `en.json` is the reference and its keys are typed (`i18next.d.ts`), so a
  missing key is a type error; a missing translation falls back to English.
  `scripts/check-locales.mjs` checks every locale against it (keys, `{{values}}`
  and tags). Names the backend sends (layouts, export targets, tools) are
  translated by id in `src/lib/data/names.ts`; the backend's error messages are
  English.
- **UI components come from shadcn** (`npx shadcn@latest add <name>`, then
  export them from `src/components/ui/index.ts`).

Languages: English, Spanish, French, Portuguese (Brazil and Portugal), German,
Japanese, Korean and Simplified Chinese. The system's language is used unless
one is chosen in About; a new language is a file in `locales/` plus a line in
`LAUNCHER_LANGUAGES` (`src/i18n/i18n.ts`).
