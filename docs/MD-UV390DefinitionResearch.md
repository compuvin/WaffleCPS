# TYT MD-UV390 definition research

Current status: 2026-11-14. `definitions/md-uv390.wcp` now includes codeplug
import/export, paired Top/Bottom channel lists, a radio icon, and embedded
USB read/write instructions. Local file inspection and software checks have
passed; USB hardware verification remains pending.

The findings and implementation checklists below are the original
2026-11-12 research checkpoint. Its proposed interface is historical;
the implemented editor uses Top, Bottom, and Both channel assignments.

## Sample and verified findings

Sample: `Codeplugs/uv390-10121801.rdt` — 852,533 bytes.

An in-memory candidate derived from MD-380 decoded:
- 91 channels
- 39 contacts
- 9 zones
- 1 receive group
- 1 scan list

The candidate passed its declared inspection checks, but initially
decoded only the first 16 A-list channels per zone. That result does
not establish complete support.

Differences from MD-380:
- Model text is `MD-UV390` at file offset `0x125`; it needs nine bytes
  including its terminator.
- Channels start at file offset `0x40235`, with 64-byte records.
- Contacts start at file offset `0x70235`, with 36-byte records.
- Channel power is at record offset 30, mask `0x03`:
  0 = Low, 2 = Medium, 3 = High.
- Zone membership includes extension storage and separate A/B lists.

These are file offsets, not USB/radio memory addresses.

## Zone storage and radio behavior

Each named zone has:
- An A list for the upper display, supporting up to 64 channels.
- A B list for the lower display, supporting up to 64 channels.

The A list is physically split:
- First 16 members in the base zone record.
- Remaining 48 members in the extension record.

The extension record also contains the complete 64-member B list.

This storage split is invisible to the operator: 21 A-list channels
form one continuous list, not separate 16-channel and 5-channel zones.

The sample uses extension members:
- Damascus: 21 channels.
- Thurmont: 17 channels.

Every zone in this sample has identical A and B lists. Other codeplugs
may have different lists; they must not be merged or overwritten.

## Revised interface recommendation

Pending future review:
- [x] Keep one sidebar entry per named zone or repeater.
- [x] Hide the physical storage split.
- [x] Present Upper display (A) and Lower display (B) inside the editor.
- [ ] When both lists match, offer a shared-list view with
      “Use the same channels on both displays” enabled.
- [ ] Allow independent editing when that option is disabled.
- [x] Preserve differing imported lists and their order.
- [x] Define classification behavior when A and B describe different
      repeaters or channel combinations.

Separate A/B sidebar entries were considered and superseded by this
recommendation.

## Future implementation checklist

- [x] Add generic support for arrays split across storage regions.
- [x] Represent paired channel lists in the shared programming model.
- [x] Declare storage locations, limits, and list roles in the WCP;
      do not hard-code UV390 behavior into Waffle.
- [x] Implement complete import, export, starter generation, and
      project save/reopen for both lists.
- [x] Preserve list pairing through zone/repeater reordering.
- [x] Repair both lists when channels are moved or deleted.
- [x] Verify extended zones and differing A/B lists.
- [x] Verify unchanged and edited export/reimport.
- [ ] Obtain hardware validation separately.

Do not assume MD-UV380 compatibility solely from the shared family.
USB instructions are now included in the WCP; hardware testing remains separate.

## References

- [Existing project research](DMRProjectResearch.md)
- [DMRconfig UV380-family implementation](https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/uv380.c)
- [ASRC MD-UV390 operating guide](https://www.asrc.net/pub/ASRC-TYT-Radios.pdf)
