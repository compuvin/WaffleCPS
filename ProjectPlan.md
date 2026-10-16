# WaffleCPS - a Visual DMR CPS

## Project Overview

Develop a modern, open-source, cross-platform DMR Customer Programming Software (CPS) application that makes programming DMR radios significantly easier and more intuitive than traditional CPS software.

The first codeplug target will be the **Connect Systems CS7000 M17 Plus**, with a modular architecture allowing additional DMR radios to be supported in the future.

Unlike conventional CPS applications that rely heavily on spreadsheets, channel lists, contact tables, and complicated menus, this application will use a **graphical, repeater-centered programming interface**.

The goal is to let users think about how their radio system works rather than how a manufacturer's codeplug is structured.

## 1. Visual Repeater Builder

The application's defining feature will be its repeater-centered builder.

Instead of manually creating digital contacts, channels, receive groups, and zones in separate screens, users will visually construct their repeater configurations.

The first interface will use an expandable hierarchy:

- **Repeater:** Name, RX/TX frequencies, and color code.
- **Children:** Time Slot 1 and Time Slot 2.
- **Below each time slot:** Assigned talkgroups.

Users will be able to expand and collapse repeaters, time slots, and talkgroups as they work.

A later graphical canvas can show the same relationships with nodes and connecting lines. Both views should edit the same underlying configuration.

Users will work on one repeater at a time.

## 2. Simplified Talkgroup Programming

The application will automatically inherit relevant programming information from parent nodes.

For example, when adding a talkgroup to Time Slot 2, the application already knows:

- The repeater's transmit and receive frequencies.
- The repeater's color code.
- The selected time slot.

The user only needs to select an existing talkgroup or create a new one.

If the talkgroup doesn't exist in the database, the application will guide the user through adding it without navigating to another screen.

Creating a new talkgroup typically requires:

- Talkgroup name.
- Numerical talkgroup ID.
- Group call type (default).

The software will automatically handle the necessary underlying channel and contact creation.

## 3. Automatic Zone Management

For newly created configurations, **one repeater will create one zone by default** in the radio. Imported codeplugs may contain several zones for one repeater, and those zones will remain separate.

Users should not need to manually create a zone when programming a new repeater.

The application will generate default zones and channel assignments automatically for new repeaters.

However, it will also provide a separate area for custom zones, such as:

- Simplex channels.
- Mixed repeater groups.
- Travel zones.
- Other configurations that don't follow the one-repeater-per-zone model.

Internally, repeaters and zones will remain separate objects to preserve flexibility.

## 4. Repeater Duplication and Reuse

Users will be able to duplicate an existing repeater configuration.

The duplicated repeater will retain its talkgroups, time-slot assignments, and other relevant configuration settings.

The user can then change the frequencies, name, and any other repeater-specific information.

This will be particularly useful for statewide DMR systems where multiple repeaters share almost identical talkgroup configurations.

Future enhancements could include applying talkgroup changes to multiple repeaters simultaneously.

## 5. Existing Codeplug Import

The application must be able to import an existing codeplug file or read a programmed radio and reconstruct its configuration in the repeater-centered interface.

This is more complicated than creating a new configuration because conventional codeplugs don't explicitly describe repeater relationships.

The application will analyze channels, RX and TX frequencies, color codes, time slots, contacts, and zone assignments to identify likely repeater groups. A zone whose channels are all DMR channels with the same RX frequency, TX frequency, and color code as a repeater will be categorized under that repeater. Zones containing channels from multiple repeaters or other channel types will remain zones. Same-frequency DMR groups and other ambiguous cases will be shown for review, and users can override classifications.

It will then:

1. Identify likely repeaters.
2. Reconstruct repeater trees and their assigned talkgroups.
3. Keep unmatched or mixed zones as zones, with their original channel order and membership.
4. Present uncertain classifications for user review.
5. Allow users to correct or reorganize imported configurations.

The import process must preserve the original configuration, including zone identity, channel order, and settings the application doesn't yet understand. Repeater classification is a view of imported data, not a destructive conversion. See [Import and Data Model](docs/ImportAndDataModel.md).

## 6. Application Interface

The interface will be a modern graphical desktop application, not a traditional spreadsheet-style CPS.

**Left navigation panel:**

- Repeaters
- Zones
- Talkgroup library
- Radio configuration

**Main workspace:**

Displays one repeater at a time using an expandable repeater, time-slot, and talkgroup hierarchy.

Users can add, edit, remove, or rearrange talkgroups in the hierarchy.

Selecting a repeater displays its entire programming structure in one place.

Custom and unmatched zones should use a visual channel-group editor with clear grouping, channel summaries, and ordering controls. A later graphical canvas could add zooming, automatic layout, and drag-and-drop editing.

A conventional advanced editor may be offered for users who need access to radio-specific settings.

## 7. Multi-Radio Architecture

The application will separate its graphical interface and internal programming model from radio-specific codeplug formats.

A shared internal data model will represent:

- Repeaters
- Frequencies
- Color codes
- Time slots
- Talkgroups
- Channels
- Contacts
- Zones
- Radio-specific settings

Individual radio adapters will convert this configuration to and from supported radio formats.

**Initial codeplug target:** Connect Systems CS7000 M17 Plus, matching the available sample file. Exact model and firmware compatibility must be verified before any radio writes.

**Future goal:** Additional Connect Systems models and other manufacturers' DMR radios.

Radio capabilities and limitations will need to be accounted for when generating codeplugs.

## 8. Technical Direction

**Recommended development stack:**

- Language: C++17 or C++20
- GUI framework: Qt 6
- Graphical interface: Qt Quick / QML
- Platforms: Linux and Windows
- License: Open source, with compatibility checked when reusing external code

Potential existing projects to investigate include QDMR, DMRconfig, and EditCP.

QDMR is especially interesting because it uses C++ and Qt and already implements significant portions of DMR codeplug management and radio communication.

The project should investigate reusing existing open-source code where practical rather than unnecessarily recreating radio programming protocols.

## 9. Installation and Usability

Ease of installation and use is a major priority.

Windows users should have a conventional installer.

Linux users should have a straightforward distribution option such as AppImage, with Flatpak potentially added later.

The software should operate offline, without accounts or subscriptions.

Radio-specific USB driver and device-permission requirements should be handled or clearly explained.

## 10. Initial Development Priorities

**Phase 1 – Repeater hierarchy prototype**

Create the expandable repeater builder using sample repeater data.

Implement the repeater node, two time-slot branches, and expandable talkgroup branches.

**Phase 2 – Programming logic**

Implement the internal configuration model, talkgroup library, automatic channel creation, zone generation, and repeater duplication.

**Phase 3 – Codeplug support**

Investigate the CS7000 M17 Plus codeplug format, implement safe import and export, and verify that existing configurations can be preserved accurately. Begin format research early enough to test the internal model against a real file before committing to the full editor.

**Phase 4 – Radio communication**

Implement reading and writing radio configurations, including backups, validation, and safeguards against corrupting codeplugs.

**Phase 5 – Additional radio support**

Expand the radio adapter system to support other DMR radios without major interface changes.

## Core Design Philosophy

**The user should program a repeater, not manually assemble all the individual pieces that make a repeater work.**

The application should understand the relationships among repeaters, time slots, talkgroups, channels, contacts, and zones—and handle the repetitive programming work automatically.

The graphical interface should make these relationships visible and understandable.

Ultimately, programming a DMR repeater should feel more like building a simple diagram than editing a spreadsheet. 
