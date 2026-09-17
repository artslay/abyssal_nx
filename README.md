# ABYSSAL — Nintendo Switch Port

A Godot engine edition for **Deep 3D: Submarine Odyssey** by Fishlabs.

This project is an unofficial fan-made Nintendo Switch port. It loads the original Android game libraries and provides the required compatibility layer for running them on Nintendo Switch.

## How to install

Create the following folder on your SD card:

```text
/switch/abyssal_nx/
```

Place the following files inside:

```text
/switch/abyssal_nx/
├── abyssal_nx.nro
├── libgodot_android.so
├── libc++_shared.so
├── config.txt
├── assets/
│   ├── project.binary
│   └── ...
└── save/
    ├── content/
    └── native/
```

The Android libraries and game data must be obtained from the original Android version of the game.

## Obtaining the game files

The original Android APK can be obtained from the [ABYSSAL repository](https://github.com/TheWWWorm/abyssal).

Open the APK with an archive utility such as 7-Zip or WinRAR.

The required native libraries are located in:

```text
lib/arm64-v8a/
```

Copy:

```text
libgodot_android.so
libc++_shared.so
```

to:

```text
/switch/abyssal_nx/
```

The game data is contained in the APK `assets/` directory.

Copy the complete `assets/` folder to:

```text
/switch/abyssal_nx/assets/
```

The following file is required:

```text
/switch/abyssal_nx/assets/project.binary
```

The original game libraries and game data are not included in this repository.

## Save data

The `content` and `native` folders must be placed inside:

```text
/switch/abyssal_nx/save/
```

The resulting structure should be:

```text
/switch/abyssal_nx/save/
├── content/
└── native/
```

## Asset pack

The port supports an indexed asset pack for faster loading from the SD card.

On the first launch, the port automatically creates:

```text
/switch/abyssal_nx/assets.nxpack
/switch/abyssal_nx/assets.nxidx
```

The asset pack is generated from:

```text
/switch/abyssal_nx/assets/
```

These files do not need to be created manually.

If the asset pack is missing or outdated, it is rebuilt automatically. If packing fails, the port falls back to loading the loose assets directly.

## Notes

Do not launch the application from Album/applet mode if the available memory is insufficient.

Launching through a title override or forwarder is recommended.

## Controls

The Nintendo Switch controller is mapped to the Godot input system.

| Nintendo Switch | Godot         |
| --------------- | ------------- |
| A               | A             |
| B               | B             |
| X               | X             |
| Y               | Y             |
| L               | L1            |
| R               | R1            |
| L3              | L3            |
| R3              | R3            |
| ZL              | Left Trigger  |
| ZR              | Right Trigger |
| Left Stick      | Left Analog   |
| Right Stick     | Right Analog  |
| D-Pad           | D-Pad         |
| +               | Start         |
| -               | Back          |

## Configuration

The configuration file is:

```text
/switch/abyssal_nx/config.txt
```

The configuration can be used to adjust the screen resolution, analog deadzone, asset packing, Vulkan rendering and touch controls.

Example:

```text
screen_width 1280
screen_height 720
deadzone 18
assetpack 1
enable_vulkan 1
touch_controls 0
rendering_method mobile
```

## Resolution

The default resolution is:

```text
screen_width 1280
screen_height 720
```

The resolution can be changed in `config.txt`.

For example:

```text
screen_width 1920
screen_height 1080
```

Lower resolutions can be used to reduce rendering load.

## Build

### Requirements

Install the devkitPro Nintendo Switch toolchain, including:

* devkitA64
* libnx
* required Switch portlibs

### Mesa

This project does **not** include `mesa-sdk`.

Mesa must be built separately from this repository:

https://github.com/NaGaa95/mesa-switch

Build/install the required Mesa Switch libraries before building `abyssal_nx`.

The resulting Mesa libraries must be available to the `abyssal_nx` build according to the paths expected by the Makefile.

### Build abyssal_nx

Clone the repository and build it with:

```bash
make
```

For a clean rebuild:

```bash
make clean
make
```

The build produces:

```text
abyssal_nx.nro
abyssal_nx.nacp
abyssal_nx.elf
```

## Project structure

```text
abyssal_nx/
├── source/
├── Makefile
├── LICENSE
└── README.md
```

Mesa is built separately and is not included in this repository.

## Credits

* NaGaa95 — custom Mesa and Vulkan work.
* TheWWWorm — Android version / game port source
* Delson (delsonazevedo) — original Godot 4 Nintendo Switch wrapper this project was retargeted from.
* TheFloW (Andy Nguyen), fgsfds & Rinnegatamante — SoLoader lineage used by the wrapper.
* Godot Engine contributors — Godot Engine, licensed under MIT.
* Nintendo Switch homebrew community — tools, libraries and documentation used by the project.

## Legal

**Deep 3D: Submarine Odyssey** and its related assets, trademarks and copyrights belong to their respective copyright holders.

This project is an unofficial Nintendo Switch port and is not affiliated with or endorsed by Fishlabs or the original rights holders.

Game assets and Android libraries are not included in this repository.

Users must obtain the original game and required files themselves.

Unless otherwise specified, the source code of this project is distributed under the MIT License. See `LICENSE` for details.
