# Radio operations and backups

Use the **Radio** menu to read a radio's programming or write the current project to a radio. Available operations depend on the selected radio's transfer definition. See [Radio support](../README.md#radio-support) for included definitions and hardware-tested models.

## Reading from a radio

1. Connect and power on the radio. Close any other programming software using it.
2. Choose **Radio → Read**. If no radio project is loaded, select your radio from the list. Otherwise, WaffleCPS uses the current project's radio definition.
3. Wait for the transfer to finish. A successful read replaces the current project with the downloaded programming. Save any existing work first.

Read is disabled when the current radio has no transfer definition. Write is disabled until a radio-specific project with a transfer definition is loaded or created.

Reads show progress and can be cancelled. A failed or cancelled read leaves the current project intact. Cancellation may wait for an outstanding device request; whether the radio exits programming mode depends on its protocol and how the transfer ended.

## Backing up the current radio programming

Backups are manual. WaffleCPS does not automatically read or save the radio before writing.

1. Read the radio's current programming with **Radio → Read**.
2. Save a separate `.waffle` project before editing. It retains the original codeplug produced by the read, including data the editor does not expose.
3. Keep that project as the backup and use **Project → Save As** for an edited copy. A project made from an older imported file may not represent the radio's current programming.

A `.waffle` project must be exported through **Codeplug → Export** before its programming can be opened in the vendor CPS.

## Writing and validation

Choose **Radio → Write** and confirm the operation. WaffleCPS exports and validates the project's codeplug, checks compatibility with its transfer definition, and performs the identification checks declared for that radio. These checks do not guarantee compatibility with every firmware version.

Writing runs with progress reporting, and failures are displayed. Keep the radio connected and powered on until the operation finishes. Writes cannot be cancelled through the application.

Verification depends on the transfer protocol:

| Transfer definition | Write verification |
| --- | --- |
| CS7000-M17 Plus | Protocol responses and acknowledgements; no automatic read-back comparison. |
| CS700 / CS701 and MD-380 | DFU protocol status and completion checks; no automatic read-back comparison. |
| OpenGD77 | Read back and compare each changed flash sector after writing. |
| DM-32UV | Read back and compare changed logical pages after writing. |

Read-back checks cover the data written by the operation. They do not replace a backup or prove that every radio feature behaves correctly.

## Hardware test results

User-reported tests after moving USB transfer definitions into `.wcp` packages:

| Radio | Read | Write |
| --- | --- | --- |
| CS7000 | Successful | Previously tested; not repeated in this round |
| CS750 | Successful | Successful |
| CS800 | Successful | Successful |

## Device access

Windows and macOS may request administrator authorization when the storage-style USB transport requires it. WaffleCPS tries normal access first and uses its helper when that access is denied. Serial and DFU connections use their normal device-access paths.

For CS7000 Linux USB permissions and read behavior, see [Radio reading](RadioReading.md). Serial connections also require an installed cable driver and permission to open the serial port.

## Recovery limits

An interrupted write can leave the radio partially programmed. WaffleCPS does not automatically roll it back. Retain a known-good backup for another programming attempt using WaffleCPS or the vendor CPS once communication is available.

The backup covers codeplug programming data. It is not a firmware backup or a guaranteed recovery method for an unresponsive radio.
