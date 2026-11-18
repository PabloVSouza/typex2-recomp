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
- **The King of Fighters: Maximum Impact Regulation A:** on Android, a fight
  starts after a ~1 s load, and each next team member after 0.3-0.5 s (the
  game's own loading). Not played by hand yet.
  Some test-menu sound
  effects are missing from the game's files
  ([details](Game-KOF-MI-Regulation-A.md#known-issues)).
- **Samurai Shodown: Edge of Destiny** (in progress): Android runs it in
  its SD mode (640×480) to hold 60 fps; in HD (`X2_HD=1`) fights dip
  during the large animations unless `X2_SCENE_SCALE=0.75` lowers the 3D
  resolution. Movies blinked on Android
  before a renderer fix; not seen since
  ([details](Game-Samurai-Shodown-EoD.md#known-issues)).
- **Super Street Fighter IV Arcade Edition:** on slower Android devices some
  stages still lose frames in fights: 1-7 late per 120 on the test tablet.
  These come from occasional 20-30 ms frames; the game thread averages
  12-13.5 ms, and the GPU 10-12.5 ms. No link play; the opening
  movie's English audio track isn't in the game's files
  ([details](Game-SSF4-AE.md#known-issues)).
