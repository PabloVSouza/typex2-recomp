# Proposal: building games on the Android device

**Status: open, to be decided.** This is a possible direction, not planned
work: we need to decide whether it is a path we want to follow. Opinions are
welcome on [Discord](https://discord.gg/bEQYMsEe9A) or in
[GitHub issues](https://github.com/PabloVSouza/typex2-recomp/issues).

## The idea

Today a game reaches Android through a computer: the [launcher](Launcher.md)
recompiles it there and exports the RetroArch core or the app, which is then
copied to the device. A self-contained Android app would do everything on the
device: find the game folders, recompile each game, compile the result and
play it, with no computer involved.

## What it takes

- **The recompiler on Android.** `x2recomp` (C++20 and Capstone) and the
  runtime libraries (runtime, glslang, FFmpeg) cross-compile for Android
  without trouble. The runtime would ship prebuilt in the app, so the device
  only compiles each game's generated C and links it.
- **A C compiler on the device.** The app carries clang and lld built for
  Android, roughly 150-250 MB (part of the app, or downloaded on first use).
  Android doesn't let an app run programs from its writable storage, so they
  ship as the app's native libraries (`lib*.so` in the APK, run from the
  native library folder), as Termux does.
- **Loading what was built.** The app loads the game library it compiled from
  its own storage, which Android allows.
- **The interface.** The launcher's front end could probably carry over:
  Tauri 2 runs on Android. Reading the game folders needs broad storage
  access, as the current app already has.
- **RetroArch.** A core built on the device still has to be installed by hand
  (Load Core → Install or Restore a Core): RetroArch's folders are private to
  it. The standalone app, which loads what it builds directly, is the natural
  target.

## The risk: build time on the device

Each game's generated C is 24-75 MB (KOF '98 UM and BlazBlue CT the
smallest, KOF MI and SSF4 the largest). An Apple M3 Pro compiles a game in a
minute or two. The test tablet (Helio G99: 2 fast cores, 6 slow, 8 GB)
should be 15-30 times slower: an estimated **30-60 minutes or more per
game**. The tablet would heat up and slow down, and memory limits how many
compilers can run at once. A lighter optimisation level builds faster, but
the game runs slower; that matters on a device that only just holds 60 fps.

## Cost

A few weeks of work, most of it in packaging the compiler and in the
build flow on the device, with build time as the open risk.

## Before deciding

A one-day measurement: build clang for the tablet, compile one game's
generated C there, and time it (and its peak memory) at the optimisation
levels we use. Under about 20-30 minutes per game, the rest is worth doing;
hours, and the idea is dropped.
