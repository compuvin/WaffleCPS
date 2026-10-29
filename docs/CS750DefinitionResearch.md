# CS750 definition research

## Channel power — verified, 2026-10-28

Controlled vendor CPS saves establish a three-level field at channel-record
offset `8`, mask `0x30`, shift `4`: `0 = low`, `1 = medium`, `2 = high`.
Value `3` has no established meaning and remains numeric in inspection.

The user changed only `ANSO Alert Msg` (physical slot 1, record at `0x12020`)
and saved `CS750-LowPower.rdb`, `CS750-MedPower.rdb`, and
`CS750-HighPower.rdb` in `Codeplugs/`. Every pair differs at exactly one
file byte, `0x12028`: Low `0x08`, Medium `0x18`, High `0x28`.
All three files pass definition inspection. The definition now declares this
mapping without changing its packaged template bytes.

The earlier candidate, bit `0x80` at offset `9`, was rejected by this test:
offset `9` stays `0xB0` in all three files. That bit's CS7000 meaning must
not be applied to CS750. Waffle now carries power values and choices from the definition, including Medium;
the legacy High/Low project representation remains readable.

## Zone selection and ordering — controlled samples, 2026-10-28

In `Codeplugs/CS750-W1OEM 2024-10-14.rdb`, the definition decodes 75
consecutive named zone slots beginning at `0x26000`, with an 80-byte stride.
The storage immediately following those slots is empty. There are no
empty-name gaps among the 75 decoded slots.

The current definition reads zones in physical slot order and determines the
slot count by scanning through the last nonempty zone name. It does not
decode a separate active-zone index. Whether such an index exists, and how
deleted slots or reordered zones are represented, remains unverified.
The contiguous sample does not establish those behaviors.

Verification needed: use CS750 CPS to save controlled copies after deleting
one middle zone and, if supported, changing zone order. Compare the files to
determine whether records move, slots are cleared, or a separate selection
or ordering structure changes. Confirm the displayed zone order against the
stored records. Until that evidence is available, do not add an assumed
zone-index mapping or change the definition's zone-selection behavior.

## Contact export limit and format versions — decision, 2026-10-28

The user approved **65,000 contacts** as the conservative export limit for
the CS750 format generation currently under investigation. This is a chosen
software limit, not a measured hardware maximum. It is now enforced by the
definition's target validation and contact-table capacity.

The manufacturer's [CS750 software history](https://www.connectsystems.com/products/top/radios/CS750_SOFTWARE.htm)
lists 2,000 channels and 65,536 contacts for older releases, including CPS
4.00.42 compatible with firmware 3.00.14 and 3.00.17. Its
[product page](https://www.connectsystems.com/products/top/radios/CS750.htm)
instead advertises 65,280 contacts, while the
[software overview](https://www.connectsystems.com/amateur-software/CS750_software.html)
summarizes that generation as 65,000. The chosen export limit is below the
two larger published figures. No boundary test has established the exact
maximum for the sample's format and firmware.

The sample `CS750-W1OEM 2024-10-14.rdb` contains model `CS750`, header
version `D4.0.03`, and secondary version `S3.00.14`. The definition currently
uses the header version for recognition. The secondary version is compatibility
evidence; its presence does not prove compatibility with every firmware or
CPS release. Do not equate the header marker with a firmware version.

Definitions should target specific codeplug formats. Firmware/CPS releases
verified to share the same layout can share a definition; incompatible layouts
should use separate definitions. Document supported versions and recognition
evidence rather than applying one CS750 layout to every release. Later
manufacturer releases advertise larger capacities and are not established as
compatible with this definition by this research.

### Zone deletion and designated-zone evidence

Comparing `CS750-HighPower.rdb` with `CS750-HighPower-ZoneDeleted.rdb`
establishes deletion of Killingly at physical zone index 29. Following 80-byte
records move back exactly one slot, the former final slot is cleared, and all
remaining zone names and member lists are unchanged (75 zones become 74).

The two other settings that change are zero-based references to Home, from
64 to 63. `CS750-PowerUp-Ansonia.rdb` isolates Power up Designated Zone at
`0x2227`: 63 becomes 0, with Home Zone unchanged. `CS750-Home-Ansonia.rdb`
isolates Designated Home Zone at `0x2248`: 63 becomes 0, with the startup
zone unchanged. Only the observed low byte at each location is declared;
any following bytes remain preserved. These values fit the 250-zone policy.
The writer remaps surviving source-zone indices and uses the first zone when
an old designation is removed. The packaged default sets both to zero.

The storage location of Power up Designated Channel is still unresolved.
Do not claim this setting is remapped or reset by the writer. A controlled
save changing only this selection is needed to establish its encoding.

## Native editor/export implementation

The definition now declares editor bindings, fixed-table rebuilding, contact
tail resizing, limits (2,000 channels, 250 zones, 65,000 contacts), reference
repair for receive groups and scan lists, and output decoding/comparison.
Unknown bytes in selected source records are retained. Mode changes/new
channels select the matching packaged mode template. Reference repair uses
physical source identities, not names, to handle reorderings and clones.

Unchanged exports preserve every byte. Radio identity-only edits skip table
rebuilding. A native test covers additions, deletions, contact tail resizing,
all three power choices, arbitrary enum labels and project persistence. The
large-sample test also reorders 35,041 contacts and deletes Killingly.
Vendor CPS acceptance remains to be tested after the startup-channel mapping
is resolved. Regenerating with the Python tool still builds a reset template;
it does not reconstruct these executable export rules.
