# Sample Codeplug Research

The private sample is in `Codeplugs/`, which Git ignores. Do not copy the file or its owner-specific identifiers into public fixtures.

## Confirmed from read-only inspection

- The sample is a 13,027,285-byte `.rdbx` binary file.
- Decoding its repeating 256-byte mask in memory reveals `CS7000-M17 PLUS` and CPS version `V1.02.20.00` in the file metadata.
- A fixed-width UTF-16LE name table contains 821 consecutive nonempty channel-name slots.
- A zone table contains 82 zones with ordered one-based channel references, including repeater groups and separate simplex, GMRS, receive-only, and hotspot zones.
- Additional populated areas include radio settings and other record types. Their full meaning has not been established.

These observations show that a repeater-only editor would leave significant parts of a real codeplug without a home. They also give us a large test case for import performance and for the mixed-zone view.

## CS7000 default baseline

`Codeplugs/CS7000 Default.rdbx` is a second private sample (13,022,219 bytes). It uses the same repeating mask and identifies the model as `CS7000-M17 PLUS`, but reports CPS version `V1.02.21.00`. The known channel-name and record offsets yield two channels, `ACH 1` and `DCH 1`, both at 400.000 MHz receive and transmit. Their channel records have the High Power bit set. No contacts appear at the known contact table offset. The first zone at the known zone offset is `Zone 1` and references both channels.

WaffleCPS imports this CPS `V1.02.21.00` file using the same channel and zone layout observed in the populated `V1.02.20.00` sample. The default has one zone; bytes following its two channel references are not a second zone name. Its contact table starts 5,066 bytes earlier because the file is 5,066 bytes shorter; in both observed files, the table starts `0x7EF415` bytes from the end. The default provides a baseline for distinguishing default values from settings in the populated sample, but it does not by itself establish the meaning of unknown fields.

### Radio settings for the first file-export experiment

At decoded offset `0x1300`, the default file contains the UTF-16LE text `My Radio`; the populated file contains the owner's call sign at the same offset. This is strong evidence for a radio-name field that can hold a call sign. A later controlled edit located the DMR radio ID, as described below.

The small Radio Settings page now shows a read-only model/CPS version label and lets users edit the radio name or call sign and numeric DMR ID. It stores those values separately from contact IDs in the `.waffle` project while preserving attached source bytes. For a first `.rdbx` export test, begin with an unchanged byte-for-byte round trip, then patch only the confirmed radio-name and DMR-ID fields in a **copy** of a matching-version source file and compare the output to a CPS-saved control. Do not use an exported test file for radio writing until the format and checks are validated.

Controlled CPS-saved copies from one baseline are the best way to verify field mappings: change only the radio name in one, and only the radio's own DMR ID in another, while keeping the CPS version fixed. These files can stay in the Git-ignored `Codeplugs/` directory.

The supplied `CS7000 Default plus RID.rdbx` is such a controlled comparison for the default CPS `V1.02.21.00` file. The only decoded changes are the UTF-16LE radio name at `0x1300`, a little-endian 32-bit radio ID at `0x5A778` (from 1 to 3109101), and the byte immediately after it at `0x5A77C` (from `0x01` to `0x09`). The ID record is not at a fixed absolute offset: the populated file has the same ID and following bytes at `0x5BB42`, exactly 5,066 bytes later, matching the difference in file lengths. In these samples its offset measured from the end of the file is `0xC10C93`. The meaning of the adjacent `0x09` byte and the length or anchor rules for this section remain unverified.

`CS7000 Default plus RID2.rdbx` is the same size and CPS version as the first controlled copy, with the same radio name. Its radio ID is 3208609. Compared with the first controlled copy, **only the three low bytes of the little-endian ID field change** (`ED 70 2F` to `A1 F5 30`); the fourth byte remains zero and the following byte remains `0x09`. The adjacent byte does not vary with these two ID values. Its meaning is still unknown, so a first export experiment should preserve it from the source rather than generate it.

The supplied `KB1JDX 2026-10-19.rdbx` still reports CPS `V1.02.20.00` and is byte-for-byte identical to `KB1JDX 2026-02-23.rdbx` after decoding. It does not provide a newer-version populated layout for comparison. The controlled default comparison nevertheless locates the first radio settings fields needed for a small settings UI.

## Verified first import

For the sample's CS7000-M17 Plus CPS V1.02.20.00 layout, WaffleCPS decodes the 256-byte repeating XOR mask, 821 channel names and frequency records, 335 contacts, and 82 zones with ordered one-based channel references. Contact records hold a group/private flag, a 24-bit DMR ID, and a sixteen-character packed name. The name occupies the record's final eight bytes and the following record's four-byte prefix; padding all sixteen positions is required or CPS shows trailing zeroes. Digital channel records hold one-based contact references. The digital marker, color code nibble, and time slot bit are inferred from the sample records. Allowing simplex channels alongside one repeater key classifies 67 sample zones as distinct repeater entries; 15 zones remain in Zones. Zones on the same physical repeater retain separate entries and their 16-channel limits. The source bytes are archived unchanged in a `.waffle` project.

The supplied Brooklyn repeater settings confirm that the sample record's first frequency field is transmit (440.73750 MHz) and the second is receive (445.73750 MHz), with color code 3. CPS versions other than `V1.02.20.00` and `V1.02.21.00` are rejected because their offsets have not been checked. Imported codeplug data can gain simplex channels and repeater-detail edits in the `.waffle` project, but the original `.rdbx` stays unchanged. Receive groups, scan lists, and most other radio settings are not yet decoded.

## Channel-field evidence from the CPS export

`Codeplugs/JDX-Channels_2026-02-23.xlsx` contains 787 digital and 32 analog channel rows. Each of those 819 channel names matches a unique record in the original `.rdbx` file; two additional binary channel records do not appear in the workbook's channel sheets. The 48-byte channel records begin at decoded file offset `0x23768`. Offsets below are **zero based within a channel record**, after the file's repeating XOR mask is removed. These mappings come from comparing workbook values with the corresponding records. Fields that never vary in the workbook cannot be located from this comparison alone.

The user may have edited the codeplug after exporting the workbook. Continue using the workbook for field names and candidate values, but treat mismatches with the binary file as version differences to verify, not as proof that a byte mapping is wrong. Controlled CPS edits with a saved before/after pair provide stronger evidence.

| Record offset | Observed relationship | Status |
|---|---|---|
| 8 | Digital `0x50` corresponds to High RX/TX reference frequency; `0x28` to Middle. The workbook gives RX and TX the same value on every digital row, so their independent encoding is unknown. | Correlation |
| 9 | Bit `0x02` matches RX Only, `0x10` matches Talk Around, and `0x80` matches High Power. | Workbook correlation; power also confirmed by controlled copy below |
| 10 | Digital TX Admit: `0x00` Always, `0x10` Color Code Free, `0x08` Channel Idle. | Correlation |
| 11 | Bit `0x04` matches a 60-second transmit time-out; zero matches Infinite. | Correlation |
| 16 | Low nibble is color code; bit `0x10` is Time Slot 2. Bit `0x20` matches Private Call Confirmed and bit `0x80` matches Data Call Confirmed. | Correlation for confirmation bits |
| 22 | Digital RX Group List: `0x00` None, `0x01` TS 1, `0x02` TS 2, `0x03` Alert TS1, `0x04` Alert TS 2, `0x05` ReceiveAll in the tested CPS version. The workbook labels 32 channels Alert TS 2 even though their binary records have `0x00`; the codeplug may have been edited after workbook export. | None, TS 1, TS 2, and Alert TS 2 values confirmed by controlled copies below; `0x03` and `0x05` correlated |
| 34–35 | Analog transmit CTCSS: `FF FF` for none; `10 00` for 100.0 Hz; `67 15` for 156.7 Hz in the sample. | Correlation |
| 40 | `0x01` matches analog CTCSS Tail Revert On; zero matches Off. | Correlation |

Other variable bytes include offsets 26, 28, 32–33, 42–43, and bits not identified in the table. Offsets 26 and 28 strongly track channel mode, but their exact meanings remain unverified. The workbook shows no variation for many settings, including Scan/Roam/Vote List, so it cannot establish their binary locations. All unrecognized bytes must be preserved during export.

## Controlled Brooklyn channel copies

`Codeplugs/Waffle Test.rdbx` is a copy of the original codeplug with four added copies of `BKLN CT S WIDE`. The original channel record remains byte-for-byte unchanged. Channel names are stored in a separate table. `BKLN TEST 1` and `BKLN TEST 2` each differ from the original at one channel-record byte. The later `BKLN TEST 3` and `BKLN TEST 4` are both Low Power and differ from each other only at the RX group byte.

| Channel | CPS change reported by the user | Only record-byte difference |
|---|---|---|
| `BKLN TEST 1` | RX Group List TS 2 to TS 1 | Offset 22: `0x02` to `0x01` |
| `BKLN TEST 2` | Power High to Low | Offset 9: `0x80` to `0x00` |
| `BKLN TEST 3` | RX Group List Alert TS 2; Low Power | Offset 9: `0x80` to `0x00`; offset 22: `0x02` to `0x04` relative to original |
| `BKLN TEST 4` | RX Group List None; Low Power | Offset 9: `0x80` to `0x00`; offset 22: `0x02` to `0x00` relative to original |

This confirms the observed RX group index changes and High/Low power bit for this CPS version and these settings. It does not establish every other channel field. The four test channels are records 822–825 in the decoded channel table; the original is record 102. Keep the original and test files in the Git-ignored `Codeplugs/` directory.

WaffleCPS now reads the High/Low bit for imported channels and summarizes repeater-channel power in the repeater view. Changing a repeater's power changes its non-simplex channels in that zone; simplex channels retain their own values. Prototype export writes known fields and preserves unknown bytes from its source or embedded default; CPS and radio acceptance remain to be validated.

The user also assigned all four test channels to a scan list named `Scan Test 1`. Their 48-byte channel records contain no additional change for this assignment. In the decoded file, an existing empty `Scan List 1` name at offset `0x53e42` was changed to `Scan Test 1`, and four one-based 16-bit channel numbers (822–825, `0x0336`–`0x0339`) were added at offset `0x53e84`. A byte at `0x53e68` changed from `0x00` to `0x05`; its meaning is not yet confirmed. The same four channel numbers also appear appended to a separate index table at `0x2a92`, which appears to track all channel records rather than scan membership. This indicates that scan membership is stored in a separate list record, but the full scan-list structure and count rules still need verification before export.

## RX group workflow to revisit

New users should not have to create or select RX group lists when adding a talkgroup. A future WaffleCPS exporter may assign or generate the needed RX group records automatically, with an optional advanced control for receiving additional talkgroups on a channel. Preserve imported RX group assignments. The tested CPS permits `None` on digital channels, but its receive behavior on the CS7000 GPS/M17 Plus firmware has not been verified; do not choose an export default based on another radio's behavior alone. This is a research note, not an implemented feature or a change to the locked project checklist.

## Work needed before production export

1. Verify the frequency field direction across more channels and confirm mode and digital flag variants, contact references, and other channel settings against controlled CPS edits.
2. Confirm channel and zone limits and record offsets in other CPS versions.
3. Map receive groups, scan lists, and radio-specific settings, including unknown bytes that must survive a round trip.
4. Compare a small set of controlled CPS edits against binary copies to confirm field locations and checksums or length fields.
5. Test unchanged import and export on copies before trying radio communication. Never use the sole original file as an output target.

The sample identifies the **M17 Plus** variant. Compatibility with other CS7000 models or firmware versions is an open question; the adapter should check model metadata rather than treating all CS7000 files as interchangeable.
