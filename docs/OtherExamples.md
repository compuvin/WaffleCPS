## Other Open-Source DMR Programming Projects

Several existing open-source projects are worth investigating for WaffleCPS.

| Project | What it does | Relevance to WaffleCPS |
|---|---|---|
| [QDMR](https://github.com/hmatuschek/qdmr) | Multi-radio graphical CPS and command-line utility | Best overall foundation |
| [DMRconfig](https://github.com/OpenRTX/dmrconfig) | Reads/writes codeplugs and programs various DMR radios | Potential radio-programming backend |
| [DMR Zone Channel Builder](https://github.com/AF1E/AF1E-New-Code-Plug) | Generates channels and zones from structured configuration | Useful codeplug-generation ideas |
| [dmrfill](https://github.com/jancona/dmrfill) | Adds repeater data to QDMR codeplugs | Useful for future repeater discovery |
| [codeplug-tool](https://github.com/pskillen/codeplug-tool) | Imports and exports structured radio programming information | Interesting approach to separating data and radio formats |
| [EditCP](https://github.com/dalefarnsworth-dmr/editcp) | Open-source CPS written in Go with Qt, supporting channels, contacts, zones, group lists, and drag-and-drop editing | Useful reference for interface design |
| [Codeplug Studio](https://github.com/pskillen/codeplug-studio) | Browser-based radio programming tool with DMR talkgroup management and radio-specific exports | Interesting model for separating programming data from radio-specific output |

### Projects Worth Prioritizing

**1. QDMR**

The strongest candidate for WaffleCPS's underlying codeplug infrastructure. Written in C++ using Qt, making it particularly relevant to our proposed technology stack.

**2. DMRconfig**

Worth investigating for radio communication and codeplug reading/writing functionality.

**3. Codeplug Studio**

Interesting for its separation of radio-independent configuration data from device-specific exports.

**4. EditCP**

Worth examining for interface ideas, particularly drag-and-drop editing.

### WaffleCPS Approach

WaffleCPS will initially use a traditional expandable file-tree interface, organizing repeaters, time slots, and talkgroups hierarchically.

A future version will introduce a graphical tree visualization, allowing users to build repeater configurations visually.

The goal is to reuse existing open-source codeplug and radio-programming functionality where practical while developing a significantly more intuitive user interface. 
