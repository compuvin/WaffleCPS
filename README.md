# WaffleCPS

WaffleCPS is an early Qt 6 desktop prototype for a repeater-centered DMR codeplug editor. The current build starts with two sample repeaters. You can edit repeater details, add or remove talkgroups, and save or open a WaffleCPS project. You can attach an original codeplug to preserve its bytes in the project; the app does not yet import its radio settings, export a codeplug, or write a radio.

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

Run the project archive round-trip check with `ctest --test-dir build --output-on-failure`.

Qt 6.4 or newer is required. Windows builds use the same CMake project with a Qt 6 installation and compatible compiler.

The app icon is stored in `assets/waffle-radio.png`. To regenerate it after editing its drawing script, install Pillow and run `python3 tools/render_icon.py`; Pillow is not needed to build or run WaffleCPS.
