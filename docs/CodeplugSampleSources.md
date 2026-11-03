# Public codeplug sample sources

These public collections may provide test files for future WaffleCPS import adapters. Their listings have been checked, but the linked codeplug files have not been downloaded or validated. A listing for a radio model does not establish compatibility with another model or CPS version.

| Source | Listed samples | Why it may help |
|---|---|---|
| [Middlesex Amateur Radio Society DMR page](https://w1edh.weebly.com/dmr.html) | Connecticut club and ARES codeplugs for Connect Systems CS700 (`.rdt`, dated September 2016) and CS750 (`.rdb`, dated September 2016) | Older Connecticut examples from related Connect Systems radios. |
| [DMR.kiwi downloads](https://dmr.kiwi/downloads/) | Listings for CS700, CS750, CS800, Radioddity GD-77, TYT MD-380, and other radios | A range of models and manufacturers for later adapter tests. |
| [Connecticut ARES Region 3 digital modes](https://www.ctares-region3.org/digital-modes/) | DMR programming guidance and a reference to CTARES.org | Background on Connecticut ARES programming; this page does not provide a verified codeplug download. |

## Radios to investigate

These pages list downloadable codeplugs; the files have not been downloaded or checked in WaffleCPS. Match the exact model and CPS version before using a sample.

| # | Manufacturer | Model | Type | Codeplug listing |
|---|---|---|---|---|
| 1 | AnyTone | AT-D878UVII Plus | Portable | [NEDECN: v3.04 `.rdt` samples](https://nedecn.org/downloads/codeplugs/) |
| 2 | TYT | MD-UV380 / MD-UV390 | Portable | [Mid-Atlantic Wireless: MD-UV380/UV390 VHF](https://mawcg.org/codeplugs-and-configs/) |
| 3 | Baofeng | DM-1701 | Portable | [NEDECN: DM1701 `.rdt`](https://nedecn.org/downloads/codeplugs/) |
| 4 | Radioddity | GD-77 | Portable | [Mid-Atlantic Wireless: GD-77](https://mawcg.org/codeplugs-and-configs/) |
| 5 | BTECH | DMR-6X2 PRO | Portable | No verified sample listing found; [BTECH provides CPS and firmware](https://baofengtech.com/dmr/dmr-6x2-pro/). |
| 6 | AnyTone | AT-D578UVIII Plus | Mobile | [PI4UTR: AT-578 codeplug collection](https://codeplug.pi4utr.nl/) |
| 7 | Motorola | XPR 5550e | Mobile | [Rocky Mountain Ham Radio: XPR5550e High Power](https://www.rmham.org/sample-codeplugs/) |
| 8 | TYT | MD-9600 | Mobile | [PALS Repeater Group: MD-9600](https://www.palsdmr.com/radio-codeplugs/) |
| 9 | Retevis | RT3S | Portable | [VK2IDL: ORARC RT3S codeplug](https://vk2idl.com/index.php/retevis-rt3s/) |
| 10 | Radioddity | GD-88 | Portable | [Funkwelle: GD-88 codeplug package](https://www.funkwelle.com/download/radioddity-gd-88-dmr-codeplug) |
| 11 | Baofeng | DM-32UV | Portable | [Timz DMR: beginning codeplug](https://www.timzdmr.com/radios/Baof-DM-32UV/index.htm) |
| 12 | AnyTone | AT-D168UV | Portable | [New England `.rdt` repository](https://github.com/killcity/d168-codeplug-new-england) |
| 13 | Radioddity | GD-73 | Portable | Unknown; [QDMR supports this model](https://github.com/hmatuschek/qdmr#supported-radios). |
| 14 | Radioddity / Baofeng | RD-5R / RD-5R+ | Portable | Unknown; [QDMR supports these models](https://github.com/hmatuschek/qdmr#supported-radios). |
| 15 | TYT / Retevis | MD-390 / RT8 | Portable | Unknown; [QDMR supports these models](https://github.com/hmatuschek/qdmr#supported-radios). |
| 16 | TYT / Retevis | MD-2017 / RT82 | Portable | Unknown; [QDMR supports these models](https://github.com/hmatuschek/qdmr#supported-radios). |
| 17 | AnyTone | AT-D878UV (original) | Portable | Unknown; [QDMR supports this model](https://github.com/hmatuschek/qdmr#supported-radios). |
| 18 | AnyTone | AT-D868UVE | Portable | Unknown; [QDMR supports this model](https://github.com/hmatuschek/qdmr#supported-radios). |
| 19 | AnyTone | AT-D578UV / AT-D578UV II | Mobile | Unknown; [QDMR supports these variants](https://github.com/hmatuschek/qdmr#supported-radios). Verify compatibility separately from the AT-D578UVIII Plus above. |
| 20 | BTECH | BF-1801A6 / DR-1801UV A6 family | Portable | Unknown; [QDMR lists this family](https://github.com/hmatuschek/qdmr#supported-radios); upstream notes describe DR-1801UV A6 support as unstable. |
| 21 | BTECH | DMR-6X2 (non-PRO) | Portable | Unknown; [QDMR supports this model](https://github.com/hmatuschek/qdmr#supported-radios). |

The additional entries (13–21) are based on QDMR's [supported-radio list](https://github.com/hmatuschek/qdmr#supported-radios). Their codeplug download sources are **unknown**; QDMR is a reference for radio support and USB/codeplug implementation, **not** a verified source of vendor CPS codeplug files. QDMR also supports OpenGD77/OpenUV390-family firmware on certain radios; that is a separate compatibility target from their original firmware. Check exact model and firmware before assuming interoperability.

The Middlesex files are historical examples, not a source of current repeater frequencies or talkgroup assignments. DMR.kiwi describes its codeplugs as intended for New Zealand's AREC ZL-TRBO network. Any downloaded sample should be treated as a test fixture for its stated radio model and format, not as a configuration to write to a different radio.

## Proposed compatibility test library

Keep a small collection of legally reusable sample files outside the source tree, with each file's source URL, radio model, format, CPS version if known, date, checksum, and usage terms. For each supported adapter, an automated test could report whether WaffleCPS correctly recognizes:

- Manufacturer, model, and file version.
- Receive and transmit frequencies.
- Repeater and simplex classification.
- Zone membership and channel order.
- Contacts, talkgroups, and time slots.
- Radio settings that the adapter claims to support.

The current importer is for the tested CS7000 M17 Plus `.rdbx` layout. CS700, CS750, and other listed formats would require separate format research and adapters. They are potential future test cases, not existing WaffleCPS import support.
