# Project checklist

This is a working checklist drawn from [ProjectPlan.md](../ProjectPlan.md), later project decisions, and the current prototype. It records progress without changing the plan. Checked items describe work already present; they do not imply that radio export is safe.

## Repeater-centered editor

- [x] Show one repeater at a time with details, Time Slot 1, Time Slot 2, and talkgroups.
- [ ] Let users expand and collapse repeater, time-slot, and talkgroup branches in the main hierarchy.
- [x] Add a talkgroup using its name and ID while inheriting the repeater frequencies, color code, and time slot in the imported view.
- [x] Keep multiple imported zones for the same physical repeater as separate entries with their own 16-channel limits.
- [x] Show additional simplex frequencies on a repeater and allow adding them.
- [x] Make Add talkgroup a single, easy flow backed by a reusable talkgroup library.
  - [x] Let users type a name or ID with suggestions, or choose from a list.
  - [x] Show only group call talkgroups for repeater channels; consider group and private calls for simplex where appropriate.
  - [x] Keep an existing talkgroup's display name and ID normally read-only when selected.
  - [x] Let users create a new talkgroup with its ID and display name in the same flow.
  - [x] Provide a separate channel name field, so a channel can be labeled "Brooklyn CT Wide" while using the "CT Statewide" talkgroup.
  - [x] Inherit the repeater frequencies, color code, and selected time slot automatically.
- [x] Make the existing Order channels button open one ordered list for the selected repeater zone.
  - [x] Let users interleave channels from Time Slot 1, Time Slot 2, and Additional simplex frequencies in any order.
  - [x] Preserve an imported zone's original channel order until the user changes it.
  - [x] Default newly generated repeater zones to Time Slot 2 channels, then Time Slot 1 channels, then Additional simplex frequencies.
  - [x] Keep repeater frequencies and color code on the repeater, rather than repeating them in channel editing controls.
- [ ] Duplicate a repeater.
  - [ ] Ask whether to copy only its frequencies and color code or the entire repeater entry, including time slots, talkgroups, and additional simplex frequencies.
- [x] Generate contacts, channels, and a default zone for a newly created repeater. Today manual repeaters are project-editor data only.
- [ ] Validate capacity using the selected radio model and firmware's limits, including channels per zone.
  - [ ] When a zone would exceed its limit, offer to split it into another zone or remove the extra channel or channels.
  - [ ] Preserve channel assignments and order when splitting a zone, and show the resulting zones for review.

## Visual repeater tree

- [ ] Offer a visual repeater tree as a view option after the core programming workflow works.
  - [ ] Place the repeater frequencies and color code at the bottom as the trunk.
  - [ ] Show Time Slot 1 and Time Slot 2 above the trunk.
  - [ ] Show the channels branching from their assigned time slots.
  - [ ] Show additional simplex frequencies in a literal box beside the tree.
  - [ ] Use the same repeater configuration as the main editor so changes appear in both views.

## Contacts

- [x] Add Contacts to the left navigation, opening a contact list in the main workspace.
  - [x] Make group and private contacts searchable by name or ID and show each contact's type.
  - [x] Use the same contact list when adding a talkgroup to a repeater, showing group contacts there.

## Imported codeplugs and zones

- [x] Import channels, contacts, and ordered zones from the tested CS7000 M17 Plus CPS version.
- [x] Classify a zone with one repeater frequency pair and color code as a repeater zone, even when it also contains simplex frequencies; show those frequencies separately in Additional simplex frequencies.
- [x] Keep zones with channels from multiple repeaters, unrelated non-simplex channels, or only simplex frequencies in Zones.
- [x] Preserve the original source bytes inside a `.waffle` project.
- [x] Remove a talkgroup only from the selected zone; keep a channel record if another zone uses it, and delete it if no zone uses it.
- [ ] Build the visual editor for mixed and custom zones, including membership and ordering.
  - [ ] Let users create a custom zone and add channels to it.
  - [ ] Let users manually correct a channel frequency in a zone.
  - [ ] Reclassify the zone after a correction; if its channels now fit one repeater, show that same zone under Repeaters while preserving its name, membership, and channel order.
  - [ ] Keep other zones unchanged when a corrected channel is shared.
- [ ] Decode receive groups, scan lists, radio settings, and other required records while retaining unknown fields.
- [ ] Verify import against controlled CPS edits and more than one representative codeplug.
- [ ] Compare an unchanged import/export round trip byte by byte before enabling export.
- [ ] Export a validated CS7000 M17 Plus codeplug without losing unsupported settings.

## Project format and reliability

- [x] Save and open an offline `.waffle` project containing editor data and an untouched source attachment.
- [x] Provide New, Open, Import, Save, and Save As actions.
- [x] Fix the New-button crash after confirming a reset from an imported project; verify the window stays open with an empty project.
- [ ] Cover edit, save, reopen, and zone-membership changes with realistic project tests.
- [ ] Add clear user-facing errors and recovery steps for unsupported or damaged files.
- [ ] Keep imported source data and derived repeater classification distinct so edits can be reviewed and exported safely.

## Radio settings

- [ ] Add a separate visual Radio settings area that shows the supported radio model.
  - [ ] Let users select a pictured radio button to assign its supported function.
  - [ ] Let users select the pictured display to adjust supported display settings, such as brightness and startup messages.
  - [x] Provide clear controls for call sign, DMR ID, and power settings.
  - [ ] Use dropdowns and checkboxes for settings that do not benefit from a visual control.
  - [ ] Add other radio-specific settings as their formats and behavior are understood.
- [ ] Optional: consider a live display preview after the settings page works, if it can accurately reflect the selected radio's behavior.

## Radio support and distribution

- [ ] Separate the shared programming model and interface from radio-format adapters.
- [ ] Ask the user to select a target radio model when creating a new codeplug, so its limits guide editing from the start.
  - [ ] Allow the target radio model to be changed later.
  - [ ] When the target changes, check the entire configuration against the new model and firmware's capabilities and limits.
  - [ ] Show anything that does not fit and prompt the user to correct it, including splitting zones or removing extra channels where needed; do not silently discard settings.
- [ ] Add a cross-radio conversion workflow after source import and target export adapters are reliable.
  - [ ] Let the user choose a target radio model.
  - [ ] Show which channels, zones, contacts, and settings can transfer, which need adjustment, and which the target cannot support.
  - [ ] Require review of the converted configuration before exporting it for the target radio.
- [ ] Verify exact model and firmware compatibility before any radio write.
- [ ] Add radio read/write with backup, validation, and recovery safeguards.
- [ ] Investigate QDMR, DMRconfig, EditCP, and the other projects in [OtherExamples.md](OtherExamples.md) for reusable code and ideas; check licenses before reuse.
- [x] Choose and publish an open-source license for WaffleCPS.
- [ ] Linux support
  - [x] Build and test on Linux.
  - [ ] Provide a straightforward Linux package, such as an AppImage; consider Flatpak later.
- [ ] Windows support
  - [ ] Build and test on Windows.
  - [ ] Provide a conventional Windows installer.
- [ ] macOS support
  - [ ] Build and test on macOS.
  - [ ] Provide a macOS app bundle and decide on signing and distribution.
- [ ] Document USB drivers or device permissions on each supported platform when radio communication arrives.
- [x] Keep the app usable offline, without accounts or subscriptions.
- [ ] Support additional radios only after the shared model and first adapter are proven.

## Radio models to support

Treat each model and firmware variant as a separate compatibility target. File import, file export, and direct radio communication need their own verification for each target.

- [ ] Connect Systems CS7000 GPS — owned radio; the codeplug used for current import work came from this radio, which runs the same firmware as the Plus.
- [ ] Connect Systems CS700 — owned radio.
- [ ] Connect Systems CS701 — owned radio.
- [ ] Connect Systems CS750 — public sample codeplugs listed in the research note; no owned radio identified.
- [ ] Connect Systems CS800 — owned radio.
- [ ] Connect Systems CS800D — owned radio.
- [ ] TYT MD-380 — codeplug available; no radio available for direct device tests.
- [ ] Radioddity GD-77 — public sample codeplugs listed in the research note; no owned radio identified.
- [ ] Baofeng DM-1701 running OpenGD77 — owned radio; verify its firmware version before adapter work.

## Possible future items

- [ ] Consider applying a talkgroup change across multiple repeaters after duplication and a shared talkgroup library work. The workflow needs careful design because it could confuse users and is difficult to implement with the current setup.

## Agreed implementation order

- Complete and verify codeplug file export, including unchanged import/export round-trip checks, before implementing direct radio writing.
