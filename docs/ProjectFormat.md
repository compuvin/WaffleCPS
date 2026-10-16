# WaffleCPS Project Format

A `.waffle` file is a standard ZIP archive. It is an open working-project format, separate from any radio manufacturer's codeplug format. Version 1 stores repeater editor data and can keep an original codeplug as an untouched attachment. Version 2 adds imported channel and zone records. Version 3 supports empty projects and simplex frequencies on manually created repeaters. All three versions remain readable.

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

Frequencies are integer hertz, avoiding rounding errors from decimal MHz. Time slots are stored separately. Talkgroup IDs are positive integers up to 16,777,215. Version 1 requires at least one repeater; version 3 allows an empty `repeaters` array in a new project. Each repeater has both time-slot arrays.

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

Channel indexes in JSON are zero based and zone member order is preserved. `frequencyAHz` and `frequencyBHz` follow the codeplug's stored field order. The supplied Brooklyn repeater confirms that A is transmit and B is receive for this sample. Repeater grouping is derived from imported records when a project opens. A zone with one non-simplex digital repeater key may also contain simplex channels; these are displayed separately without losing zone membership. Adding a simplex channel appends a new imported channel and its index to the selected zone. The original source attachment remains unchanged. Import does not yet decode contacts, receive groups, scan lists, or all radio settings.

## Version 3 empty projects and simplex frequencies

When a manually created repeater has simplex frequencies, `schemaVersion` is `3`, and its repeater object includes an optional `simplexFrequencies` array:

```json
"simplexFrequencies": [
  { "name": "Local simplex", "frequencyHz": 446000000,
    "digital": true, "colorCode": 3, "timeSlot": 1 }
]
```

An imported codeplug can also appear in a version 3 project if manually created repeater data is present. Radio conversion will require import and export adapters plus compatibility checks for the target radio.
