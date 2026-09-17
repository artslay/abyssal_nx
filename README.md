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

The required native libraries are located in the Android APK:

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

These folders contain the game content and native data required by the port.

## Asset pack

The port supports an indexed asset pack for faster loading from the SD card.

Asset packing is enabled by default.

On the first launch, the port automatically creates:

```text
/switch/abyssal_nx/assets.nxpack
/switch/abyssal_nx/assets.nxidx
```

The asset pack is generated from the loose files in:

```text
/switch/abyssal_nx/assets/
```

It does not need to be created manually.

If the asset pack is missing or the contents of `assets/` have changed, it is rebuilt automatically. If packing fails, the port falls back to loading the loose assets directly.

## Notes

Do not launch the application from Album/applet mode if the available memory is insufficient.

The engine reserves a large amount of memory for loading the Android libraries, so launching through a title override or a forwarder is recommended.

## Controls

The Nintendo Switch controller is mapped to the Godot input system:

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

Analog stick deadzone can be configured with:

```text
deadzone 18
```

Touch controls are disabled by default:

```text
touch_controls 0
```

## Configuration

The configuration file is:

```text
/switch/abyssal_nx/config.txt
```

If the file does not exist, the port creates it automatically with the default configuration.

Default configuration:

```text
screen_width 1280
screen_height 720
deadzone 18
assetpack 1
enable_vulkan 1
touch_controls 0
rendering_method mobile
```

### Configuration options

| Option             | Description                       |
| ------------------ | --------------------------------- |
| `screen_width`     | Render width                      |
| `screen_height`    | Render height                     |
| `deadzone`         | Analog-stick dead zone in percent |
| `assetpack`        | Enables the indexed asset pack    |
| `enable_vulkan`    | Enables Vulkan/NVK rendering      |
| `touch_controls`   | Enables Android touch controls    |
| `rendering_method` | Godot rendering method            |

The available configuration variables are defined directly in the port source.

## Resolution

The default resolution is:

```text
screen_width 1280
screen_height 720
```

The resolution can be changed directly in `config.txt`.

For example:

```text
screen_width 1920
screen_height 1080
```

The port accepts resolutions up to 1920×1080. Smaller resolutions such as `960x540` or `640x360` can be used to reduce rendering load.

## Build

The project requires:

* devkitPro
* devkitA64
* libnx
* GNU Make

The project includes its Mesa Switch SDK in:

```text
mesa-sdk/
```

The Makefile uses the included Mesa libraries for Vulkan/EGL/GLES rendering.

Required portlibs:

```bash
dkp-pacman -S switch-zlib switch-libexpat
```

Build from a devkitPro shell:

```bash
make
```

Clean the build:

```bash
make clean
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
├── mesa-sdk/
├── Makefile
├── LICENSE
└── README.md
```

## Credits

* **NaGaa95** — custom Mesa and Vulkan work.
* **Delson (delsonazevedo)** — original Godot 4 Nintendo Switch wrapper this project was retargeted from.
* **TheFloW (Andy Nguyen), fgsfds & Rinnegatamante** — SoLoader lineage used by the wrapper.
* **Godot Engine contributors** — Godot Engine, licensed under MIT.
* **Nintendo Switch homebrew community** — tools, libraries and documentation used by the project.

## Legal

**Deep 3D: Submarine Odyssey** and its related assets, trademarks and copyrights belong to their respective copyright holders.

This project is an unofficial Nintendo Switch port and is not affiliated with or endorsed by Fishlabs or the original rights holders.

Game assets and Android libraries are not included in this repository unless explicitly provided by their respective copyright holders.
