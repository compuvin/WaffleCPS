# Public codeplug sample sources

These public collections may provide test files for future WaffleCPS import adapters. Their listings have been checked, but the linked codeplug files have not been downloaded or validated. A listing for a radio model does not establish compatibility with another model or CPS version.

| Source | Listed samples | Why it may help |
|---|---|---|
| [Middlesex Amateur Radio Society DMR page](https://w1edh.weebly.com/dmr.html) | Connecticut club and ARES codeplugs for Connect Systems CS700 (`.rdt`, dated September 2016) and CS750 (`.rdb`, dated September 2016) | Older Connecticut examples from related Connect Systems radios. |
| [DMR.kiwi downloads](https://dmr.kiwi/downloads/) | Listings for CS700, CS750, CS800, Radioddity GD-77, TYT MD-380, and other radios | A range of models and manufacturers for later adapter tests. |
| [Connecticut ARES Region 3 digital modes](https://www.ctares-region3.org/digital-modes/) | DMR programming guidance and a reference to CTARES.org | Background on Connecticut ARES programming; this page does not provide a verified codeplug download. |

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
