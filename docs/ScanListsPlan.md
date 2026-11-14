# Scan lists plan

Discussion checkpoint: 2026-11-11.
Planning only; implementation has not been approved.

## User experience

- [ ] Put SCAN LISTS below ZONES in the left sidebar.
- [ ] Display named scan lists like the existing repeater and zone entries.
- [ ] Provide an “+ Add scan list” button.
- [ ] Open the selected scan list in the main workspace.
- [ ] Use this plain-language introduction:
      “A scan list tells your radio which channels to check while scanning.”
- [ ] Do not explain scanning by referencing zones or require beginners
      to understand zones.

## Proposed basic editor

- [ ] Name the list.
- [ ] Add channels from a repeater.
- [ ] Search for and add individual channels.
- [ ] Display, remove, and reorder included channels.
- [ ] Create, rename, and delete scan lists.

“Add repeater” and “Add individual channels” were suggested labels;
exact controls and wording still need review.

## Decisions to revisit

- [ ] Confirm whether advanced scan controls belong in an expandable
      Advanced section rather than hiding the whole feature.
- [ ] Decide how to present channel-to-scan-list assignment simply.
- [ ] Decide how to handle replacing an existing assignment.
- [ ] Decide whether the sidebar sections need to collapse as they grow.

Scan-list membership and channel-to-scan-list assignment can be separate:
a channel may specify which list starts when scanning is activated there.
Verify each radio’s implementation before exposing this.

RX group lists are separate from scan lists. Do not combine their data
or controls.

Possible advanced options include priority channels, transmit/revert
behavior, and timing. Offer only settings supported by the definition.

## Definition and application work

- [ ] Start with DM-32UV, whose tester requested scanning.
- [ ] Research its complete scan-list storage and channel assignment
      using QDMR and available samples.
- [ ] Keep radio-specific addresses, limits, settings, and writer rules
      in the WCP definition.
- [ ] Add generic scan-list data and editing support to Waffle.
- [ ] Preserve unsupported settings.
- [ ] Repair references when channels or scan lists are deleted,
      reordered, or otherwise moved.
- [ ] Preserve lists and assignments through project save/reopen.
- [ ] Support import and export of edited lists and assignments.
- [ ] Extend other radio definitions individually after review.

## Current mapping checkpoint

- DM-32UV: scan-list names are mapped; membership and channel assignment
  need research.
- CS700/MD-380: membership and a channel scan-number field are present;
  assignment semantics and reference validation need review.
- CS7000/CS750/CS800/CS800D: membership is mapped; channel assignment
  remains unmapped.
- OpenGD77: no scan-list table is currently declared.

These are partial mappings, not claims of working scan-list editing.

## Verification

- [ ] Compare decoded data and edited exports with reference software.
- [ ] Verify list and channel references after additions, deletions,
      and reordering.
- [ ] Verify project persistence and export/reimport.
- [ ] Have the DM-32UV tester confirm actual scanning on hardware.
