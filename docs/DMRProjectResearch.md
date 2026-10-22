# DMR project research for WaffleCPS

Research date: 2026-10-21. Based on upstream source inspection; no upstream software was built and no USB operations were performed.

Addresses below are hexadecimal. A **file offset**, **packed image offset**, and **radio memory address** are different coordinate systems. They must not be substituted for one another.

## 1. Radios supported by each project

### Source revisions

Links in this document are pinned to the inspected revisions.

| Project | Repository | Revision |
|---|---|---|
| MD380Tools | `travisgoodspeed/md380tools` | `d7f4206c146a7580da5ff55c71784be6fc06b282` |
| DMRconfig | `OpenRTX/dmrconfig` | `f9bc00c4abc5f228af4882746120dbd94d5c9e99` |
| QDMR | `hmatuschek/qdmr` | `fb9c96af53f646c0706fa360209b2e0e6cd79a08` |
| EditCP | `dalefarnsworth-dmr/editcp` | `06317490b95ad41ef2f460659165e56c7c9d5627` |

EditCP's implementation also requires inspecting its pinned dependencies:

| Package | Version | Revision |
|---|---|---|
| `dalefarnsworth-dmr/codeplug` | `v1.0.28` | `a5dbd813d4932b50117aa3b8fc61d9685222d063` |
| `dalefarnsworth-dmr/dfu` | `v1.0.20` | `19d19851f8efeebdcc06f6f227ad2f5cbfb82068` |
| `dalefarnsworth-dmr/stdfu` | `v1.0.19` | `53d337e1f07a98eeeac45ab2de9adbd8492d79e4` |

Dependency versions come from EditCP's [go.mod][e-mod] and the DFU package's [go.mod][e-dfu-mod].

### MD380Tools

The README lists patched-firmware support for TYT/Tytera MD-380 and MD-390, including GPS variants, plus Retevis RT3 and RT8 equivalents. It distinguishes non-GPS and GPS firmware builds.

`md380-dfu` reads and writes MD-380-family codeplug images. This is separate from `md380-tool`, whose extended functions require the project's patched firmware.

**CS700 connection:** the README says its codeplug format resembles Connect Systems formats and suggests possible future compatibility. It does **not** claim an implemented CS700 adapter or verified CS700 USB programming. It provides no basis for assuming CS7000 compatibility.

Source entry points:

- [README.md][m-readme]: introduction, supported hardware, codeplug programming.
- [md380_dfu.py][m-dfu]: `download_codeplug`, `upload_codeplug`, `main`.
- [chirp/md380.py][m-chirp]: incomplete CHIRP driver; useful format research, not complete CPS support.

### DMRconfig

The README and `radio.c` identification table cover:

| Family | Models and equivalents |
|---|---|
| Original TYT | MD-380, MD-390; Retevis RT3 and RT8 |
| Larger TYT | MD-UV380, MD-UV390, MD-2017, MD-9600; Retevis RT3S and RT82 |
| Related TYT protocol | Baofeng DM-1701 / Retevis RT84 |
| Radioddity/Baofeng | GD-77, RD-5R, TD-5R, DM-1801 |
| AnyTone/BTECH | AT-D868UV, AT-D878UV, AT-D878UVII, DMR-6x2 |
| Other identified devices | Zastone D900, Zastone DP880, Radtel RT-27D |

Device descriptors select shared implementations. For example, MD-380, MD-390, D900, DP880 and RT-27D descriptors use functions from `md380.c`; the larger TYT models use `uv380.c`.

Sources:

- [README.md][d-readme]: advertised model list and commands.
- [radio.c][d-radio]: `radio_tab`, `radio_connect`, `radio_read_image`.
- [md380.c][d-md380] and [uv380.c][d-uv380]: device descriptors at the ends of the files.

There is no Connect Systems model in the inspected identification table.

### QDMR

The README and radio registry cover these stock-firmware families:

| Family | Models and aliases |
|---|---|
| TYT | MD-380/MD-390/RT8; MD-UV380/MD-UV390/RT3S; MD-2017/RT82 |
| Radioddity | GD-77, GD-73; RD-5R/RD-5R+ |
| AnyTone | AT-D868UVE, AT-D878UV, AT-D878UVII, AT-D578UV and its registered II alias, AT-D168UV |
| BTECH/Baofeng | DM-1701/RT84, DMR-6x2, DMR-6x2 PRO, BF/DR-1801 A6 family, DM-32UV |

QDMR also supports **firmware targets**:

- OpenGD77-family detection includes GD77, GD77S, RD5R, DM1801 and DM1801A.
- Its OpenUV380-family detection includes MDUV380, MD380, DM1701, DM1701RGB, MD9600 and MD2017.

Those names come from the firmware-info switch and are not independent hardware validation claims. In particular, an MD-9600 running supported open firmware must not be confused with stock MD-9600 support.

Important qualifications:

- `RadioInfo` explicitly aliases MD-380 to the MD-390 implementation, and MD-UV380 to MD-UV390.
- DR-1801/A6 has upstream stability warnings and requires explicit selection in the inspected detection code.
- DM-32UV also requires explicit selection in that detection path.
- OpenRTX source and registry entries exist, but `Radio::detect` returns `nullptr` in its OpenRTX branch. Its presence in the tree is not evidence of working end-to-end support.

Sources:

- [README.md][q-readme]: advertised support and release notes.
- [radioinfo.hh][q-radioinfo-h]: model aliases.
- [radio.cc][q-radio]: actual interface-to-radio dispatch.
- [opengd77_interface.cc][q-open-interface]: `OpenGD77Interface::identifier`.

No Connect Systems adapter is advertised or selected by the inspected registry.

### EditCP

The README explicitly advertises MD-380, MD-390 and Alinco DJ-MD40.

Its pinned codeplug package contains a broader model schema:

- MD-380, RT3, MD-390, RT3-G and DJ-MD40.
- MD-UV380, MD-UV390 and RT3S.
- MD-2017, MD-9600, RT90 and RT82.
- RT84, using the `DM-1701` model identifier.

This is a documentation discrepancy: the schema is broader than the README. Treat these as implemented schema targets, not independently confirmed hardware coverage.

Sources:

- [EditCP README.md][e-readme].
- [codeplug/codeplugs.json][e-schema]: `codeplugs` entries, model identifiers and record selection.
- [EditCP radio.go][e-radio]: radio menu actions.
- [codeplug/codeplug.go][e-codeplug]: `ReadRadio`, `WriteRadio`.

### Implications for WaffleCPS

These projects offer substantial references for MD-380-family radios and the owned DM-1701 when running the appropriate OpenGD77 firmware.

They do **not** establish adapters for CS700, CS701, CS750, CS800, CS800D or CS7000. MD380Tools' CS700 observation is a research lead, not a compatibility result.

## 2. Codeplug structures and field encodings

### MD380Tools: raw MD-380 image and CPS wrapper

The basic raw image is `0x40000` bytes, or 256 KiB. The supported `.rdt` wrapper is 262,709 bytes: a 549-byte header, the raw image and a 16-byte suffix. `main` checks the wrapper's size and `DfuSe` signature before extracting its payload.

Sources: [md380_dfu.py][m-dfu], especially `main`'s `write` branch; [applet/src/codeplug.h][m-codeplug].

The firmware-side header and CHIRP map identify these raw-image tables:

| Table | Raw offset | Record size | Capacity |
|---|---:|---:|---:|
| Text messages | `0x2180` | 288 bytes | 50 |
| Digital contacts | `0x5F80` | 36 bytes | 1,000 |
| RX group lists | `0xEC20` | 96 bytes | 250 |
| Zones | `0x149E0` | 64 bytes | 250 declared |
| Scan lists | `0x18860` | 104 bytes | See model implementation |
| Channels | `0x1EE00` | 64 bytes | 1,000 |

The experimental CHIRP driver maps only 20 scan lists, whereas DMRconfig models 250. Do not treat the unfinished driver as the final capacity authority.

Useful field mappings:

- Channel RX and TX are separate four-byte little-endian packed-BCD frequencies.
- Channel names occupy 32 bytes as 16 UTF-16LE code units.
- A contact has a three-byte little-endian ID, a flags byte and a UTF-16LE name.
- Common complete contact flags include `0xC1` for group without receive tone, `0xC2` for private without tone and `0xE1` for group with tone.
- Zones contain a 16-character name and 16 channel references.

Sources: [chirp/md380.py][m-chirp] → `MEM_FORMAT`; [applet/src/codeplug.h][m-codeplug] → `contact_t`, `zone_t` and table constants.

**WaffleCPS relevance:** this is not the current CS7000 format. In particular, the 36-byte UTF-16 contact record is different from Waffle's packed contact-name encoding. The MD-380 flags must not be transplanted into the CS7000 adapter.

### DMRconfig: executable C layouts and multiple image families

#### Original MD-380 family

[md380.c][d-md380] provides table offsets, capacities, C structures, record constructors and reference validation in one place.

Channel-relative fields include:

| Field | Offset / encoding |
|---|---|
| Mode | Byte `0`, low two bits; analog `1`, digital `2` |
| Slot | Byte `1`, bits 2–3 |
| Color code | Byte `1`, high nibble |
| TX contact reference | `+6`, 16-bit |
| Time-out timer | `+8`, six-bit value in 15-second units |
| Scan-list reference | `+11` |
| RX group reference | `+12` |
| RX frequency | `+16`, packed BCD |
| TX frequency | `+20`, packed BCD |
| Receive/transmit tone fields | `+24` / `+26` |
| Name | `+32`, 16 UTF-16 code units |

Frequency values represent 10 Hz steps. Follow the conversion helpers instead of interpreting packed BCD as an ordinary integer.

Contact records are 36 bytes: ID at `+0..2`, call type in the low part of byte `+3`, receive-tone flag in that byte, and name at `+4`. Type values are group `1`, private `2`, all-call `3`.

RX group records contain 32 contact references; scan records contain 31 member references plus priority, designated-transmit and timing fields. Validation distinguishes empty/special references from ordinary one-based references.

Sources: [md380.c][d-md380] → `channel_t`, `contact_t`, `grouplist_t`, `scanlist_t`, `md380_verify_config`; [util.c][d-util] → conversion helpers.

#### Larger TYT family

[uv380.c][d-uv380] models a packed `0xD0000`-byte image with:

- 3,000 channels at image offset `0x40000`, 64 bytes each.
- 10,000 contacts at `0x70000`, 36 bytes each.
- Base zones at `0x149E0`.
- Zone extensions at `0x31000`, 224 bytes per extension.
- Shared settings, message, RX group and scan-list regions.

The packed image omits an address gap in the radio. The DFU layer adds 832 blocks for the applicable high-image block numbers. Consequently, packed channel offset `0x40000` corresponds to radio address `0x110000`.

Sources: [uv380.c][d-uv380] → constants, `zone_ext_t`, device descriptors; [dfu-libusb.c][d-dfu] → `dfu_read_block`, `dfu_write_block`.

#### Radioddity/Baofeng family

[rd5r.c][d-rd5r], [gd77.c][d-gd77] and [dm1801.c][d-dm1801] describe separate `0x20000`-byte layouts.

They use channel banks beginning at `0x3780` and `0xB1B0`, plus bank validity information. Contacts and scan lists do not occupy the same locations across all models:

- RD-5R contacts: `0x1788`.
- GD-77/DM-1801 contacts: `0x17620`.

Inspect each model's structures and constructors for BCD frequencies, tone fields, text widths, reference bases and empty markers. Similar radio families still require separate adapters.

#### AnyTone family

[anytone_ht.c][d-anytone] and [anytone_ht-map.h][d-anytone-map] distinguish a packed host image from sparse device memory addresses.

Notable structures include:

- 64-byte channel records.
- BCD RX frequency and BCD TX offset, with a separate direction field.
- Zero-based contact references in channels.
- Separate zone names, channel lists and validity maps.
- 100-byte contact records: type at `+0`, ASCII name at `+1`, four-byte BCD ID at `+35`.
- Contact types private `0`, group `1`, all-call `2`.

That contact enum differs from the TYT enum. The map file is essential when translating packed image offsets to USB addresses.

### QDMR: reusable codecs with explicit model overrides

QDMR is the strongest architectural reference for Waffle's C++/Qt implementation:

- `Config` holds radio-independent objects.
- `Codeplug::Element` provides endian, bitfield, BCD and string accessors.
- `Codeplug::Context` maps objects to encoded indexes and back.
- Model codeplugs implement encoding, object creation and reference linking.
- `RadioInterface` handles transport separately from model codecs.

Sources: [codeplug.hh][q-codeplug], [config.cc][q-config], and the model implementations below.

#### TYT codecs

[tyt_codeplug.hh][q-tyt-h] and [tyt_codeplug.cc][q-tyt] contain shared channel, contact, zone, RX group, scan, message and settings elements.

Useful symbols:

- `ChannelElement::rxFrequency` / `txFrequency`: BCD8 little-endian, multiplied by 10 to obtain Hz.
- `ChannelElement::contactIndex`: explicit 16-bit little-endian reference.
- `ContactElement::dmrId`: explicit 24-bit little-endian ID.
- `ContactElement::callType`: group/private/all-call conversion.
- `ZoneElement::memberIndex` and `linkZone`: reference decoding and object linking.

[md390_codeplug.hh][q-md390] documents the original memory map. [uv390_codeplug.hh][q-uv390] documents physical segments `0x2000–0x40000` and `0x110000–0x1A0000`, including channels at `0x110000` and contacts at `0x140000`.

Some descriptive table comments contain arithmetic or size inconsistencies. Use executable offsets, sizes, getters and tests as the final source, rather than copying comment tables unchecked.

#### Other codecs

| Family | What to inspect |
|---|---|
| AnyTone | [anytone_codeplug.hh][q-anytone-h] and [anytone_codeplug.cc][q-anytone]: BCD big-endian fields, ASCII names, channel offsets, contact types and indexes |
| Stock GD-77 | [gd77_codeplug.hh][q-gd77]: banked channels, model-specific contacts, zones and scan lists |
| OpenGD77 | [opengd77_codeplug.hh][q-open-codeplug]: separate EEPROM and flash images |
| OpenUV380 | [openuv380_codeplug.hh][q-openuv-codeplug]: related configuration objects with different memory-bank placement |
| DR-1801 A6 | [dr1801uv_codeplug.hh][q-dr1801-codeplug]: bank structures for channels, contacts, zones, groups, messages and settings; firmware-specific format |
| DM-32UV | [dm32uv_codeplug.hh][q-dm32-codeplug]: 48-byte channel elements and explicit field-offset definitions |

For model variations, inspect the matching `*_codeplug.*`, `*_limits.*`, `*_filereader.*` and `test/*_test.*` files together.

QDMR also demonstrates preserving unsupported data: `TyTRadio::upload` can read the existing radio image before encoding supported changes and writing it back. That is useful precedent, but not proof that every unknown field survives every adapter.

Source: [tyt_radio.cc][q-tyt-radio] → `upload`.

### EditCP: declarative schema and generic field engine

The main format description is in the separate [codeplug/codeplugs.json][e-schema], not the UI repository.

It declares model selection, file sizes, header/trailer placement, record offsets, record counts, deletion markers, field bit offsets, widths, value types, defaults and reference targets. [generated.go][e-generated] is generated from this description.

Examples:

| Record | Original-family `.rdt` offset | Size |
|---|---:|---:|
| Contacts | `0x61A5` | 36 |
| RX groups | `0xEE45` | 96 |
| Zones | `0x14C05` | 64 |
| Scan lists | `0x18A85` | 104 |
| Channels | `0x1F025` | 64 |

These include the 549-byte header. They are not raw-image addresses.

Larger-family schemas place channels at `.rdt` offset `0x40235` and contacts at `0x70235`. Their 16-byte trailer sits between portions of the image rather than simply at EOF. `ReadRadio` and `WriteRadio` explicitly split or join those portions.

Sources: [codeplugs.json][e-schema]; [codeplug.go][e-codeplug] → `ReadRadio`, `WriteRadio`, file detection.

The generic field engine is particularly useful:

- [field.go][e-field] → `fieldOffset`, `bytes`, `storeBytes`: masks bitfields and resolves extension fields without replacing neighboring bits.
- Schema bit offsets use the engine's own bit-numbering convention; do not assume C-style least-significant-bit numbering.
- [misc.go][e-misc] → `bytesToFrequency`, `frequencyToBytes`, BCD and string helpers.
- [record.go][e-record] handles record behavior and references.

**WaffleCPS relevance:** this provides a concrete example of describing fields separately from the UI. It also demonstrates why file wrappers, raw payloads, field masks and references should have independent tests.

## 3. USB communication

### MD380Tools: PyUSB and TYT's DFU variant

Transport entry points:

- [md380_dfu.py][m-dfu] → `init_dfu`, `upload_codeplug`, `download_codeplug`.
- [DFU.py][m-dfu-base] → control transfers, state handling, address selection, erase and custom commands.

Protocol details:

- USB VID/PID: `0483:DF11`.
- DFU class control transfers use request types `0x21` host-to-device and `0xA1` device-to-host.
- `DNLOAD` carries writes and commands; `UPLOAD` returns data.
- Address selection uses command `0x21` plus a little-endian 32-bit address.
- Erase uses `0x41` plus an address.
- Programming mode uses vendor command `91 01`.
- The codeplug routines include additional `A2` commands and status polling.
- Codeplug payload blocks are 1,024 bytes; data transfer numbering begins at block 2.
- Original-family writing erases four 64 KiB regions beginning at zero.

This is not merely standard STM32 DFU: the TYT command sequence and state transitions matter. Codeplug programming must also be distinguished from firmware-upgrade bootloader mode.

### DMRconfig: three transport families and tracing

[radio.c][d-radio] → `radio_connect` tries:

| Transport | VID/PID | Main source |
|---|---|---|
| TYT DFU | `0483:DF11` | [dfu-libusb.c][d-dfu] |
| Radioddity HID | `15A2:0073` | [hid.c][d-hid], [hid-libusb.c][d-hid-usb] |
| AnyTone USB serial | `28E9:018A` | [serial.c][d-serial] |

**TYT DFU:** `dfu_init`, `identify`, `dfu_erase`, `dfu_read_block` and `dfu_write_block` implement identification, state transitions, erase and transfer. Larger images require block-address translation; transfer block numbers are not simply packed image offsets.

**Radioddity HID:** programming begins with the split `PROGRA` / `M` exchange and acknowledgements. Read/write commands carry an address and 32-byte payloads. `CWB` commands switch memory windows; `ENDR` and `ENDW` finish operations. `hid-libusb.c` supplies the OS-specific USB transport underneath the shared protocol.

**AnyTone serial:** `serial.c` uses a virtual serial port and `PROGRAM` / `QX` handshake, identification, `R`/`W` frames, addresses, lengths, checksums and acknowledgements. Inspect `serial_identify`, `serial_read_region`, and `serial_write_region`.

The command-line `-t` option enables protocol tracing. DFU code distinguishes ordinary tracing from higher-detail byte dumps through `trace_flag`. This is a useful reference for logging Waffle's future transport without coupling logging to its codecs.

Sources: [README.md][d-readme] for usage; transport files for exact sequences.

### QDMR: common interface, model tasks and several protocols

Start with [radio.cc][q-radio] → `Radio::detect`, then follow the selected interface and radio class. USB identifiers alone do not select a codeplug format.

| Family | Transport implementation and details |
|---|---|
| TYT | [tyt_interface.cc][q-tyt-interface] over [dfu_libusb.cc][q-dfu]; `0483:DF11`, 1,024-byte address-based DFU blocks, programming/identify/erase/reboot commands |
| Stock Radioddity | [radioddity_interface.cc][q-radioddity-interface]; `15A2:0073`, HID commands, 32-byte payloads, bank selection and retries |
| AnyTone | [anytone_interface.cc][q-anytone-interface]; Qt serial transport, 115200 baud, 16-byte blocks, big-endian frame addresses, additive checksum and ACK |
| OpenGD77/OpenUV380 | [opengd77_interface.cc][q-open-interface]; `1FC9:0094`, serial protocol, 32-byte transfers, EEPROM/flash distinction and 4 KiB flash-sector buffering |
| GD-73 | [gd73_interface.cc][q-gd73-interface] over [c7000device.cc][q-c7000]; `1206:0227`, framed bulk transfers on OUT `0x02` and IN `0x81` |
| DR-1801 A6 | [dr1801uv_interface.cc][q-dr1801-interface]; serial preparation, password/program-mode exchange, negotiated transfer speed and codeplug transfer |
| DM-32UV | [dm32uv_interface.cc][q-dm32-interface]; serial protocol with page-oriented transfer and address translation |

AnyTone interfaces cover both GD32 USB IDs `28E9:018A` and STM32 IDs `2E3C:5740`. DR-1801 and DM-32UV share adapter IDs `067B:23A3`, reinforcing the need for explicit protocol/model identification.

**Naming trap:** QDMR's `C7000Device` is used by the GD-73 transport. Its name is not evidence of support for the Connect Systems CS7000.

For orchestration, inspect [tyt_radio.cc][q-tyt-radio]:

- `run` handles task outcomes and cleanup.
- `download` reads aligned image segments.
- `upload` optionally downloads existing bytes, encodes changes, erases regions and writes segments.
- Qt signals report progress and errors.

This separation is directly relevant to Waffle: model codecs determine memory regions, transport classes implement packets, and radio tasks coordinate the operation.

### EditCP: UI → codeplug package → DFU package → platform backend

The call chain is:

```text
EditCP radio.go
  → codeplug.ReadRadio / WriteRadio
  → dfu.ReadCodeplug / WriteCodeplug
  → stdfu control-transfer backend
```

Source pointers:

- [EditCP radio.go][e-radio]: menu actions, confirmation, progress and cancellation.
- [codeplug/codeplug.go][e-codeplug]: wrapper/payload conversion.
- [dfu/dfu.go][e-dfu]: protocol, erase/read/write, status polling and reboot.
- [dfu/dfu_unix.go][e-dfu-unix]: initialization and transfer sizes.
- [stdfu/stdfu_unix.go][e-stdfu-unix]: `gousb` USB control transfers.
- [stdfu/stdfu_windows.go][e-stdfu-windows]: Windows ST DFU DLL bindings.

The DFU package originated as a Go translation of MD380Tools. It uses `0483:DF11`, 1,024-byte transfer blocks, 64 KiB erase blocks, programming-mode commands and the block-2 convention.

`readFlashTo`, `writeFlashFrom` and `eraseFlashBlocks` also contain the larger-radio address-gap translation. `ReadCodeplug` and `WriteCodeplug` reboot after completing their operations.

On Unix, `stdfu` uses `gousb` over libusb. Windows uses the ST DFU API, so its deployment requirements differ. The shared Go protocol package does not eliminate platform-specific driver handling.

### Reuse and follow-up priorities

For future Waffle work:

1. Use QDMR's model/codec/interface/task separation as the main architectural reference.
2. Use DMRconfig for compact protocol tracing and executable C record layouts.
3. Use EditCP's schema and masked field writes as a reference for maintainable field descriptions.
4. Use MD380Tools for the original TYT DFU sequences and firmware-side observations.
5. Investigate CS700 independently; none of this establishes CS7000 USB compatibility.

Licensing must be checked per reused file:

- DMRconfig: [BSD 3-Clause][d-license].
- QDMR and EditCP UI: GPLv3 license files.
- EditCP's codeplug, DFU and STDFU package source headers specify LGPLv3.
- MD380Tools has nonstandard beer-based wording in its README, while individual components have their own notices; the CHIRP driver carries a GPL notice. Do not assign one conventional license to the entire repository without further review.

[QDMR license][q-license] · [EditCP license][e-license] · [codeplug package license][e-codeplug-license] · [DFU package license][e-dfu-license].

The next verification step for any adapter is a controlled CPS comparison and hardware testing for the exact model and firmware. Existing source code is evidence of an implementation, not proof of compatibility with Waffle's radios.

[m-readme]: https://github.com/travisgoodspeed/md380tools/blob/d7f4206c146a7580da5ff55c71784be6fc06b282/README.md
[m-dfu]: https://github.com/travisgoodspeed/md380tools/blob/d7f4206c146a7580da5ff55c71784be6fc06b282/md380_dfu.py
[m-dfu-base]: https://github.com/travisgoodspeed/md380tools/blob/d7f4206c146a7580da5ff55c71784be6fc06b282/DFU.py
[m-chirp]: https://github.com/travisgoodspeed/md380tools/blob/d7f4206c146a7580da5ff55c71784be6fc06b282/chirp/md380.py
[m-codeplug]: https://github.com/travisgoodspeed/md380tools/blob/d7f4206c146a7580da5ff55c71784be6fc06b282/applet/src/codeplug.h
[d-readme]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/README.md
[d-radio]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/radio.c
[d-md380]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/md380.c
[d-uv380]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/uv380.c
[d-rd5r]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/rd5r.c
[d-gd77]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/gd77.c
[d-dm1801]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/dm1801.c
[d-anytone]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/anytone_ht.c
[d-anytone-map]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/anytone_ht-map.h
[d-util]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/util.c
[d-dfu]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/dfu-libusb.c
[d-hid]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/hid.c
[d-hid-usb]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/hid-libusb.c
[d-serial]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/serial.c
[d-license]: https://github.com/OpenRTX/dmrconfig/blob/f9bc00c4abc5f228af4882746120dbd94d5c9e99/LICENSE
[q-readme]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/README.md
[q-radioinfo-h]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/radioinfo.hh
[q-radio]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/radio.cc
[q-codeplug]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/codeplug.hh
[q-config]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/config.cc
[q-tyt-h]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/tyt_codeplug.hh
[q-tyt]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/tyt_codeplug.cc
[q-md390]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/md390_codeplug.hh
[q-uv390]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/uv390_codeplug.hh
[q-anytone-h]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/anytone_codeplug.hh
[q-anytone]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/anytone_codeplug.cc
[q-gd77]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/gd77_codeplug.hh
[q-open-codeplug]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/opengd77_codeplug.hh
[q-openuv-codeplug]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/openuv380_codeplug.hh
[q-dr1801-codeplug]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/dr1801uv_codeplug.hh
[q-dm32-codeplug]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/dm32uv_codeplug.hh
[q-tyt-radio]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/tyt_radio.cc
[q-tyt-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/tyt_interface.cc
[q-dfu]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/dfu_libusb.cc
[q-radioddity-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/radioddity_interface.cc
[q-anytone-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/anytone_interface.cc
[q-open-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/opengd77_interface.cc
[q-gd73-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/gd73_interface.cc
[q-c7000]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/c7000device.cc
[q-dr1801-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/dr1801uv_interface.cc
[q-dm32-interface]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/lib/dm32uv_interface.cc
[q-license]: https://github.com/hmatuschek/qdmr/blob/fb9c96af53f646c0706fa360209b2e0e6cd79a08/LICENSE
[e-readme]: https://github.com/dalefarnsworth-dmr/editcp/blob/06317490b95ad41ef2f460659165e56c7c9d5627/README.md
[e-mod]: https://github.com/dalefarnsworth-dmr/editcp/blob/06317490b95ad41ef2f460659165e56c7c9d5627/go.mod
[e-radio]: https://github.com/dalefarnsworth-dmr/editcp/blob/06317490b95ad41ef2f460659165e56c7c9d5627/radio.go
[e-license]: https://github.com/dalefarnsworth-dmr/editcp/blob/06317490b95ad41ef2f460659165e56c7c9d5627/LICENSE
[e-schema]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/codeplugs.json
[e-generated]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/generated.go
[e-codeplug]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/codeplug.go
[e-field]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/field.go
[e-misc]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/misc.go
[e-record]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/record.go
[e-codeplug-license]: https://github.com/dalefarnsworth-dmr/codeplug/blob/a5dbd813d4932b50117aa3b8fc61d9685222d063/LICENSE
[e-dfu-mod]: https://github.com/dalefarnsworth-dmr/dfu/blob/19d19851f8efeebdcc06f6f227ad2f5cbfb82068/go.mod
[e-dfu]: https://github.com/dalefarnsworth-dmr/dfu/blob/19d19851f8efeebdcc06f6f227ad2f5cbfb82068/dfu.go
[e-dfu-unix]: https://github.com/dalefarnsworth-dmr/dfu/blob/19d19851f8efeebdcc06f6f227ad2f5cbfb82068/dfu_unix.go
[e-dfu-license]: https://github.com/dalefarnsworth-dmr/dfu/blob/19d19851f8efeebdcc06f6f227ad2f5cbfb82068/LICENSE
[e-stdfu-unix]: https://github.com/dalefarnsworth-dmr/stdfu/blob/53d337e1f07a98eeeac45ab2de9adbd8492d79e4/stdfu_unix.go
[e-stdfu-windows]: https://github.com/dalefarnsworth-dmr/stdfu/blob/53d337e1f07a98eeeac45ab2de9adbd8492d79e4/stdfu_windows.go
