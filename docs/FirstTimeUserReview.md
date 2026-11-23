# First-time user review

Review perspective: a new ham who has never programmed a DMR radio.
Each item can be accepted, changed, or skipped based on its benefit and cost.

## Reusable review prompt

Review WaffleCPS from two perspectives:
1. A new ham who has never programmed a radio.
2. An experienced ham who has never used DMR.

Walk through starting a project, adding a repeater and talkgroups,
managing channels and zones, using scan lists, saving, importing,
exporting, converting, and reading/writing a radio.

Look for unfamiliar terminology, unclear next steps, inconsistent
labels, hidden requirements, and confusing action results. Favor
clear labels and optional help over additional explanatory text
or warnings.

Compare findings with this document. Do not duplicate existing items
or reopen rejected suggestions. Propose only new, actionable items,
with the screen or action and the specific source of confusion.

State whether the review used source inspection, an interactive
walkthrough, or observation of an actual beginner. Do not present
predicted confusion as a confirmed usability problem.

Show the proposed additions for approval before editing this file.
Preserve existing wording and checkbox states.

## Getting started

- [X] Explain the two starting paths in New: create a project for a selected radio, or import an existing codeplug. The current radio picker shows definitions but does not explain when to use Import. (I have decided not to do this as it doesn't add benefit)
- [X] Explain that call sign and DMR ID can be entered later. The New Project form permits blank values, but does not say so.
- [ ] Review “Blank project” wording. “Start without a radio selected” is accurate; a new user may also need to know when to choose it.

## Programming a first repeater

- [ ] Add brief help for receive and transmit frequencies from the radio’s point of view, including where to find a repeater’s published settings.
- [ ] Explain color code and time slot at the point where users first encounter them. Keep the explanation short and optional.
- [ ] Review what happens after Add repeater. Check whether the next action, Add talkgroup, is clear without prior DMR experience.
- [ ] Review the talkgroup and channel-name fields together. Show a concrete example of one talkgroup used with a distinct channel name, if testing shows users confuse them.

## Understanding the project

- [ ] Explain the difference between a repeater, a zone, and a channel when those views are first used. The zone page already has a useful explanation; check whether it appears early enough.
- [ ] Review the Contacts page’s “People” and “Talkgroups” labels with a new user. Check whether “contact” makes sense as the name for both.
- [ ] Review the meaning of Save (`.waffle` project) and Export (radio codeplug file) in the menus. The separate Project and Codeplug menus help; add guidance only if users still confuse them.
- [ ] Check whether a user can tell which radio and file format their current project targets before exporting.

## Validation

- [ ] Have someone unfamiliar with DMR attempt a small project while describing what they expect each control to do. Record actual points of confusion before selecting changes from this list.

## Additional review — November 17, 2026

- [ ] Distinguish the user's DMR radio ID from a talkgroup ID. Both are numbers entered during setup, but they identify different things.
- [x] Review “Group” and “Private” in the channel contact picker. Relate these labels to “Talkgroups” and “People” used on the Contacts page.
- [ ] Provide optional help for analog CTCSS settings: what Transmit and Receive mean, and when to leave either set to None.
- [ ] Review “Top / Bottom” and “Both” with a first-time user. Check whether they understand these control channel availability on the radio's two displays.
- [ ] Explain scan-list editing availability where appropriate. The Scan Lists section disappears for unsupported definitions, which could leave users unsure whether their radio cannot scan or Waffle cannot yet edit its lists.
- [x] Replace implementation terminology in disabled Radio menu tooltips. “No USB transfer definition is available” should explain that Waffle does not currently support transferring directly to this radio.
- [x] Review the write-success message. “The radio acknowledged every codeplug block” should describe the completed action in plain language.
