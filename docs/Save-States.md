# Save states

Save the game at any moment of a fight and go back to that moment, for
practice or to retry a round. This works in the standalone app (F5 saves, F7
loads) and in RetroArch (its Save State and Load State).

## What to expect

- **Load where you saved.** A state loads on the screen it was saved on: the
  same fight, the same menu. Loaded somewhere else, it is refused, with a
  message that says so. Inside a fight, the round, the life bars and the
  timer all go back.
- **Until the game is closed.** States load only in the run of the game that
  saved them. A state from an earlier run is refused.
- **Not during movies.** A state can't be saved or loaded while a movie
  plays.
- **Large.** A state holds the game's whole memory: about 55 MB for
  KOF '98 UM, 200 MB for KOF XII, and 400-550 MB for KOF XIII, SSF4 and
  BlazBlue. Saving or loading takes a fraction of a second.
- **No rewind or run-ahead.** They need a state every frame, which this size
  doesn't allow, so the core refuses them.

## Why these limits

A recompiled game runs as native code. When a state is saved, each of the
game's threads is in the middle of a chain of function calls. That chain
lives on the computer's own stack, where it can't be saved. So a state can be
loaded only when every thread is in the same chain of calls as when it was
saved: the same screen. The game's graphics, sounds and other objects are kept
by the runtime outside the game's memory, and a state relies on the ones of
the run that saved it.

## How it works

For developers: `runtime/src/savestate.c` explains the method in its header
comment, and [Debugging](Debugging.md#switches) lists `X2_STATE_AT` and
`X2_STATE_PATH` for scripted tests.
