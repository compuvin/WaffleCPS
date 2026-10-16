# WaffleCPS

WaffleCPS is an early Qt 6 desktop prototype for a repeater-centered DMR codeplug editor. You can create a blank project, add repeaters and simplex frequencies, save or open a WaffleCPS project, and import a CS7000-M17 Plus codeplug made by CPS V1.02.20.00. The import shows channels and zones. Zones with one digital repeater frequency pair and color code appear under that repeater even when they also contain simplex channels; other zones stay in the zone list. You can add a simplex channel to an imported repeater zone. The original codeplug bytes are preserved in the project; the app does not export a codeplug or write a radio.

The evolving product direction is in [ProjectPlan.md](ProjectPlan.md). The open project container is documented in [docs/ProjectFormat.md](docs/ProjectFormat.md). Import rules and early sample-file findings are in [docs/](docs/).

## Build on Ubuntu 24.04

Install CMake, a C++ compiler, zlib development headers, and the Qt 6 development and QML runtime packages:

```sh
sudo apt install cmake g++ zlib1g-dev qt6-base-dev qt6-declarative-dev \
  qml6-module-qtqml qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-templates \
  qml6-module-qtquick-window
```

Then build and run:

```sh
cmake -S . -B build
cmake --build build
./build/wafflecps
```

You can also open a `.waffle` project or import a supported `.rdbx` file by passing its path to `wafflecps`.

Run the project and importer checks with `ctest --test-dir build --output-on-failure`.

Qt 6.4 or newer is required. Windows builds use the same CMake project with a Qt 6 installation and compatible compiler.

The app icon is stored in `assets/waffle-radio.png`. To regenerate it after editing its drawing script, install Pillow and run `python3 tools/render_icon.py`; Pillow is not needed to build or run WaffleCPS.
