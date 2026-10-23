# Reading a CS7000 over USB

Use **Read from radio** with the radio powered on and connected through its programming socket. Normal operating mode is sufficient. The first read implementation supports the tested CS7000-M17 Plus / CS7000 GPS layout; it identifies the radio before reading settings.

Waffle reads both complete codeplug regions in a background thread. It shows progress for the radio-settings region, then for the contact region, which takes most of the time. Cancel finishes the current request/response exchange before stopping; it does not send a command to exit the radio's Program Mode. A failed or cancelled read leaves your current project intact. A successful read replaces the current project; save it as a `.waffle` project to retain the entire original codeplug, including settings the editor does not expose.

The raw radio image is encoded into `.rdbx` without changing its decoded bytes. The original is the base for later exports. Preserving that source does not establish that every possible edit preserves all unsupported fields; exports remain experimental and should be checked in the vendor CPS.

## Linux USB permission

Install the device-specific rule once, then reconnect the radio:

```sh
sudo install -m 0644 packaging/70-wafflecps.rules /etc/udev/rules.d/70-wafflecps.rules
sudo udevadm control --reload-rules
```

Unplug and reconnect the USB cable afterward. Run Waffle as your normal desktop user. The rule grants the active local desktop session access to USB device `0483:5720`; it does not grant access to other USB IDs. The reader temporarily claims interface 0 and restores its kernel driver when finished. Do not run vendor CPS or another programming utility against the same radio at the same time.

## Standalone diagnostics

The diagnostic tool shares the application's transport and read sequence:

```sh
cmake --build build --target cs7000-read
./build/cs7000-read --capture pcaps/ReadFromRadio.pcapng
./build/cs7000-read --direct --read --output /tmp/cs7000-new-read
```

Without `--read`, capture verification does not open USB. A live diagnostic read writes decoded blocks, a transaction log, and a coverage manifest into a new directory. The output contains private radio programming data. A manifest without a `complete` record indicates a partial read. The diagnostic tool refuses to overwrite an existing output directory.

The full-read protocol was tested on hardware, and the reconstructed `.rdbx` was accepted by both Waffle and the vendor CPS. The integrated GUI still requires hardware testing after changes. Waffle does not implement writing to the radio.
