#include "channel_order_fixture.h"
#include "../src/repeaterstore.h"
#include "../src/ziparchive.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QTemporaryDir>
#include <stdexcept>

namespace {
void check(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}
QVariantList zoneOrder(const RepeaterStore &store)
{
    QVariantList result;
    const auto zones = store.selectedImportedGroup().value(QStringLiteral("zones")).toList();
    for (const auto &channel : zones.constFirst().toMap().value(QStringLiteral("channels")).toList())
        result.append(channel.toMap().value(QStringLiteral("number")));
    return result;
}
QStringList slotNames(const RepeaterStore &store, int slot)
{
    QStringList names;
    const auto group = store.selectedImportedGroup();
    for (const auto &channel : group.value(slot == 1 ? QStringLiteral("slot1Channels")
                                                   : QStringLiteral("slot2Channels")).toList())
        names.append(channel.toMap().value(QStringLiteral("name")).toString());
    return names;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory;
        check(directory.isValid(), "Cannot create temporary directory");
        QString error;
        const auto source = channelOrderFixture(&error);
        check(!source.isEmpty(), qPrintable(error));
        RepeaterStore store;
        check(store.importRadioCodeplug(source), "Cannot import ordering fixture");
        store.selectImportedGroup(0);
        const QVariantList original{4, 1, 2, 5, 3, 6};
        check(zoneOrder(store) == original, "Initial zone order was lost");
        check(store.saveProjectToFile(directory.filePath("baseline.waffle")), "Cannot save baseline");
        check(!store.modified(), "Saved baseline is marked modified");
        const auto uneditedPath = directory.filePath("unedited.rdbx");
        check(store.exportCodeplugToFile(uneditedPath), "Cannot export unedited imported zone");
        QFile unedited(uneditedPath);
        check(unedited.open(QIODevice::ReadOnly) && unedited.readAll() == source,
              "Import/export changed the source zone order before any user reorder");
        check(store.reorderSelectedImportedZone(original) && !store.modified(), "No-op order dirtied project");
        for (const auto &bad : QList<QVariantList>{
                 {4, 1, 2, 5, 3}, {4, 1, 2, 5, 3, 3}, {4, 1, 2, 5, 3, 7},
                 {4, 1, 2, 5, 3, 0}, {4, 1, 2, 5, 3, 1.5}, {4, 1, 2, 5, 3, QStringLiteral("bad")}}) {
            check(!store.reorderSelectedImportedZone(bad), "Invalid order accepted");
            check(zoneOrder(store) == original && !store.modified(), "Invalid order changed project");
        }
        int selectionChanges = 0;
        QObject::connect(&store, &RepeaterStore::importedSelectionChanged, &app, [&] { ++selectionChanges; });
        const QVariantList reordered{3, 6, 5, 2, 4, 1};
        check(store.reorderSelectedImportedZone(reordered), "Valid reorder failed");
        check(zoneOrder(store) == reordered && store.modified() && selectionChanges == 1,
              "Reorder did not update the selected zone");
        check(slotNames(store, 1) == QStringList({"TS1 Alpha 2", "TS1 Bravo", "TS1 Alpha 1"}),
              "TS1 is not filtered from zone order");
        check(slotNames(store, 2) == QStringList({"TS2 Bravo", "TS2 Alpha"}),
              "TS2 is not filtered from zone order");
        store.selectImportedGroup(1);
        check(zoneOrder(store) == QVariantList({3, 4}), "Shared-channel zone was reordered");
        store.selectImportedGroup(2);
        check(!store.reorderSelectedImportedZone({6}), "Non-repeater zone was reorderable");
        store.selectImportedGroup(0);
        const auto project = directory.filePath("ordered.waffle");
        check(store.saveProjectToFile(project), "Cannot save ordered project");
        QMap<QString, QByteArray> entries;
        check(ZipArchive::read(project, &entries, &error), qPrintable(error));
        check(entries.value(QStringLiteral("source/original-codeplug.bin")) == source,
              "Reordering changed the original source attachment");
        RepeaterStore reopened;
        check(reopened.openProjectFromFile(project), "Cannot reopen ordered project");
        reopened.selectImportedGroup(0);
        check(zoneOrder(reopened) == reordered, "Saved order did not round-trip");
        const auto exportPath = directory.filePath("ordered.rdbx");
        check(reopened.exportCodeplugToFile(exportPath), "Cannot export ordered project");
        QFile output(exportPath);
        check(output.open(QIODevice::ReadOnly), "Cannot open exported codeplug");
        const auto exported = output.readAll();
        ImportedCodeplug parsed;
        check(CodeplugReader::read(exported, &parsed, &error), qPrintable(error));
        check(parsed.zones[0].channelIndexes == QVector<int>({2, 5, 4, 1, 3, 0}), "Export lost zone order");
        check(parsed.zones[1].channelIndexes == QVector<int>({2, 3}), "Export changed another zone");
        check(exported.size() == source.size(), "Ordering changed codeplug length");
        for (qsizetype i = 0; i < source.size(); ++i) {
            if (i >= 0x52954 + 36 && i < 0x52954 + 36 + 12) continue;
            check(exported[i] == source[i], "Ordering changed unrelated codeplug bytes");
        }
        RepeaterStore manual;
        manual.newProject();
        check(manual.updateRadioSettings("TEST RADIO", "3109101"), "Cannot configure manual radio identity");
        check(manual.addRepeater("Manual", "444.350", "449.350", 2), "Cannot add manual repeater");
        check(manual.addTalkgroup(2, "Alpha", 9, "TS2 Alpha"), "Cannot add TS2 Alpha");
        check(manual.addTalkgroup(2, "Bravo", 3100, "TS2 Bravo"), "Cannot add TS2 Bravo");
        check(manual.addTalkgroup(1, "Alpha", 9, "TS1 Alpha"), "Cannot add TS1 Alpha");
        check(manual.addTalkgroup(1, "Bravo", 3100, "TS1 Bravo"), "Cannot add TS1 Bravo");
        check(manual.addSimplexToSelectedRepeater("Simplex", "446.000", true, 1, 1), "Cannot add simplex");
        auto manualNames = [](const RepeaterStore &value) {
            QStringList result;
            for (const auto &entry : value.selectedRepeaterChannels())
                result.append(entry.toMap().value(QStringLiteral("name")).toString());
            return result;
        };
        const QStringList defaultNames{"TS2 Alpha", "TS2 Bravo", "TS1 Alpha", "TS1 Bravo", "Simplex"};
        check(manualNames(manual) == defaultNames, "Manual default order changed");
        const auto manualPath = directory.filePath("manual.waffle");
        check(manual.saveProjectToFile(manualPath), "Cannot save manual baseline");
        check(manual.reorderSelectedRepeaterChannels({1, 2, 3, 4, 5}) && !manual.modified(), "Manual no-op dirtied project");
        for (const auto &bad : QList<QVariantList>{{1, 2}, {1, 2, 3, 4, 4}, {1, 2, 3, 4, 6}, {1, 2, 3, 4, 1.5}})
            check(!manual.reorderSelectedRepeaterChannels(bad) && !manual.modified(), "Invalid manual order accepted");
        check(manual.reorderSelectedRepeaterChannels({4, 5, 2, 3, 1}), "Manual reorder failed");
        const QStringList manualExpected{"TS1 Bravo", "Simplex", "TS2 Bravo", "TS1 Alpha", "TS2 Alpha"};
        check(manualNames(manual) == manualExpected, "Manual order not applied");
        check(manual.slot1Talkgroups().first().toMap().value("channelName").toString() == "TS1 Bravo"
              && manual.slot2Talkgroups().first().toMap().value("channelName").toString() == "TS2 Bravo",
              "Manual slot boxes do not follow combined order");
        check(manual.saveProjectToFile(manualPath), "Cannot save manual order");
        RepeaterStore manualReopened;
        check(manualReopened.openProjectFromFile(manualPath) && manualNames(manualReopened) == manualExpected,
              "Manual order did not survive reopen");
        const auto manualExport = directory.filePath("manual.rdbx");
        check(manualReopened.exportCodeplugToFile(manualExport), "Cannot export manual order");
        QFile manualFile(manualExport);
        check(manualFile.open(QIODevice::ReadOnly), "Cannot open manual export");
        ImportedCodeplug manualDecoded;
        check(CodeplugReader::read(manualFile.readAll(), &manualDecoded, &error), qPrintable(error));
        QStringList exportedNames;
        for (int index : manualDecoded.zones.first().channelIndexes)
            exportedNames.append(manualDecoded.channels[index].name);
        check(exportedNames == manualExpected, "Export lost manual order");
        check(manualReopened.addTalkgroup(1, "Charlie", 123, "New channel"), "Cannot add channel after reorder");
        check(manualNames(manualReopened).last() == "New channel", "New channel did not append");
        check(manualReopened.addTalkgroup(2, "Delta", 124, "New TS2"), "Cannot append TS2 after TS1");
        check(manualNames(manualReopened).last() == "New TS2", "TS2 insertion disturbed append order");
        check(manualReopened.addSimplexToSelectedRepeater("New simplex", "445.000", false, 0, 0), "Cannot add simplex after reorder");
        manualReopened.removeTalkgroup(2, 3100);
        check(manualReopened.removeSimplexFromSelectedRepeater(0), "Cannot remove ordered simplex");
        check(manualNames(manualReopened) == QStringList({"TS1 Bravo", "TS1 Alpha", "TS2 Alpha", "New channel", "New TS2", "New simplex"}),
              "Removal disturbed remaining channel order");
        check(manualReopened.saveProjectToFile(manualPath), "Cannot save edited manual order");
        RepeaterStore edited;
        check(edited.openProjectFromFile(manualPath) && manualNames(edited) == manualNames(manualReopened),
              "Edited manual order did not survive reopen");
        qInfo() << "Channel ordering, shared zones, source preservation, save/reopen and export passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
}
