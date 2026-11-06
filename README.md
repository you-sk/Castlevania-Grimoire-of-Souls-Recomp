# GoSRecomp

> **This branch: experimental macOS 1.4.3 / build 105 source port.**
> Read [PORTING-1.4.3.md](PORTING-1.4.3.md) for the tested scope, Unity
> 2020.3.49f1 build steps and isolated Windows launcher. No 1.4.3 game binaries,
> assets, dumps, saves or receipts are distributed here. The upstream 1.4.2
> installation/release instructions below are historical and do not install
> this 1.4.3 port.
>
> 日本語: [1.4.3対応状況と起動条件](PORTING-1.4.3.ja.md)

Castlevania: Grimoire of Souls (Apple Arcade, macOS) running natively on Windows.
Same idea as [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).

> You need your own copy of the game: the macOS app, version 1.4.2, as a `.dmg`.
> This repo has no game code or data. The releases have no game data (art,
> sound, text): the setup takes it from your own image. Their `GameAssembly.dll`
> does contain the game's code, machine-translated to x64, and it doesn't run
> without the original files from your image. Please don't ask for game files
> or post links to them. Not affiliated with Konami, Apple or Unity.

## What's in it

- Fully offline. The game's server is emulated inside the game, so there's no
  account and no network
- Direct3D 11, with the post-processing and the SPFX particle effects
- Xbox and PlayStation controllers, keyboard and mouse. Mouse buttons can be
  bound in the game's own keyboard options
- Frame rate cap, V-Sync, fullscreen / borderless / windowed and resolution in
  `gosr.ini`
- Every language the game shipped with
- A debug menu on F8: gold, gems, every item by name, unlock everything, open
  every quest and banner, skip the tutorial, invincibility, damage multiplier.
  It asks before the first use, because it changes your save for good and skips
  the game's progression

## Installing

1. On the Releases page, download `GoSRecomp-x.y.z.zip` (not "Source code",
   that's just this repo) and extract it into a new, empty folder.
2. Drag your `.dmg` onto `Setup.bat`, or run it and paste the path. It takes a
   few minutes and needs about 3 GB free while it runs. If your image isn't
   version 1.4.2 it stops and tells you.
3. Start the game with `Play.bat`.

To update, extract the new release into a new folder and run `Setup.bat`
again. Your save stays where it is:
`%USERPROFILE%\AppData\LocalLow\Konami Digital Entertainment Co_, Ltd_\Castlevania_ Grimoire of Souls`

Needs Windows 10 or 11, 64-bit, and a Direct3D 11 GPU.

## Settings

Everything is in `gosr.ini` next to `GoS.exe`, and each line is explained in
there. Leave a value empty to keep the game's own behavior.

| Section | What |
|---|---|
| `[game]` | `language`: empty follows your Windows language |
| `[display]` | `fps` (0 = no cap), `vsync`, `mode` (fullscreen, borderless, windowed), `width`, `height` |
| `[controller]` | `layout=pc` (default), or `mac` for the tables the macOS build used. `dpad_move=0` stops the d-pad from moving your character. `xinput=0` if an Xbox controller misbehaves. `standard`: other controllers read by their standard buttons (1, default), for every controller (2) or off (0) |
| `[effects]` | `flip_fix=1`: experimental fix for effects drawn upside down in some screens (off by default) |
| `[mouse]` | `buttons=0` turns off mouse buttons in the keyboard options |
| `[debug]` | `menu=0` turns off the debug menu, `hotkey` changes F8 |

Bindings are set in the game's own Gamepad and Keyboard screens. Mouse buttons
show up there as Mouse L / R / M / 4 / 5. If you customized your controller with
an older build, press Default once.

## Status

It plays. The story, quests, summons, roster and upgrades work, and so do the
effects. Things I know about:

- An effect can show up a few frames late the first time you see it while its
  shaders get converted. After that they're cached.
- The game was made for 60 fps. Higher caps work, but if something feels too
  fast, go back to 60.

No multiplayer: I thought the game had multiplayer, but looks like I was completely wrong mb, which is a good thing in this case.

### Troubleshooting

- **Setup.bat says "not a GoSRecomp release"**: you downloaded "Source code". Download `GoSRecomp-x.y.z.zip` from the Releases page.
- **Setup.bat says the files are not the expected ones**: the `.dmg` must be the macOS app, version 1.4.2, unmodified.
- **Characters T-pose in the hub or roster**: update your graphics driver. An NVIDIA driver update fixed it for the players who had it.
- **Controller buttons or sticks are wrong**: PlayStation and Xbox controllers, press Default once in the game's controller options. Other controllers (8BitDo, Switch Pro, ...): use their X-input (Xbox) or Switch mode; if buttons are still wrong, set `standard=2` under `[controller]` in `gosr.ini`. An Xbox controller that misbehaves: `xinput=0`.
- **The d-pad moves your character** and you don't want it: `dpad_move=0`.

Found a bug? Start the game with `Play with crash log.bat`. When it closes it
makes a `crash_report.zip` with the logs, a copy of your save and settings, and
a dxdiag report. Open an issue and attach it. Have a look inside first if you
care about what's in there.

## Building

It builds on Linux and cross-compiles for Windows. You need the macOS game
files, Unity 2020.3.30f1 with Windows IL2CPP support and an MSVC sysroot from
xwin. [BUILDING.md](BUILDING.md) goes through every step. How it works is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## AI Note

AI was used to help with the coding. Still, the build was tested on different machines, and the code was reviewed. 
Sorry if there are still problems. I do it for the love of the game.

## Repository layout

```
tools/recomp/        arm64 to C++ translator and the generators around it
tools/analysis/      Function tables and surveys of the game binary and data
tools/shaders/       Metal to HLSL shader conversion
tools/patch/         Binary patches the setup applies to your own files
recomp/runtime/      What the translated code runs on (registers, memory, exceptions)
recomp/winbuild/     GameAssembly.dll: runtime glue, debug menu, display, controls
recomp/server/       The offline server
plugins/spfx/        The particle effects plugin, translated, OpenGL drawn on D3D11
plugins/apple/       Stand-ins for the Apple-only plugins
packaging/windows/   Setup.bat, Play.bat, gosr.ini and the readme that ship
docs/                How it works, shaders, server notes
```

## Credits

- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), for
  showing what static recompilation can do for a game
- [Il2CppDumper](https://github.com/Perfare/Il2CppDumper),
  [LIEF](https://github.com/lief-project/LIEF),
  [Capstone](https://github.com/capstone-engine/capstone),
  [Unicorn](https://github.com/unicorn-engine/unicorn) and
  [UnityPy](https://github.com/K0lb3/UnityPy), for taking the game apart
- [glslang](https://github.com/KhronosGroup/glslang) and
  [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), built into the
  effects plugin
- [7-Zip](https://www.7-zip.org/), used by the setup
- [xwin](https://github.com/Jake-Shadle/xwin), for building Windows binaries on Linux

Licenses are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Castlevania
is a trademark of Konami.
