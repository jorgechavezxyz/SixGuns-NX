# Six Guns NX

A loader that runs the Android version of **Six Guns** (Gameloft, 2.9.9a,
`com.gameloft.android.ANMP.GloftSXHM`) natively on a Nintendo Switch running homebrew. It loads the
game's own arm64 engine and replaces the Android pieces around it: graphics, audio, input, saves,
files and networking. **No game code or data is included**: you build the SD card from your own copy
of the game with the tool in this repository.

## Features

- Full single-player campaign, 720p / 60 fps target, native audio, saves with automatic backups.
- Complete controller support: every menu, the shop, upgrades, pop-ups, map, journal; button hints;
  D-pad shortcuts (buy ammo when empty, quest teleport); hold A to skip cutscenes and comics.
- **LAN multiplayer** between Switch consoles (and the Ryubing emulator): host or join from a rebuilt
  lobby with a server browser and multiplayer name; capture the flag and team deathmatch.
- **Graphics menu** on the title screen (button next to the settings gear): Low / Medium / High
  presets and texture filtering, texture quality, draw distance, model detail, particles, screen
  effects, shadows, 30/60 fps. Medium is the original look.
- **Mod loader**: zips in `switch/sixguns/mods/` replace the game's files; **custom multiplayer maps**
  get their own page in the lobby. Included tech demo: **Stadium**, a football field with a running
  track, bleachers and a stadium wall, generated from your own game files.
- Online-only Gameloft services (events, clans, leaderboards, purchases, ads) no longer exist and are
  disabled; nothing is sent anywhere.

## What you need

- A Switch that runs homebrew (Atmosphère) and **title override** (hold R while starting a game).
  Applet / album mode does not have enough memory.
- Your own copy of **Six Guns for Android, version 2.9.9a**, as Gameloft published it:
  - the APK (signed by Gameloft; the builder checks the signature and refuses modified or repackaged
    copies), and
  - its two data files from the phone's `Android/obb/com.gameloft.android.ANMP.GloftSXHM/` folder:
    `main.29431.com.gameloft.android.ANMP.GloftSXHM.obb` and `patch.29523.com.gameloft.android.ANMP.GloftSXHM.obb`
    (they appear after the game has been started once on the phone).

## Install

### With the SD Builder app (macOS / Windows)

1. Download **Six Guns SD Builder** for your computer from the releases page and unzip it. It already
   contains the port (`sixguns.nro`); you only pick your game files.
   - macOS: the app is not notarised; the first time, right-click it and choose **Open**.
   - Windows: SmartScreen may warn about an unknown publisher; choose **More info > Run anyway**.
2. Choose your **Game APK** and the **OBB folder**, pick an output folder, keep **Stadium** (the demo
   custom map) and **LAN multiplayer** ticked if you want them, then **Build SD card**. **Check files**
   only verifies your files.
3. Put it on your Switch: see [Install on your Switch](#install-on-your-switch) below (the app shows the
   same steps when the build finishes).

### From the command line (any OS, Python 3.8+)

```
python3 tools/build_sd.py --apk SixGuns.apk --obb-dir com.gameloft.android.ANMP.GloftSXHM --nro sixguns.nro --stadium
```

`--check` only verifies the files, `--no-lan` keeps networking off. The Stadium map needs numpy
(`python3 -m pip install --user numpy`). `python3 tools/sd_builder_gui.py` opens the same window as the
app (on macOS use a python.org Python: the system one's Tk 8.5 cannot draw windows on current macOS; the
window offers to install numpy if it is missing).

### Install on your Switch

You need a Switch that runs homebrew (Atmosphère custom firmware).

1. Turn the Switch fully off (hold the power button > Power Options > Turn Off) and take out the
   microSD card. Put it in your computer.

2. Open the SD card. It already has a folder called "switch" (your homebrew lives there).
   Open the output folder of this builder: the builder's output folder (`switch/sixguns` inside it)
   Copy the "sixguns" folder INTO the SD card's "switch" folder, so you get:
       SD card/switch/sixguns/sixguns.nro, libAndroid.so, base.apk, assets/, obb/ ...
   Mac: do NOT drag a whole "switch" folder onto the card's "switch" folder; the Finder would offer
   to replace it and delete your other homebrew. Copy "sixguns" into it instead.
   (No "switch" folder on the card? Copy the builder's whole "switch" folder to the card's root.)
   About 850 MB; wait for the copy to finish.

3. Eject the SD card safely (Mac: Eject in the Finder sidebar; Windows: "Safely Remove Hardware"),
   put it back in the Switch and start Atmosphère as usual.

4. Start Six Guns in "title override" mode, which gives homebrew enough memory:
   on the HOME menu, hold R and start any game; the homebrew menu opens instead of the game.
   Release R and pick Six Guns. (The album / applet mode does not have enough memory.)

5. The first start takes a little longer. Your saves are in switch/sixguns/save/ with automatic
   backups in switch/sixguns/save_backups/. Quit with + and - together, or from the HOME menu.

Updating the port later: copy a newer sixguns.nro over switch/sixguns/sixguns.nro (saves stay).
Multiplayer: every console in a match needs the same version; custom maps need the same map mods.


## Controls

The engine has two controller schemes: the Android build's own gamepad support and an iOS-era
analog scheme. The port routes each press to the right one.

| Switch | Gameplay | Menus |
|---|---|---|
| Left stick | Move (analog) | Navigate (like the D-pad). On screens without built-in controller support (map, quest list, death screen, ...) the D-pad/stick move a gold frame between the screen's buttons and icons |
| Right stick | Camera (analog) | Shows the pointer |
| A | Action / interact; spur on the horse; hold 5 s to skip a cutscene (indicator bottom-right) | Confirm; press the framed button; tap at the pointer when it is shown (hold A + stick = drag) |
| B | Roll (somersault) | Back |
| L3 (press left stick) | Take / leave cover (when the wall icon shows) | |
| R3 (press right stick) | Turn around 180° | |
| ZR / ZL | Fire / Aim (hold ZL: keeps trying until the gun is ready) | Shop: next / previous tab |
| R | Tap: throw grenade/dynamite. Hold + D-pad Up/Down: next/previous grenade type, Left/Right: weapon | Shop: next tab |
| L | Hold: elixir key on screen; + D-pad Left/Up/Right: use elixir 1/2/3 (free, 5 min cooldown each) | Shop: previous tab |
| Y / X | Reload / Next weapon | |
| D-pad | Left: call horse / dismount · Down: buy ammo when the out-of-ammo offer shows · Right: fast travel to the selected quest when its button shows (small indicators mark both) | Navigate |
| + | Pause menu (its first button, formerly Customer Care, opens the shop) | Back (closes the pause menu) |
| Touch screen | Touch | Touch |
| − | Map (− again closes it) | Closes the map |
| + and − together | Quit | Quit |

A small list of the controls is shown at the bottom right during gameplay. The SHARE button on
achievement popups is now SCREENSHOT (saved to the Switch album). Camera speed can be tuned with a
`camera_speed.txt` in `switch/sixguns/` containing "<on foot> <on horse> [curve]" (default
"1.35 1.55 1.4").



## Multiplayer (LAN)

All players must be on the same network. MULTIPLAYER on the title screen opens the lobby: set your
name, mode and map; PLAY joins a match with those settings or hosts one; **Servers** lists matches on
the network (or add a host by address). Matches on a custom map need the same map mod on every console.

## Mods and custom maps

Put mod zips in `switch/sixguns/mods/`. Any file inside a zip replaces the game's file of the same
name; a zip with a `custom_map.txt` adds a multiplayer map. Formats, tools and the Stadium example:
[docs/MODDING.md](docs/MODDING.md). The tools only ever read your own game files; do not share zips
that contain the game's own data with people who do not own the game.

## Building from source

Install devkitPro with `switch-dev switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
switch-freetype switch-harfbuzz switch-libpng switch-bzip2`, then:

```
cd port && ./build.sh        # Windows: run it from devkitPro's MSYS2 shell
```

The result is `port/sixguns.nro`. `build.sh` builds in a temporary folder because devkitPro's
makefiles cannot handle spaces in paths.

## Status and known limits

- Tested on Switch hardware (single player, LAN multiplayer Switch + emulator) and in Ryubing 1.3.3.
- Only game version 2.9.9a (OBBs 29431 / 29523) is supported; the port patches the engine at known
  addresses.
- Online services are gone for good; the EVENTS screen is empty (a local events system is planned).
- Experimental: graphics options other than Medium have only been checked in the emulator.
- The SD Builder apps are unsigned (no Apple Developer ID / Windows code-signing certificate).

## Legal

This project contains no Gameloft code, assets or data, and does not circumvent any protection: it
runs the game files you supply. Six Guns is a trademark of Gameloft; this is an unofficial fan
project with no affiliation. Use it only with a copy of the game you own.

## How the port works

We got Six Guns running on Switch first by loading the original Android arm64 game engine and providing the platform functions it expected. Our loader connects graphics, audio, files, saves and input to Switch homebrew libraries, so the original game can run without Android.

We added controller support across gameplay and most game menus: selecting buttons, moving focus with the sticks and D-pad, confirming with A, and backing out with B. We adapted touch-oriented menus and rebuilt parts of the multiplayer interface, including the LAN lobby and server browser. The original game engine and assets are still supplied by the player; this repository contains our loader, patches and tools.

The desktop companion, [Six Guns Remaster-NX](https://github.com/jorgechavezxyz/sixguns-pc), runs this same Switch port through a dedicated Ryubing-based launcher. Its setup guides players through their own Switch keys, firmware and verified game files. It installs the game with this repository's SD Builder (`tools/packaging/sd_builder_cli.spec`, the command-line build of `tools/build_sd.py`), so a PC or Mac gets the same checks, the same `sixguns.nro` and the same Stadium map as a Switch, and LAN matches between them use identical builds.

## License

Original project code and tools are available under the MIT license in LICENSE. Existing third-party notices remain applicable. The license does not cover Gameloft's proprietary engine, game assets, Switch firmware or keys; those are not distributed here.

