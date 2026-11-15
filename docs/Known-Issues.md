# Known issues

What doesn't work yet, or works with a catch, in the current version. Each
game's page has the details; fixed issues leave this list. Found something
that isn't here? Report it in
[GitHub issues](https://github.com/PabloVSouza/typex2-recomp/issues) or on
[Discord](https://discord.gg/bEQYMsEe9A).

## Everywhere

- **Linux and Windows:** playing games there is untested: development
  happens on a Mac. Reports are welcome, working or not.
- **RetroArch:** no save states, rewind or run-ahead; Reset works with the
  Vulkan renderer (the default), not with OpenGL.
- **RetroArch, switching games:** a closed game leaves 5-20 MB behind until
  RetroArch quits.

## Games

- **BlazBlue: Calamity Trigger:** on slower Android devices the fight start
  pauses for 0.3-0.6 s while the game loads it, mostly the game's own work
  ([details](Game-BlazBlue-CT.md#known-issues)).
- **BlazBlue: Continuum Shift:** loading pauses of up to about 230 ms on
  slower Android devices, outside fights
  ([details](Game-BlazBlue-CS.md#known-issues)).
- **The King of Fighters XIII:** starts in Japanese; switch the language in
  the test menu
  ([details](Game-KOF-XIII.md#known-issues)).
- **The King of Fighters: Maximum Impact Regulation A:** on Android, a fight
  loads in one ~1.2 s frame at its start (the game's own loading);
  not played by hand yet. Some test-menu sound
  effects are missing from the game's files
  ([details](Game-KOF-MI-Regulation-A.md#known-issues)).
- **Super Street Fighter IV Arcade Edition:** with OpenGL
  (`X2_RENDERER=gl`) a few stages run at 50-55 fps on slower Android devices
  (Vulkan, the default, holds 60). No link play; the opening movie's English
  audio track isn't in the game's files
  ([details](Game-SSF4-AE.md#known-issues)).
