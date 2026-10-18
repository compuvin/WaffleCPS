# WaffleCPS Project Format

A `.waffle` file is a standard ZIP archive. It is an open working-project format, separate from any radio manufacturer's codeplug format. Version 1 stores repeater editor data and can keep an original codeplug as an untouched attachment. Version 2 adds imported channel and zone records. Version 3 supports empty projects, simplex frequencies on manually created repeaters, and imported contact records. All three versions remain readable.

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

## Imported data (versions 2 and 3)

When a codeplug has been imported, `project.json` also contains `importedCodeplug`. Version 3 stores contacts and channel references; older version 2 projects can recover these from their attached original codeplug when reopened:

```json
{
  "model": "CS7000-M17 PLUS",
  "cpsVersion": "V1.02.20.00",
  "channels": [
    { "name": "Example", "frequencyAHz": 449350000, "frequencyBHz": 444350000,
      "digital": true, "colorCode": 2, "timeSlot": 1, "contactIndex": 1 }
  ],
  "contacts": [
    { "name": "Local", "id": 9, "groupCall": true }
  ],
  "zones": [
    { "name": "Example zone", "channelIndexes": [0] }
  ]
}
```

Channel indexes in JSON are zero based; contact indexes are one based, with zero meaning no contact. Zone member order is preserved. `frequencyAHz` and `frequencyBHz` follow the codeplug's stored field order. The supplied Brooklyn repeater confirms that A is transmit and B is receive for this sample. Repeater classification is derived from imported records when a project opens; each matching zone gets its own repeater entry. A zone with one non-simplex digital repeater key may also contain simplex channels; these are displayed separately without losing zone membership. Adding a talkgroup creates a channel using the selected repeater's frequencies and color code, up to 16 channels in that zone. Adding a simplex channel appends a new imported channel and its index to the selected zone, under the same limit. Editing imported repeater details updates only that zone's repeater channels; shared channels are copied so other zones retain their settings. Removing a talkgroup removes its matching channel memberships from the selected zone. A channel record is deleted if no zone references it afterward; shared channels and other zones' memberships remain. The original source attachment remains unchanged. Import does not yet decode receive groups, scan lists, or all radio settings.

## Version 3 empty projects and simplex frequencies

When a manually created repeater has simplex frequencies, `schemaVersion` is `3`, and its repeater object includes an optional `simplexFrequencies` array:

```json
"simplexFrequencies": [
  { "name": "Local simplex", "frequencyHz": 446000000,
    "digital": true, "colorCode": 3, "timeSlot": 1 }
]
```

An imported codeplug can also appear in a version 3 project if manually created repeater data is present. Radio conversion will require import and export adapters plus compatibility checks for the target radio.

## Version 4 contact library

Projects with contacts added or edited outside an imported codeplug use `schemaVersion: 4` and a `contactLibrary` array. Each record has `name`, `id`, and `groupCall` fields. `groupCall: false` represents a person (private call), and `true` represents a talkgroup. The library stores contacts independently of repeater channels. Editing a talkgroup shown from a manual repeater also updates its matching channel entries and saves the contact in this library. Imported codeplug contacts continue to live in `importedCodeplug.contacts`, with channels referring to them by contact index.

Version 4 also permits a separate `channelName` on a manually created repeater slot entry. Its `name` and `id` identify the reusable talkgroup; `channelName` is the label shown for that particular radio channel. Older projects without this field display the talkgroup name as the channel name. Imported codeplugs already keep channel names separately in `importedCodeplug.channels[].name`.
