#include "channel_order_fixture.h"
#include "../src/repeaterstore.h"
#include "../src/ziparchive.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QDebug>
#include <stdexcept>

namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
QVariantList members(const RepeaterStore &store) {
    return store.selectedImportedGroup().value("zones").toList().first().toMap().value("channels").toList();
}
QVariantList numbers(const RepeaterStore &store) {
    QVariantList result;
    for (const auto &member : members(store)) result.append(member.toMap().value("number"));
    return result;
}
int groupIndex(const RepeaterStore &store, const QString &name) {
    int index = store.importedRepeaterNames().indexOf(name);
    return index >= 0 ? index : store.importedRepeaterNames().size() + store.importedZoneNames().indexOf(name);
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Cannot open test output");
    return file.readAll();
}
QByteArray decoded(QByteArray bytes) {
    const auto mask = bytes.mid(0x300, 256);
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = bytes[i] ^ mask[i % 256];
    return bytes;
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "No temporary directory");
        QString error;
        const auto source = channelOrderFixture(&error);
        RepeaterStore store;
        check(store.importRadioCodeplug(source), "Cannot import fixture");
        check(store.createCustomZone("Mixed"), "Cannot create custom zone");
        check(!store.createCustomZone("Mixed"), "Duplicate zone accepted");
        check(store.addExistingChannelToSelectedZone(1) && store.addExistingChannelToSelectedZone(3)
              && store.addExistingChannelToSelectedZone(6), "Cannot add shared channels");
        check(!store.addExistingChannelToSelectedZone(1), "Duplicate membership accepted");
        check(!store.updateSelectedZoneChannelFrequencies(2, "bad", "449.350"), "Invalid frequency accepted");
        check(store.updateSelectedZoneChannelFrequencies(2, "444.400", "449.400"), "Cannot correct shared channel");
        check(numbers(store) == QVariantList({1, 7, 6}), "Shared correction changed membership position");
        check(members(store)[1].toMap().value("name").toString() != "TS1 Alpha 2", "Copy did not get a unique name");
        store.leaveZoneEditor();
        check(!store.selectedImportedGroup().value("isRepeater").toBool(), "Mixed frequencies classified as repeater");
        check(store.reorderSelectedZone({7, 1, 6}), "Cannot order mixed zone");
        check(store.renameSelectedZone("Corrected"), "Cannot rename zone");
        check(store.updateSelectedZoneChannelFrequencies(1, "444.350", "449.350"), "Cannot finish correction");
        check(store.importedZoneNames().contains("Corrected") && !store.selectedImportedGroup().value("isRepeater").toBool(),
              "Zone moved while still being edited");
        const auto project = temp.filePath("corrected.waffle");
        check(store.saveProjectToFile(project), "Cannot save edited zone");
        check(store.importedZoneNames().contains("Corrected"), "Save moved zone");
        const auto exportPath = temp.filePath("corrected.rdbx");
        check(store.exportCodeplugToFile(exportPath), "Cannot export edited zone");
        check(store.importedZoneNames().contains("Corrected"), "Export moved zone");
        ImportedCodeplug parsed;
        const auto output = readFile(exportPath);
        check(CodeplugReader::read(output, &parsed, &error), qPrintable(error));
        check(parsed.channels.size() == 7 && parsed.zones.last().name == "Corrected"
              && parsed.zones.last().channelIndexes == QVector<int>({6, 0, 5}), "Export changed custom zone order or name");
        check(parsed.zones[0].channelIndexes == QVector<int>({3, 0, 1, 4, 2, 5})
              && parsed.zones[1].channelIndexes == QVector<int>({2, 3}), "Other zone membership changed");
        const auto before = decoded(source);
        const auto after = decoded(output);
        for (int i = 0; i < 6; ++i)
            check(before.mid(0x23768 + 48 * i, 48) == after.mid(0x23768 + 48 * i, 48), "Other channel record changed");
        check(before.mid(0x23768 + 48 * 2, 48) == after.mid(0x23768 + 48 * 6, 48), "Copy lost original channel's unknown settings");
        QMap<QString, QByteArray> entries;
        check(ZipArchive::read(project, &entries, &error) && entries.value("source/original-codeplug.bin") == source,
              "Original attachment changed");
        RepeaterStore reopened;
        check(reopened.openProjectFromFile(project), "Cannot reopen edited zone");
        reopened.selectImportedGroup(groupIndex(reopened, "Corrected"));
        check(numbers(reopened) == QVariantList({7, 1, 6}), "Saved membership/order changed");
        check(reopened.exportCodeplugToFile(temp.filePath("reopened.rdbx"))
              && readFile(temp.filePath("reopened.rdbx")) == output, "Source lineage lost after save/reopen");
        // The target is captured before regrouping changes sidebar indices.
        store.selectImportedGroup(groupIndex(store, "Simplex zone"));
        check(store.selectedImportedGroup().value("name").toString() == "Simplex zone"
              && store.importedRepeaterNames().contains("Corrected"), "Deferred classification lost navigation destination");
        store.selectImportedGroup(groupIndex(store, "Corrected"));
        check(store.selectedImportedGroup().value("isRepeater").toBool() && numbers(store) == QVariantList({7, 1, 6}),
              "Reclassification replaced zone or order");

        check(store.createCustomZone("Analog test")
              && store.createChannelInSelectedZone("New analog", "446.000", "446.000", false, 0, 0, 0, true),
              "Cannot create analog channel in digital-only import");
        check(store.exportCodeplugToFile(temp.filePath("analog.rdbx")), "Cannot export new mode missing from source");
        ImportedCodeplug analogParsed;
        check(CodeplugReader::read(readFile(temp.filePath("analog.rdbx")), &analogParsed, &error)
              && !analogParsed.channels.last().digital, "Default analog record did not reimport");

        RepeaterStore fullEdit;
        check(fullEdit.importRadioCodeplug(source) && fullEdit.createCustomZone("Full edit")
              && fullEdit.addExistingChannelToSelectedZone(3), "Cannot prepare full edit fixture");
        check(fullEdit.saveProjectToFile(temp.filePath("full-baseline.waffle")), "Cannot save full edit baseline");
        check(fullEdit.updateSelectedZoneChannel(1, "TS1 Alpha 2", "444.350", "449.350", true, 2, 1, 1, 1)
              && !fullEdit.modified() && numbers(fullEdit) == QVariantList{3}, "No-op full edit copied or dirtied channel");
        check(!fullEdit.updateSelectedZoneChannel(1, "TS1 Alpha 2", "444.350", "449.350", true, 2, 1, 0, 1)
              && !fullEdit.modified(), "Digital edit accepted a missing contact");
        check(!fullEdit.updateSelectedZoneChannel(1, "TS1 Alpha 2", "444.350", "449.350", true, 2, 1, 3, 1)
              && !fullEdit.modified(), "Digital edit accepted an invalid contact");
        check(!fullEdit.updateSelectedZoneChannel(1, "TS1 Bravo", "444.350", "449.350", true, 2, 1, 1, 1)
              && !fullEdit.modified(), "Duplicate channel name accepted");
        check(!fullEdit.updateSelectedZoneChannel(1, "Renamed", "444.350", "449.350", true, 16, 1, 1, 1)
              && !fullEdit.modified(), "Invalid full edit changed project");
        check(fullEdit.updateSelectedZoneChannel(1, "Renamed", "444.500", "449.500", true, 7, 2, 2, 0), "Cannot edit all settings");
        check(numbers(fullEdit) == QVariantList{7} && fullEdit.importedZoneNames().contains("Full edit"),
              "Full edit changed order or moved zone");
        const auto editedChannel = members(fullEdit).first().toMap();
        check(editedChannel.value("name").toString() == "Renamed" && editedChannel.value("colorCode").toInt() == 7
              && editedChannel.value("timeSlot").toInt() == 2 && editedChannel.value("contactIndex").toInt() == 2
              && editedChannel.value("power").toString() == "Low", "Full edit lost a setting");
        const auto fullPath = temp.filePath("full.rdbx");
        check(fullEdit.exportCodeplugToFile(fullPath), "Cannot export full edit");
        const auto fullBytes = decoded(readFile(fullPath));
        for (int i = 0; i < 6; ++i)
            check(before.mid(0x23768 + 48 * i, 48) == fullBytes.mid(0x23768 + 48 * i, 48), "Full edit changed other channels");
        check(before[0x23768 + 48 * 2 + 13] == fullBytes[0x23768 + 48 * 6 + 13], "Renaming lost unsupported settings");
        check(fullEdit.updateSelectedZoneChannel(1, "Analog edit", "446.000", "446.000", false, 0, 0, 0, 1), "Cannot switch to analog");
        check(fullEdit.saveProjectToFile(temp.filePath("mode.waffle")), "Cannot save mode change");
        RepeaterStore modeReopened;
        check(modeReopened.openProjectFromFile(temp.filePath("mode.waffle")), "Cannot reopen mode change");
        modeReopened.selectImportedGroup(groupIndex(modeReopened, "Full edit"));
        check(modeReopened.exportCodeplugToFile(temp.filePath("mode.rdbx")), "Cannot export reopened mode change");
        ImportedCodeplug modeParsed;
        check(CodeplugReader::read(readFile(temp.filePath("mode.rdbx")), &modeParsed, &error)
              && !modeParsed.channels.last().digital && modeParsed.channels.last().contactIndex == 0,
              "Mode change was not preserved");
        check(modeReopened.updateSelectedZoneChannel(1, "Digital edit", "444.350", "449.350", true, 3, 2, 2, 0)
              && modeReopened.exportCodeplugToFile(temp.filePath("digital-mode.rdbx")), "Cannot switch back to digital");
        check(CodeplugReader::read(readFile(temp.filePath("digital-mode.rdbx")), &modeParsed, &error)
              && modeParsed.channels.last().digital && modeParsed.channels.last().colorCode == 3
              && modeParsed.channels.last().timeSlot == 2 && modeParsed.channels.last().contactIndex == 2,
              "Digital mode settings did not reimport");

        RepeaterStore advanced;
        check(advanced.importRadioCodeplug(source) && advanced.saveProjectToFile(temp.filePath("advanced-base.waffle")),
              "Cannot prepare advanced fixture");
        const auto originalAdvancedOrder = numbers(advanced);
        check(advanced.editSelectedRepeaterAsZone() && advanced.editingAsZone() && !advanced.modified()
              && numbers(advanced) == originalAdvancedOrder, "Opening advanced view changed the zone");
        check(advanced.zoneContactChoices().size() == 2
              && advanced.zoneContactChoices()[1].toMap().value("number").toInt() == 2,
              "Contact snapshot lost table indices");
        advanced.selectImportedGroup(advanced.selectedImportedIndex());
        check(!advanced.editingAsZone() && !advanced.modified(), "Selecting repeater again did not exit advanced view");
        check(advanced.editSelectedRepeaterAsZone()
              && advanced.updateSelectedZoneChannelFrequencies(1, "444.400", "449.400"), "Cannot edit repeater as zone");
        check(advanced.editingAsZone() && advanced.importedRepeaterNames().contains("Ordering test"),
              "Advanced editing reclassified immediately");
        check(advanced.saveProjectToFile(temp.filePath("advanced.waffle")) && advanced.editingAsZone(),
              "Saving exited advanced view");
        RepeaterStore advancedReopened;
        check(advancedReopened.openProjectFromFile(temp.filePath("advanced.waffle"))
              && !advancedReopened.editingAsZone() && advancedReopened.importedZoneNames().contains("Ordering test"),
              "Advanced view state persisted or corrected classification was lost");
        advanced.selectImportedGroup(groupIndex(advanced, "Shared channels"));
        check(!advanced.editingAsZone() && advanced.importedZoneNames().contains("Ordering test")
              && advanced.selectedImportedGroup().value("name").toString() == "Shared channels"
              && numbers(advanced) == QVariantList({3, 4}), "Advanced navigation changed other zone or destination");
        advanced.newProject();
        check(!advanced.editingAsZone() && !advanced.editSelectedRepeaterAsZone(), "Empty project entered advanced view");
        check(advanced.addRepeater("First", "443.000", "448.000", 1)
              && advanced.addTalkgroup(1, "Alpha", 9, "First Alpha")
              && advanced.addRepeater("Second", "444.350", "449.350", 2)
              && advanced.addTalkgroup(1, "Bravo", 3100, "Second Bravo"), "Cannot prepare manual advanced view");
        check(advanced.editSelectedRepeaterAsZone() && advanced.editingAsZone()
              && advanced.selectedImportedGroup().value("name").toString() == "Second"
              && advanced.importedRepeaterNames() == QStringList({"First", "Second"})
              && members(advanced).first().toMap().value("name").toString() == "Second Bravo",
              "Manual advanced view lost selection or created another zone");
        advanced.leaveZoneEditor();
        check(!advanced.editingAsZone() && advanced.importedRepeaterNames().size() == 2,
              "Manual advanced view failed to exit");

        RepeaterStore fresh;
        fresh.newProject();
        check(fresh.updateRadioSettings("TEST RADIO", "3109101"), "Cannot set identity");
        check(fresh.createCustomZone("Empty"), "Cannot create zone in empty project");
        check(fresh.saveProjectToFile(temp.filePath("empty.waffle")), "Cannot save empty zone");
        RepeaterStore empty;
        check(empty.openProjectFromFile(temp.filePath("empty.waffle")) && empty.importedZoneNames().contains("Empty"),
              "Empty custom zone did not reopen");
        check(fresh.createChannelInSelectedZone("Simplex", "446.000", "446.000", false, 0, 0, 0, false), "Cannot create analog channel");
        check(!fresh.createChannelInSelectedZone("Digital", "444.350", "449.350", true, 2, 1, 0, true),
              "Digital creation accepted a missing contact");
        check(fresh.addContact("Local", 9, true), "Cannot add required digital contact");
        check(!fresh.createChannelInSelectedZone("Digital", "444.350", "449.350", true, 2, 1, 2, true),
              "Digital creation accepted an invalid contact");
        check(fresh.createChannelInSelectedZone("Digital", "444.350", "449.350", true, 2, 1, 1, true), "Cannot create digital channel");
        check(fresh.removeSelectedZoneChannel(1) && fresh.addExistingChannelToSelectedZone(1), "Cannot edit membership");
        check(fresh.exportCodeplugToFile(temp.filePath("fresh.rdbx")), "Cannot export fresh custom zone");
        for (int i = 2; i < 16; ++i)
            check(fresh.createChannelInSelectedZone(QString("Channel %1").arg(i), "446.000", "446.000", true, 1, 1, 1, true), "Cannot fill zone");
        check(!fresh.createChannelInSelectedZone("Overflow", "446.000", "446.000", true, 1, 1, 1, true), "Zone capacity exceeded");
        fresh.leaveZoneEditor();
        check(fresh.addRepeater("New repeater", "444.350", "449.350", 2, false), "Cannot add repeater after custom zone");
        check(fresh.saveProjectToFile(temp.filePath("draft.waffle")), "Cannot save empty repeater");
        RepeaterStore draft;
        check(draft.openProjectFromFile(temp.filePath("draft.waffle")), "Cannot reopen empty repeater");
        draft.selectImportedGroup(groupIndex(draft, "New repeater"));
        check(draft.selectedImportedGroup().value("isRepeater").toBool()
              && draft.selectedImportedGroup().value("power").toString() == "Low", "Empty repeater settings lost");
        check(draft.addImportedTalkgroup(1, "Local", 9, "Repeater Local"), "Cannot add first talkgroup to new repeater");
        check(members(draft)[0].toMap().value("power").toString() == "Low", "New repeater power not inherited");
        RepeaterStore existing;
        existing.newProject();
        check(existing.addRepeater("Existing", "444.350", "449.350", 2)
              && existing.addTalkgroup(1, "Local", 9, "Existing Local"), "Cannot create manual fixture");
        check(existing.createCustomZone("Custom") && existing.importedRepeaterNames().contains("Existing")
              && existing.availableZoneChannels().size() == 1, "Creating custom zone lost existing repeater");
        qInfo() << "Zone creation, deferred classification, shared channel isolation, export preservation and persistence passed";
        return 0;
    } catch (const std::exception &error) { qCritical() << error.what(); return 1; }
}
