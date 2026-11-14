# TYT MD-9600 definition research

Checkpoint: 2026-11-12.

Work is deferred pending generic split-array and paired A/B list support
in WaffleCPS. No MD-9600 package has been created.

## Sample and verified findings

Sample: `Codeplugs/TYT9600V1.0.rdt` — 852,533 bytes.

An in-memory candidate derived from MD-380 decoded:

- 262 channels
- 7,775 contacts
- 10 zones: five populated and five empty
- 1 receive group
- 1 scan list

The candidate passed its declared inspection checks. Zone extension
references were checked separately and all referred to decoded channels.
This is inspection evidence, not verified Waffle import/export support.

## Layout findings

- Model text: `MD9600`, at file offset `0x125`.
- Channels: file offset `0x40235`, 64-byte records.
- Contacts: file offset `0x70235`, 36-byte records.
- Channel power: record offset 30, mask `0x03`;
  0 = Low, 2 = Medium, 3 = High.
- Zone extension records: file offset `0x31225`, 224 bytes each.

These are file offsets, not USB/radio memory addresses.

The sample uses the same extended A/B zone structure found in the
MD-UV390 research:

- First 16 A-list members are in the base zone record.
- Remaining 48 A-list members are in the extension.
- The extension also stores the complete 64-member B list.

The physical split must be hidden from users.

## Different A/B lists in this sample

All five populated zones have different A and B lists:

| Zone | A members | B members |
| --- | ---: | ---: |
| Morton/Peoria/BN | 15 | 30 |
| CENT IL LOCAL | 23 | 25 |
| PEO/BLM | 18 | 10 |
| TRAVEL | 1 | 1 |
| CHAM/DAN/lLAFYET | 12 | 19 |

The five remaining zones, Zone2 through Zone6, have empty lists.

Unlike the UV390 sample, this file exercises genuinely different A/B
assignments. It is a useful future regression fixture. Combining the
lists or copying one over the other would lose programming information.

## Deferred implementation checklist

- [ ] Add generic support for arrays split across storage regions.
- [ ] Represent both channel lists and their pairing in the shared model.
- [ ] Keep one sidebar entry per named zone or repeater.
- [ ] Present A/B lists within the editor.
- [ ] Offer shared editing only when appropriate; preserve differing
      imported lists.
- [ ] Define classification when A/B contain different repeaters or
      channel combinations.
- [ ] Declare addresses, capacities, and list roles in the WCP.
- [ ] Support import, export, starter generation, and project persistence.
- [ ] Preserve pairing when zones are reordered.
- [ ] Repair references in both lists when channels move or are deleted.
- [ ] Verify unchanged and edited export/reimport.
- [ ] Obtain hardware validation separately.

USB support remains a separate future step. Shared layout evidence
does not establish compatibility across every MD-9600 firmware version.

## References

- [MD-UV390 findings and interface recommendation](MD-UV390DefinitionResearch.md)
- [Existing project research](DMRProjectResearch.md)
- [DMRconfig larger TYT implementation](https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/uv380.c)
