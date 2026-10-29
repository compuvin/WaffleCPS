# WaffleCPS

WaffleCPS is an early desktop editor for DMR codeplugs. It organizes programming around repeaters, time slots, and talkgroups so you can see how you will use a radio without working through a traditional channel table.

The application is under active development. It reads external `.wcp` definitions for codeplug file import and export. Definitions are included for CS7000-M17 Plus, CS750, CS800, and OpenGD77. Waffle can read and write a supported CS7000-M17 Plus over USB. Other imported definitions have no radio transfer controls unless a matching `.wrd` file is present.

## Screenshots

These views use an imported CS7000 GPS codeplug.

**Repeater overview** — CT-Brooklyn Ares, with separate time-slot cards and talkgroup details.

![WaffleCPS repeater overview for CT-Brooklyn Ares](docs/screenshots/repeater.png)

**Add talkgroup** — choose an existing group contact and give the radio channel its own name.

![Add talkgroup dialog with NY NJ Metro selected and Brooklyn Metro entered as the channel name](docs/screenshots/add-talkgroup.png)

**Contacts** — people and talkgroups have separate searchable tabs.

![WaffleCPS contacts view showing people from the imported codeplug](docs/screenshots/contacts.png)

## What works today

- Import codeplug files using a selected `.wcp` definition, or detect a definition from file contents. The included definitions cover CS7000 (`.rdbx`), CS750 and CS800 (`.rdb`), and OpenGD77 (`.g77`). Compatibility depends on the model and file layout declared by each definition.
- Browse imported repeaters, zones, channels, and contacts. A zone with one repeater frequency pair and color code appears as a repeater, even if it also contains simplex frequencies. Separate zones remain separate entries.
- View each repeater's frequencies, color code, transmit power, time slots, and additional simplex frequencies. The channel name appears above its talkgroup name and ID, matching the radio's idle versus receive/transmit display behavior.
- Search people and talkgroups in separate contact tabs. Add a repeater talkgroup from the group contact list or create one in the same flow, with a separate name for its radio channel.
- Add or remove talkgroups and additional simplex frequencies. When an imported channel is shared with another zone, removing it from the selected zone leaves the shared record available to the other zone.
- Create a blank project and save or open `.waffle` projects. Imported projects retain the original codeplug bytes as an untouched attachment.
- Set a radio name or call sign and DMR radio ID on the Radio Settings page. Imported values are shown automatically and saved with the project.
- Export an imported codeplug using its selected definition and preserved source bytes. New currently uses the CS7000 default from its external definition; selecting another radio for New is still pending.
- Read and write the supported CS7000-M17 Plus over USB. Backups are manual; see [Radio operations](docs/RadioOperations.md).

The editor does not yet expose every radio setting, receive group, or scan list. File export remains experimental; keep your original codeplug and check exports in the vendor's CPS. Direct radio writing has been tested only for the supported CS7000-M17 Plus workflow. The [project checklist](docs/ProjectChecklist.md) tracks the remaining work.

## Build and run

WaffleCPS uses C++17, CMake 3.21 or newer, Qt 6.4 or newer, zlib, and libusb. On Ubuntu 24.04, install the build tools and Qt development/QML packages:

```sh
sudo apt install cmake g++ pkg-config libusb-1.0-0-dev zlib1g-dev qt6-base-dev qt6-declarative-dev \
  qml6-module-qtqml qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-templates \
  qml6-module-qtquick-window
```

Then build and launch the application:

```sh
cmake -S . -B build
cmake --build build
./build/wafflecps
```

Keep the `definitions/` directory available beside the project or executable. Use **Import codeplug** for a file format declared by a loaded definition, or **Open** for a `.waffle` project. You can also pass either file type when launching the app:

```sh
./build/wafflecps path/to/project.waffle
./build/wafflecps path/to/codeplug.rdbx
```

For supported CS7000-M17 Plus radios, **Read from radio** downloads the complete codeplug over USB and preserves it as the project's original source. See [radio reading](docs/RadioReading.md) for Linux USB permissions, cancellation, and standalone diagnostics.

Run the automated checks with:

```sh
ctest --test-dir build --output-on-failure
```

WaffleCPS uses Qt and CMake, but Windows and macOS builds have not yet been verified or packaged. The app icon is generated from `tools/render_icon.py`; Pillow is needed only to regenerate the image, not to build WaffleCPS.

## Project documents

- [Project checklist](docs/ProjectChecklist.md) — agreed scope and progress.
- [Project plan](ProjectPlan.md) — original product direction.
- [Project format](docs/ProjectFormat.md) — the open `.waffle` archive format.
- [Import and data model](docs/ImportAndDataModel.md) — classification and editor design.
- [Codeplug research](docs/CodeplugResearch.md) — findings from the tested sample.

## License and support

WaffleCPS is licensed under the [GNU General Public License version 3](LICENSE).

If you would like to support development, you can [buy the author a coffee](https://buymeacoffee.com/compuvin).

[![CodeFactor](https://www.codefactor.io/repository/github/compuvin/wafflecps/badge)](https://www.codefactor.io/repository/github/compuvin/wafflecps)
