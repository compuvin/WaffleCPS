# Import and Data Model

This document describes the first import behavior for the CS7000 M17 Plus. It is a design target, not a claim that every field in the sample file has been decoded.

## Keep the codeplug structure intact

Import channels, contacts, receive groups, zones, scan lists, radio settings, and any other known records as separate objects. Give each imported object a stable identity. Keep its original order, source record reference, and fields that the editor does not understand yet. Preserve the original file as a backup for comparison and recovery.

The repeater hierarchy is an editable view built from those objects. Classifying a zone under a repeater must not delete the zone or replace its channel membership. Export should use the retained objects and validate all generated changes against the radio's limits.

## Repeater identity

For a DMR channel, the candidate repeater key is its exact normalized RX frequency, TX frequency, and color code. Frequency comparison should use integer Hz or the file's exact frequency units, never floating-point rounding. Time slot and talkgroup are children of this key, not part of the key.

Channels with different RX or TX frequencies or color codes belong to different repeater candidates. Analog channels have no DMR color code and cannot satisfy this rule. A same-frequency DMR channel may describe simplex or a hotspot, so the importer should show it as an uncertain candidate unless other evidence identifies a repeater.

## Zone classification

For each imported zone:

1. Resolve every member to its channel. A missing or unreadable member makes the zone uncertain.
2. If all non-simplex members are DMR channels with the same repeater key, classify the zone under that repeater. Time slots and talkgroups may vary. Put same-frequency members in an **Additional simplex frequencies** box while preserving their zone membership and order.
3. If non-simplex members have different repeater keys or include analog or unrecognized channels, keep the zone in **Zones**.
4. If every member is same-frequency, keep the zone in **Zones** because it may represent simplex or a hotspot.
5. Keep empty zones in **Zones**.
6. If several zones match one repeater, show all of them under that repeater with their own names and channel order. Do not merge them automatically.

Classification is a suggested view with a visible reason. Users can reassign a zone or leave it in **Zones**. Save the override separately from the imported records so reclassification does not erase it. Re-run automatic classification only for zones without a user override.

The importer may also group channels by repeater key when a codeplug lacks a matching zone, but it should label that group as inferred. It must not invent or rewrite a radio zone merely to display the group.

## Editor presentation

The initial repeater view is an expandable hierarchy: repeater details, Time Slot 1 and Time Slot 2, then talkgroups and their channels. Show each matching zone's name and membership as part of the repeater view.

The **Zones** view should show each remaining zone as an ordered set of channel cards or rows. Each channel summary should expose name, mode, RX/TX frequencies, and, for DMR, color code, time slot, and talkgroup. Grouping and filters can help users understand large mixed zones while preserving the radio's actual order. Users should be able to edit membership and order directly.

A later node canvas can use the same data and commands as the expandable hierarchy. It should not require a second import model.

## Validation examples

| Imported zone | Result |
|---|---|
| DMR channels on one repeater RX/TX pair and color code, across both time slots | Under the matching repeater |
| Two zones on the same repeater RX/TX pair and color code | Both under one repeater, still separate zones |
| One repeater RX/TX pair and color code plus same-frequency channels | Under the repeater; same-frequency channels in Additional simplex frequencies |
| Channels on two frequency pairs | Stays in Zones |
| One digital repeater plus analog simplex channels | Under the repeater; analog simplex channels in Additional simplex frequencies |
| Same-frequency DMR channels with no repeater evidence | Uncertain candidate for review |
| Unresolved channel reference | Uncertain zone for review |

Before export is considered safe, an import with no edits must reproduce every understood record and retain every unknown record. A round-trip comparison should report any difference; writing to a radio requires separate model and firmware compatibility checks.
