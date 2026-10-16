# WaffleCPS Project Format

A `.waffle` file is a standard ZIP archive. It is an open working-project format, separate from any radio manufacturer's codeplug format. Version 1 stores the repeater editor's data and can keep one original codeplug as an untouched attachment. Version 2 adds read only imported channel and zone records. Both versions remain readable.

## Archive entries

| Entry | Required | Contents |
|---|---|---|
| `project.json` | Yes | UTF-8 JSON document described below |
| `source/original-codeplug.bin` | When `sourceCodeplug` is present | Original source file bytes, unchanged |

WaffleCPS writes entries with ordinary ZIP32 deflate compression and also reads uncompressed ZIP entries. The application currently limits an individual entry to 256 MiB and rejects archive entries it does not recognize, so it cannot silently discard data from a newer project version. It saves through a temporary file and replaces the destination only after the archive is complete.

## `project.json`

```json
{
  "format": "org.wafflecps.project",
  "schemaVersion": 1,
  "repeaters": [
    {
      "name": "North Ridge",
      "rxHz": 444500000,
      "txHz": 449500000,
      "colorCode": 1,
      "slots": {
        "1": [{ "name": "Local", "id": 9 }],
        "2": [{ "name": "Regional", "id": 3100 }]
      }
    }
  ]
}
```

Frequencies are integer hertz, avoiding rounding errors from decimal MHz. Time slots are stored separately. Talkgroup IDs are positive integers up to 16,777,215. Version 1 requires at least one repeater and both time-slot arrays for each repeater.

When a source codeplug is attached, the JSON adds:

```json
"sourceCodeplug": {
  "entry": "source/original-codeplug.bin",
  "fileName": "original.rdbx",
  "size": 13027285,
  "sha256": "hexadecimal SHA-256 of the original bytes"
}
```

The source's original filename is for display only; the archive entry name is fixed. On open, WaffleCPS verifies the ZIP checksum, byte count, and SHA-256 before accepting the project. A failed open leaves the current project in memory.

## Version 2 import data

When a codeplug has been imported, `schemaVersion` is `2` and `project.json` also contains `importedCodeplug`:

```json
{
  "model": "CS7000-M17 PLUS",
  "cpsVersion": "V1.02.20.00",
  "channels": [
    { "name": "Example", "frequencyAHz": 449350000, "frequencyBHz": 444350000,
      "digital": true, "colorCode": 2, "timeSlot": 1 }
  ],
  "zones": [
    { "name": "Example zone", "channelIndexes": [0] }
  ]
}
```

Channel indexes in JSON are zero based and zone member order is preserved. `frequencyAHz` and `frequencyBHz` follow the codeplug's stored field order; their RX/TX direction has not been independently verified. Repeater grouping is derived from the imported records when a project opens, so the original zone names and channel membership remain intact. Import does not yet decode contacts, receive groups, scan lists, or all radio settings. The original codeplug attachment remains the recovery source for those records. Radio conversion will require import and export adapters plus compatibility checks for the target radio.
