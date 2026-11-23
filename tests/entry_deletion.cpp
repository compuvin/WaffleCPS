#include "../src/codeplugdefinition.h"
#include "../src/repeaterstore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QDebug>
#include <stdexcept>

static void check(bool ok, const QString &message)
{
    if (!ok) throw std::runtime_error(message.toStdString());
}
static QByteArray readFile(const QString &path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "Read exported fixture");
    return file.readAll();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QDir::setCurrent(WAFFLE_SOURCE_DIR);
    try {
        RepeaterStore blank;
        blank.newProject();
        check(blank.addRepeater("Local", "444.350", "449.350", 1), "Manual repeater");
        check(blank.addTalkgroup(1, "Local", 9, "Local TS1"), "Manual talkgroup");
        auto info = blank.selectedEntryDeletionInfo();
        check(blank.deleteSelectedEntry(false, info.value("revision").toString()), "Manual deletion");
        check(blank.repeaterNames().isEmpty() && blank.contacts().size() == 1, "Manual contacts lost");

        QTemporaryDir temporary;
        check(temporary.isValid(), "Temporary directory");
        for (const QString definition : {QString("dm32uv.wcp"), QString("md-uv390.wcp"), QString("cs7000-m17-plus.wcp")}) {
            QString error;
            CodeplugDefinition codec;
            check(CodeplugDefinition::load("definitions/" + definition, &codec, &error), error);
            const auto base = codec.defaultCodeplug();
            ImportedCodeplug model;
            check(codec.read(base, &model, &error), error);
            model.radioName = "TEST RADIO";
            model.radioId = 3109101;
            model.channels.clear();
            model.zones.clear();
            model.scanLists.clear();
            for (int i = 0; i < 5; ++i) {
                ImportedChannel channel;
                channel.name = "Channel " + QString::number(i);
                channel.frequencyAHz = i == 2 ? 449500000 : 449350000;
                channel.frequencyBHz = i == 2 ? 444500000 : 444350000;
                channel.digital = true;
                channel.colorCode = 1;
                channel.timeSlot = 1;
                channel.useDefaultTemplate = true;
                model.channels.append(channel);
            }
            ImportedZone a; a.name = "Delete me"; a.channelIndexes = {0, 1, 3};
            ImportedZone b; b.name = "Keep mixed"; b.channelIndexes = {0, 2};
            ImportedZone c; c.name = "Keep other"; c.channelIndexes = {4};
            if (model.separateDisplayLists) { b.channelIndexes = {2}; b.bottomChannelIndexes = {0}; }
            model.zones = {a, b, c};
            if (model.scanListsSupported) {
                ImportedScanList list; list.name = "Scan test"; list.channelIndexes = {3};
                if (model.scanCurrentChannelSupported) list.channelIndexes.prepend(-1);
                model.scanLists.append(list);
            }
            QByteArray bytes;
            check(codec.write(base, model, model.radioName, model.radioId, &bytes, &error), error);
            const QString input = temporary.filePath(definition + ".input");
            QFile file(input);
            check(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Save fixture");
            file.close();
            for (int mode = 0; mode < 3; ++mode) {
                const bool cleanup = mode != 0;
                RepeaterStore store;
                check(store.importCodeplugFromFile(input, definition), "Import fixture");
                store.selectRepeater(0);
                if (mode != 1) check(store.editSelectedRepeaterAsZone(), "Edit repeater as zone");
                info = store.selectedEntryDeletionInfo();
                check(info.value("canDelete").toBool(), info.value("reason").toString());
                check(!store.deleteSelectedEntry(cleanup, "stale"), "Accepted stale confirmation");
                check(store.deleteSelectedEntry(mode == 2, info.value("revision").toString()), "Delete selected entry");
                const QString output = temporary.filePath(definition + (cleanup ? ".cleanup" : ".keep"));
                check(store.exportCodeplugToFile(output), "Export deletion");
                ImportedCodeplug result;
                check(codec.read(readFile(output), &result, &error), error);
                check(result.zones.size() == 2, "Wrong remaining zone count");
                const int expectedChannels = !cleanup ? 5 : model.scanListsSupported ? 4 : 3;
                check(result.channels.size() == expectedChannels, "Wrong remaining channel count");
                check(result.contacts.size() == model.contacts.size(), "Contacts changed");
                if (cleanup && model.scanListsSupported) {
                    QVector<int> expected{2};
                    if (model.scanCurrentChannelSupported) expected.prepend(-1);
                    check(result.scanLists[0].channelIndexes == expected, "Scan references not remapped");
                }
                if (cleanup && model.separateDisplayLists)
                    check(result.zones[0].bottomChannelIndexes == QVector<int>({0}), "Bottom-only shared channel lost");
                const QString project = temporary.filePath(definition + ".waffle");
                check(store.saveProjectToFile(project), "Save deleted project");
                RepeaterStore reopened;
                check(reopened.openProjectFromFile(project), "Reopen deleted project");
                check(reopened.exportCodeplugToFile(output), "Export reopened project");
                check(codec.read(readFile(output), &result, &error), error);
                check(result.zones.size() == 2 && result.channels.size() == expectedChannels, "Deletion lost on reopen");
            }
        }
        qInfo() << "Entry deletion checks passed";
        return 0;
    } catch (const std::exception &error) { qCritical() << error.what(); return 1; }
}
