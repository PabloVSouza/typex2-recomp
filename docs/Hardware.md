# Hardware

Specifications are from Wikipedia's
[Taito Type X](https://en.wikipedia.org/wiki/Taito_Type_X) article.

## Type X / Type X+ (2004–)

| | |
|---|---|
| OS | Windows XP Embedded |
| CPU | Intel Celeron 2.5 GHz (up to a Pentium 4 3.0 GHz) |
| Chipset | Intel 865G |
| RAM | 256 MB DDR (up to 2 GB) |
| GPU | AGP 8x: ATI Radeon 9600 SE / 9600 XT / X700 PRO |
| Storage | PATA / SATA hard disk |

Type X+ is the same board with a stronger graphics card.

## Type X2 (2007–2015)

| | |
|---|---|
| OS | Windows XP Embedded |
| CPU | LGA 775: Celeron D 352, Pentium 4 651, Core 2 Duo E6400 |
| Chipset | Intel Q965 + ICH8 |
| RAM | DDR2 667/800, 512 MB – 4 GB |
| GPU | PCIe x16: Radeon X1600 Pro / X1300 LE, GeForce 7900 GS / 7600 GS / 7300 GS |
| Video out | 640×480, 1280×720, 1920×1080 |
| I/O | 1 JVS, 4 USB 2.0, serial, parallel, PS/2, SATA |

Wikipedia says software doesn't move between Type X and Type X2, because of
Windows XP Embedded driver differences. That is irrelevant here, since the
games no longer run on Windows.

## What the runtime emulates

| Cabinet part | In typex2-recomp |
|---|---|
| x86 CPU | Code recompiled to C; one host thread per guest thread, running in parallel. On ARM, shared stores are ordered to keep x86 memory semantics (`X2_TSO`). |
| Windows XP | Host implementations of the imported functions: kernel32, user32, gdi32, advapi32 crypto, ole32, winmm, Winsock stubs (`runtime/src/hle/`). |
| GPU, Direct3D 9 | D3D9 / D3DX objects as host COM objects. Frames are recorded on the game thread and replayed with OpenGL 3.3 / GLES 3.2. Shaders are translated to GLSL from bytecode, assembly, HLSL source or compiled effects. |
| Sound | DirectSound mixed by the runtime, output through SDL3 or libretro. |
| Movies | DirectShow graphs decoded with FFmpeg (WMV / WMA). |
| JVS I/O board | Emulated on COM2 (`runtime/src/jvs.c`): addressing, switches, coins, analog, lamps. It also answers the controller handshake BlazBlue CT sends before JVS. |
| Test / service buttons, coin chute | Keyboard, pad (Select = coin, hold it for test) or touch overlay. |
| Hard disk | The game folder. Saves go where the game writes them (`sv/`, `Ranking/`, ...). |
