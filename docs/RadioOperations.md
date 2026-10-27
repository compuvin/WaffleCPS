# Radio operations and backups

Waffle supports reading and writing the tested CS7000-M17 Plus codeplug layout over USB. The developer confirmed successful hardware writes of both a default codeplug and an existing populated codeplug. This does not establish compatibility with every model or firmware variant.

## Backing up the current radio programming

Backups are manual. Waffle does not automatically read or save the radio before writing.

1. Connect and power on the radio, then use **Read from radio**.
2. After the read completes, save a separate `.waffle` project before editing. The project retains the complete original codeplug, including unsupported settings.
3. Keep that project as the backup and use **Save As** for an edited copy. A project made from an older imported file is not necessarily a backup of the radio's current programming.

See [radio reading](RadioReading.md) for read cancellation, Linux USB permissions, and standalone diagnostics.

## Writing and validation

**Write to radio** asks for confirmation. Waffle validates the exported codeplug and write plan before entering programming mode, checks the connected radio's identification, and checks the radio's acknowledgements during transfer. Writing runs in a background thread with progress reporting; failures are reported to the user.

As requested, the write workflow has no automatic backup and no post-write download to compare the written bytes. Successful completion means the protocol exchanges completed with the expected acknowledgements, rather than an independent read-back verification. Writes are not cancellable through the application.

The write sequence was compared offline with a vendor CPS USB capture using `tools/verify_cs7000_write.py`, then tested on hardware. Source pointers: `src/radiowriter.cpp` for background execution and initial validation, `src/cs7000usb.cpp` for identification, write planning, and acknowledgements, and `qml/Main.qml` for the write confirmation.

## Recovery limits

A failed or cancelled read leaves the current project intact. An interrupted write can leave the radio partially programmed; Waffle does not automatically roll it back. Retain a known-good backup for a subsequent programming attempt using Waffle or the vendor CPS once communication is available. A `.waffle` backup must be exported to a supported codeplug file before opening it in the vendor CPS.

This backup covers codeplug programming data, not a firmware image or a guaranteed recovery method for an unresponsive radio.
