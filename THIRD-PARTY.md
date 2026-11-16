# Third-party software

typex2-recomp is licensed under the GNU General Public License, version 3 or
(at your option) any later version: see [LICENSE](LICENSE). Its builds include
or link the following projects, under their own licenses.

| Project | Used for | License |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) (release-3.4.20) | windows, input, audio; the Android game activity | zlib |
| [FFmpeg](https://ffmpeg.org) | game movies (WMV) | LGPL-2.1-or-later as built for Android (`tools/android/build_ffmpeg.sh`, no GPL parts); desktop builds use the platform's package, whose configuration may make it GPL |
| [Capstone](https://github.com/capstone-engine/capstone) (5.0.1) | x86 disassembly in the recompiler | BSD-3-Clause |
| [glslang](https://github.com/KhronosGroup/glslang) (16.6.0) | shader compilation for Vulkan | BSD-3-Clause, with parts under Apache-2.0 and other permissive licenses (see its `LICENSE.txt`) |
| [stb_truetype](https://github.com/nothings/stb) (`runtime/src/third_party`) | font rendering on Android and Linux | public domain or MIT |
| [libretro API headers](https://github.com/libretro/RetroArch) (`runtime/src/libretro`) | the RetroArch core | MIT |
| [Tauri](https://tauri.app) and the launcher's Rust crates and npm packages | the launcher and the Android app | mostly MIT and/or Apache-2.0 (see `launcher/src-tauri/Cargo.lock` and `launcher/package-lock.json`) |

No game files and no recompiled game code are part of this project or its
releases: games are recompiled on your own computer from your own files.
