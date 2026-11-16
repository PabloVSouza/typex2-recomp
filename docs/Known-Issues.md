# Known issues

What doesn't work yet, or works with a catch, in the current version. Each
game's page has the details; fixed issues leave this list. Found something
that isn't here? Report it in
[GitHub issues](https://github.com/PabloVSouza/typex2-recomp/issues) or on
[Discord](https://discord.gg/bEQYMsEe9A).

## Everywhere

- **Linux and Windows:** playing games there is untested: development
  happens on a Mac. Reports are welcome, working or not.
- **Save states** load only on the screen they were saved on, and only until
  the game is closed; none during movies. No rewind or run-ahead
  ([details](Save-States.md)).
- **RetroArch, switching games:** a closed game leaves 1-3 MB behind until
  RetroArch quits.

## Games

- **BlazBlue: Calamity Trigger:** on slower Android devices the fight start
  pauses for 0.3-0.6 s while the game loads it, mostly the game's own work
  ([details](Game-BlazBlue-CT.md#known-issues)).
- **BlazBlue: Continuum Shift:** loading pauses of up to about 230 ms on
  slower Android devices, outside fights
  ([details](Game-BlazBlue-CS.md#known-issues)).
- **The King of Fighters XIII Climax:** on slower Android devices most stages
  are too heavy for 60 fps in fights (about 50 on the test tablet: the game
  thread needs 16.5-18 ms a frame); a few, like the green one, hold 60.
- **The King of Fighters: Maximum Impact Regulation A:** on Android, a fight
  starts after a ~1 s load, and each next team member after 0.3-0.5 s (the
  game's own loading). Not played by hand yet.
  Some test-menu sound
  effects are missing from the game's files
  ([details](Game-KOF-MI-Regulation-A.md#known-issues)).
- **Super Street Fighter IV Arcade Edition:** on slower Android devices the
  game thread is near its frame budget in fights (14-15.5 ms of 16.7 on the
  test tablet), so some stages lose frames: 15-28 late per 120 depending on
  the stage. No link play; the opening
  movie's English audio track isn't in the game's files
  ([details](Game-SSF4-AE.md#known-issues)).
