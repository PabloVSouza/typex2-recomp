# Building from source

Most players don't need this: the [launcher](Launcher.md) builds everything
from a release. This page is for development: the recompiler, the runtime and
the games built from a checkout.

## Desktop (macOS, Linux, Windows)

macOS:

```sh
brew install capstone sdl3 ffmpeg ninja pkg-config     # unicorn too, for tools/lifttest
cmake -S . -B build -G Ninja
cmake --build build --target x2recomp
build/recompiler/x2recomp "<game dir>/game.exe" games/kof12/gen
cmake -S . -B build && cmake --build build
```

Linux (Ubuntu 24.04; other distributions need the same packages): see the
`linux` job in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) for
the package list. Configure with `-DX2_FETCH_CAPSTONE=ON` where the system's
capstone is older than 5. SDL3 is built from the pinned release when the
system has none.

Windows: MSYS2's CLANG64 environment with the packages listed in the
`windows` job of the same file. The Windows port builds in CI but has not run
a game yet.

Games are recompiled into `games/<id>/gen`: `kof12`, `kof13`, `kof13cl` (XIII
Climax), `kof98um`, `kofmira`, `ssf4ae`, `bbcs`, `bbct` (BlazBlue Continuum
Shift / Calamity Trigger). KOF '98 UM starts from `launcher.exe`: give
x2recomp that file; the runtime finds either name. Generated code is never
committed (it is the game's code).

The build makes `build/x2-<game>` for every `games/<game>/gen`, the RetroArch
core `build/typex2_libretro.dylib` / `.so` (`-DX2_LIBRETRO=OFF` /
`-DX2_STANDALONE=OFF` to skip either), and `build/x2-player`, the launcher's
host (`x2-player <game dir> --game-lib <library>`). `-DX2_GAMES_DIR=<folder>`
takes the generated code from another folder than `games/`.
`-DX2_GEN_LTO=ON` builds the generated code with ThinLTO (the default on
Android, where it saves 10-30 % of the game thread's time). Each game's LTO link
takes every core and several GB, so the links run one at a time.

Renderers: Metal on macOS, Vulkan on Linux and Windows (with OpenGL as the
fallback); `X2_RENDERER=gl` selects OpenGL anywhere.

## Running

```sh
build/x2-kof12 "<KOF XII folder>"     # -f fullscreen, -v / -vv / -vvv logs
build/tools/retro_host build/typex2_libretro.dylib "<game folder>"   # the core, without RetroArch
```

Keys, pads and button layouts: [Controls](Controls.md). Environment switches
for logs, captures and experiments: [Debugging](Debugging.md#switches).

The desktop RetroArch core: copy it and
[`tools/libretro/typex2_libretro.info`](../tools/libretro/typex2_libretro.info)
into RetroArch's cores folder.

## Android

[Android and RetroArch](Android-and-RetroArch.md#building): the NDK and SDK
build-tools, no Gradle; one APK for every game, plus the RetroArch core.

## Tools (`tools/`)

- `arc`: unpack `gamediskimg.arc`.
- `kofarc`: list and extract KOF XIII's `media.kof`, decrypting TXAC entries
  with the game's `TypeXmulti/txac` key.
- `lifttest`: differential test of the lifter against Unicorn.
- `fxdump`: list the compiled D3DX effects in a file (e.g. a `game.exe`).
- `shadertest`: translate a folder of dumped shaders offline
  ([Debugging](Debugging.md#switches), `X2_SHADER_DUMP`).
- `retro_host`: a minimal libretro frontend for testing the core; `--next
  <game folder>` (repeatable) closes and reopens the core between games as
  RetroArch does, to test switching games; `--reset N,M` calls Restart at
  those frames; `--state s600,l900` saves and loads a state.
- `android/` (APK packaging, FFmpeg cross-build), `es-de/` (ES-DE system
  files), `libretro/` (the core's info file), `version.sh`, `lint.sh`,
  `wiki_sync.sh`.

The launcher: [Launcher → Developing](Launcher.md#developing). A release-style
app with its kit: `launcher/scripts/stage-kit.sh && cd launcher && npx tauri
build --config src-tauri/tauri.release.conf.json`.

## Contributing

Agents and humans: read [`CLAUDE.md`](../CLAUDE.md) (also `AGENTS.md`) for the
project rules. Style is `.clang-format` on new and changed lines only
(`tools/lint.sh format`, `tools/lint.sh fix`); bug-finding checks are
`.clang-tidy` (`tools/lint.sh tidy` after configuring `build/`); editors follow
`.editorconfig`. CI builds Linux, macOS and Windows for each release tag (or by hand: Actions → CI → Run workflow).

## Versions and releases

`tools/version.sh` keeps one version in every file that holds it (CI checks
they agree). A release: `tools/version.sh set X.Y.Z`, a dated section in
[`CHANGELOG.md`](../CHANGELOG.md), a commit, then `git tag vX.Y.Z` and push the
tag. [`.github/workflows/release.yml`](../.github/workflows/release.yml) checks
the tag against the version, builds the launcher for macOS, Linux and Windows
and publishes the release with the changelog section as its notes.
`vX.Y.Z-rc.N` tags make pre-releases. Then `tools/discord_release.sh` posts it to
the Discord server's #releases channel (the `DISCORD_RELEASES_WEBHOOK` secret):
the summary, What's new (a bullet per `**Label:**` paragraph or list) and Known
issues from the same section. `DRY_RUN=1 tools/discord_release.sh vX.Y.Z` shows
the post; `gh workflow run discord-release.yml -f tag=vX.Y.Z` posts one again.
