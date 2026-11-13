# Porting a game

Here is what it took to port the six supported games, step by step. Keep the
[project rules](../CLAUDE.md) in mind: C / C++ only, nothing copyrighted in
git, and never write into the game's folder.

## 1. Recompile

```sh
cmake --build build --target x2recomp
build/recompiler/x2recomp "<game dir>/game.exe" games/<id>/gen
```

- **Check `report.txt`.** It lists unsupported instructions. `outsb` /
  `insd` / `aas` come from data misread as code and are harmless.
- **Don't pipe `x2recomp` into `head`.** SIGPIPE stops it halfway, and
  `funcs.h` and `table.c` then disagree.

## 2. Build

1. Add the game's label in the `if(game STREQUAL ...)` chain of
   `runtime/CMakeLists.txt`.
2. Configure and build:

```sh
cmake -S . -B build && cmake --build build --target x2-<id>
```

## 3. Run headless and fill in imports

```sh
cd "<game dir>"
SDL_AUDIO_DRIVER=dummy X2_NOPRESENT=1 X2_SHOT=300,600 X2_SHOT_PATH=$TMPDIR/s.png \
    /path/to/build/x2-<id> . -vv
```

- **Missing imports.** `-vv` lists them ("unimplemented import DLL!Name").
  The first call to one stops the game and names the caller. Implement it
  in the matching `runtime/src/hle/*.c` table.
- **New DLL versions.** A new version of a DLL (`d3dx9_31.dll`, ...) is an
  alias in `hle_modules[]` (`loader.c`) and in `fake_modules[]`
  (`hle/k32_misc.c`).
- **Follow the game's code.** Before guessing at a protocol, read what the
  game does. The disassembly of `game.exe` and the generated C side by side
  answer most questions.

## 4. Inputs

- **Find the button layout.** Open the game's INPUT TEST in its test menu.
  `X2_INPUT=900:t,910:-,...` scripts the test switch and the buttons.
- **If the I/O board never comes up**, look at how the game opens it. BlazBlue
  CT talks to an I/O-board controller before JVS, and its I/O thread
  suspends itself every frame.
- **Add a default pad layout** in `padmap.c` if the panel isn't KOF-like.
  Document the `X2_PAD` line for 8-button sticks.

## 5. Android

1. Add the game to `X2_GAMES` and build the core:
   `cmake --build build-android --target typex2_libretro`. A plain
   `cmake --build build-android` does **not** rebuild the core.
2. Copy the game to `ROMs/arcade/<Game>.x2/`.
3. Measure with `X2_PACELOG` and `X2_PROF`. See
   [Android and RetroArch](Android-and-RetroArch.md).

## 6. Regression check and docs

- **Recompiler changes.** After any change to the recompiler, regenerate
  every game's `gen/`.
- **Check the other games.** At least their attract mode. Use pixel-identical
  screenshots of a deterministic screen (title, character select) when a
  change should not alter output.
- **Update the docs:**
  - the game's page and [Game status](Game-Status.md);
  - the README (status, switches, known gaps);
  - `CLAUDE.md` (game table).
- **Commit and push.**
