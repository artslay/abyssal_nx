# ABYSSAL — Nintendo Switch Port

A Godot engine edition for **Deep 3D: Submarine Odyssey** by Fishlabs.

This project is an unofficial fan-made Nintendo Switch port. It loads the original Android game libraries and provides the required compatibility layer for running them on Nintendo Switch.

## How to install

Copy the built NRO to:

```text
/switch/abyssal_nx/abyssal_nx.nro
```

User configuration, shader cache and runtime overrides are stored under:

```text
/switch/abyssal_nx/save/
```

`assets/` and the required Android libraries are bundled into the NRO during the build.

## JAR import

The Switch port can accept an original DEEP JAR directly from the Import Content file picker.

When a `.jar` is selected, the native importer:
- validates the MIDlet manifest;
- extracts and decodes the data resources;
- evaluates the restricted declarative class-data profile;
- creates a content pack compatible with the normal `.abyss` installer;
- caches the generated pack under:

```text
/switch/abyssal_nx/save/_jar_import/<sha256>.abyss
```

The JAR itself is never modified.

MIDI and AMR to WAV conversion is not yet included in the native Switch importer. Those resources are therefore not included in the generated content pack until the native audio conversion stage is implemented.

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
/switch/abyssal_nx/save/config.txt
```

The configuration can be used to adjust the screen resolution, analog deadzone, asset packing and Vulkan rendering.

Example:

```text
screen_width 1280
screen_height 720
deadzone 18
assetpack 1
enable_vulkan 1
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

## How to build

You need:

* devkitPro
* devkitA64
* libnx
* GNU Make
* Mesa Switch port

Mesa for Nintendo Switch:

https://github.com/NaGaa95/mesa-switch

Build the required Mesa Switch libraries according to the instructions in the repository.

Additional dependencies:

```bash
dkp-pacman -S switch-zlib switch-libexpat
```

The original Android APK is used as a build input. Place it next to the Makefile as `abyssal.apk`, or pass a different path with `APK=`.

Then build:

```bash
make
```

For a clean build:

```bash
make clean
make
```

The build embeds the APK `assets/` tree and the two arm64-v8a libraries into the NRO ROMFS.

The following APK directories are excluded:

```text
assets/dexopt/
assets/abyssal-importer/
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

