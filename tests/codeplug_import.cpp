#include "../src/codeplugreader.h"
#include "../src/repeaterstore.h"
#include "../src/ziparchive.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

namespace {
void put32(QByteArray &bytes, int offset, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

void put16(QByteArray &bytes, int offset, quint16 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

void putName(QByteArray &bytes, int offset, const QString &name)
{
    for (int i = 0; i < name.size(); ++i)
        put16(bytes, offset + 2 * i, name.at(i).unicode());
}

void putContact(QByteArray &bytes, int index, const QString &name, int id)
{
    const int record = 0x47d3c0 + 16 * index;
    bytes[record + 4] = 1; // Group call.
    bytes[record + 5] = char(id & 0xff);
    bytes[record + 6] = char((id >> 8) & 0xff);
    bytes[record + 7] = char((id >> 16) & 0xff);
    for (int character = 0; character < 16; ++character) {
        const QChar letter = character < name.size() ? name.at(character) : QChar(' ');
        int code = 62;
        if (letter >= QChar('0') && letter <= QChar('9'))
            code = letter.unicode() - '0';
        else if (letter >= QChar('A') && letter <= QChar('Z'))
            code = letter.unicode() - 'A' + 10;
        else if (letter >= QChar('a') && letter <= QChar('z'))
            code = letter.unicode() - 'a' + 36;
        const int bit = character * 6;
        for (int part = 0; part < 6; ++part) {
            const int position = bit + part;
            bytes[record + 8 + position / 8] = char(uchar(bytes[record + 8 + position / 8])
                | (((code >> (5 - part)) & 1) << (7 - position % 8)));
        }
    }
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc >= 4 && QByteArray(argv[1]) == "--default") {
        QFile source(QString::fromLocal8Bit(argv[2]));
        ImportedCodeplug parsed;
        QString error;
        const int expectedId = QByteArray(argv[3]).toInt();
        if (!source.open(QIODevice::ReadOnly)
            || !CodeplugReader::read(source.readAll(), &parsed, &error)
            || parsed.cpsVersion != QStringLiteral("V1.02.21.00")
            || parsed.channels.size() != 2 || parsed.zones.size() != 1
            || parsed.contacts.size() != 1 || parsed.radioId != expectedId) {
            qCritical() << "Default CPS V1.02.21.00 import failed:" << error;
            return 31;
        }
        RepeaterStore store;
        if (!store.importCodeplugFromFile(source.fileName())
            || store.radioId() != expectedId
            || !store.saveProjectToFile(QStringLiteral("default-import.waffle")))
            return 32;
        RepeaterStore loaded;
        if (!loaded.openProjectFromFile(QStringLiteral("default-import.waffle"))
            || loaded.radioId() != expectedId || loaded.radioName() != parsed.radioName
            || loaded.importedCodeplugSummary().isEmpty())
            return 33;
        qInfo() << "Default CPS V1.02.21.00 import verified:" << parsed.channels.size()
                << "channels," << parsed.zones.size() << "zone," << parsed.contacts.size() << "contact";
        return 0;
    }
    QByteArray bytes(0x47d500, '\0');
    bytes.replace(0x1200, 16, QByteArray("CS7000-M17 PLUS\0", 16));
    bytes.replace(0x1298, 12, QByteArray("V1.02.20.00\0", 12));
    const QStringList names{QStringLiteral("Local"), QStringLiteral("Wide"),
                            QStringLiteral("Simplex"), QStringLiteral("Other repeater")};
    for (int i = 0; i < names.size(); ++i) {
        putName(bytes, 0x4368 + 32 * i, names.at(i));
        const int record = 0x23768 + 48 * i;
        put32(bytes, record, i == 2 ? 446000000 : (i == 3 ? 447200000 : 449350000));
        put32(bytes, record + 4, i == 2 ? 446000000 : (i == 3 ? 442200000 : 444350000));
        bytes[record + 16] = i == 1 ? 0x12 : 0x02;
        bytes[record + 9] = i == 1 ? char(0x00) : char(0x80);
        bytes[record + 23] = 0x20;
        bytes[record + 24] = i == 1 ? 2 : 1;
    }
    putContact(bytes, 1, QStringLiteral("Local"), 9);
    putContact(bytes, 2, QStringLiteral("Statewide"), 3100);
    put32(bytes, 0x52568, 4);
    int cursor = 0x52954;
    putName(bytes, cursor, QStringLiteral("Repeater zone"));
    put32(bytes, cursor + 32, 2);
    put16(bytes, cursor + 36, 1);
    put16(bytes, cursor + 38, 2);
    cursor += 40;
    putName(bytes, cursor, QStringLiteral("Mixed zone"));
    put32(bytes, cursor + 32, 2);
    put16(bytes, cursor + 36, 1);
    put16(bytes, cursor + 38, 3);
    cursor += 40;
    putName(bytes, cursor, QStringLiteral("Simplex only"));
    put32(bytes, cursor + 32, 1);
    put16(bytes, cursor + 36, 3);
    cursor += 38;
    putName(bytes, cursor, QStringLiteral("Two repeaters"));
    put32(bytes, cursor + 32, 2);
    put16(bytes, cursor + 36, 1);
    put16(bytes, cursor + 38, 4);

    ImportedCodeplug parsed;
    QString error;
    if (!CodeplugReader::read(bytes, &parsed, &error) || parsed.channels.size() != 4
        || parsed.zones.size() != 4 || parsed.zones.at(0).channelIndexes.at(1) != 1
        || parsed.channels.at(1).timeSlot != 2 || parsed.channels.at(1).contactIndex != 2
        || parsed.contacts.size() != 2 || parsed.contacts.at(0).name != QStringLiteral("Local")
        || !parsed.channels.at(0).highPower.value_or(false)
        || parsed.channels.at(1).highPower.value_or(true)
        || parsed.contacts.at(1).id != 3100) {
        qCritical() << "Synthetic codeplug import failed:" << error;
        return 1;
    }
    QByteArray unsupported = bytes;
    unsupported[0x1299] = '2';
    if (CodeplugReader::read(unsupported, &parsed, &error)) {
        qCritical() << "Unsupported CPS version was accepted";
        return 7;
    }
    RepeaterStore talkgroupStore;
    talkgroupStore.importCodeplugFromBytes(bytes);
    talkgroupStore.selectImportedGroup(1);
    if (!talkgroupStore.addImportedTalkgroup(2, QStringLiteral("Regional"), 1234,
                                             QStringLiteral("Mixed Regional"))
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("zoneChannelCount")).toInt() != 3
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().size() != 1
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("channels")).toList().at(0).toMap().value(QStringLiteral("name")).toString()
            != QStringLiteral("Mixed Regional")
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("channels")).toList().at(0).toMap().value(QStringLiteral("timeSlot")).toInt() != 2
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("channels")).toList().at(0).toMap().value(QStringLiteral("colorCode")).toInt() != 2
        || talkgroupStore.selectedImportedGroup().value(QStringLiteral("receiveFrequency")).toString()
            != QStringLiteral("444.3500")
        || !talkgroupStore.saveProjectToFile(QStringLiteral("talkgroup-edit.waffle"))) {
        qCritical() << "Adding an imported talkgroup failed";
        return 14;
    }
    RepeaterStore talkgroupReloaded;
    if (!talkgroupReloaded.openProjectFromFile(QStringLiteral("talkgroup-edit.waffle"))) {
        qCritical() << "Imported talkgroup did not save";
        return 16;
    }
    talkgroupReloaded.selectImportedGroup(1);
    if (talkgroupReloaded.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().value(0).toMap()
            .value(QStringLiteral("id")).toInt() != 1234
        || talkgroupReloaded.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().value(0).toMap()
            .value(QStringLiteral("channels")).toList().at(0).toMap().value(QStringLiteral("name")).toString()
            != QStringLiteral("Mixed Regional")
        || !talkgroupReloaded.removeImportedTalkgroup(2, 3)
        || talkgroupReloaded.selectedImportedGroup().value(QStringLiteral("zoneChannelCount")).toInt() != 2
        || !talkgroupReloaded.addImportedTalkgroup(2, QStringLiteral("Regional"), 1234)) {
        qCritical() << "Imported talkgroup remove or re-add failed";
        return 17;
    }
    RepeaterStore sharedChannel;
    sharedChannel.importCodeplugFromBytes(bytes);
    sharedChannel.selectImportedGroup(1);
    if (!sharedChannel.removeImportedTalkgroup(1, 1)
        || sharedChannel.selectedImportedGroup().value(QStringLiteral("isRepeater")).toBool()
        || sharedChannel.importedRepeaterNames().size() != 1) {
        qCritical() << "Shared talkgroup removal changed classification incorrectly";
        return 20;
    }
    sharedChannel.selectImportedGroup(0);
    if (sharedChannel.selectedImportedGroup().value(QStringLiteral("slot1Talkgroups")).toList().value(0).toMap()
            .value(QStringLiteral("id")).toInt() != 9) {
        qCritical() << "Shared talkgroup removal changed another zone";
        return 21;
    }
    RepeaterStore store;
    if (!store.importCodeplugFromBytes(bytes) || store.importedRepeaterNames().size() != 2
        || store.contacts().size() != 2
        || store.contacts().at(0).toMap().value(QStringLiteral("name")).toString() != QStringLiteral("Local")
        || store.importedZoneNames().size() != 2
        || store.selectedImportedGroup().value(QStringLiteral("zones")).toList().size() != 1
        || store.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("Mixed")
        || store.selectedImportedGroup().value(QStringLiteral("name")).toString() != QStringLiteral("Repeater zone")) {
        qCritical() << "Separate repeater zones were not retained";
        return 2;
    }
    RepeaterStore powerStore;
    if (!powerStore.importCodeplugFromBytes(bytes)
        || !powerStore.updateSelectedImportedRepeater(QStringLiteral("Power zone"),
             QStringLiteral("444.3500"), QStringLiteral("449.3500"), 2, -1)
        || powerStore.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("Mixed")
        || !powerStore.addImportedTalkgroup(1, QStringLiteral("New group"), 777,
                                            QStringLiteral("Power Added"))
        || powerStore.selectedImportedGroup().value(QStringLiteral("slot1Channels")).toList().last().toMap()
            .value(QStringLiteral("power")).toString() != QStringLiteral("High")
        || !powerStore.updateSelectedImportedRepeater(QStringLiteral("Power zone"),
             QStringLiteral("444.3500"), QStringLiteral("449.3500"), 2, 0)
        || powerStore.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("Low")) {
        qCritical() << "Imported mixed power was not preserved or normalized";
        return 25;
    }
    powerStore.selectImportedGroup(1);
    if (powerStore.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("High")
        || !powerStore.updateSelectedImportedRepeater(QStringLiteral("Mixed zone"),
             QStringLiteral("444.3500"), QStringLiteral("449.3500"), 2, 0)
        || powerStore.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("Low")
        || powerStore.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().at(0).toMap()
            .value(QStringLiteral("power")).toString() != QStringLiteral("High")
        || !powerStore.saveProjectToFile(QStringLiteral("power-edit.waffle"))) {
        qCritical() << "Power edit changed another zone or its simplex channel";
        return 26;
    }
    RepeaterStore powerReloaded;
    if (!powerReloaded.openProjectFromFile(QStringLiteral("power-edit.waffle"))
        || powerReloaded.selectedImportedGroup().value(QStringLiteral("power")).toString() != QStringLiteral("Low")) {
        qCritical() << "Repeater power did not survive save and open";
        return 27;
    }
    RepeaterStore contactStore;
    if (!contactStore.importCodeplugFromBytes(bytes)
        || contactStore.addContact(QStringLiteral("Another person"), 9, false)
        || contactStore.updateContact(1, QStringLiteral("LOCAL"), 9)
        || !contactStore.addContact(QStringLiteral("New person"), 123456, false)
        || !contactStore.updateContact(0, QStringLiteral("Local edit"), 10)
        || contactStore.addImportedTalkgroup(1, QStringLiteral("New person"), 123456,
                                              QStringLiteral("Invalid private"))
        || contactStore.selectedImportedGroup().value(QStringLiteral("slot1Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("id")).toInt() != 10
        || !contactStore.saveProjectToFile(QStringLiteral("contact-edit.waffle"))) {
        qCritical() << "Imported contact editing failed";
        return 22;
    }
    RepeaterStore contactReloaded;
    if (!contactReloaded.openProjectFromFile(QStringLiteral("contact-edit.waffle"))
        || contactReloaded.contacts().size() != 3
        || contactReloaded.contacts().at(2).toMap().value(QStringLiteral("name")).toString() != QStringLiteral("New person")
        || contactReloaded.selectedImportedGroup().value(QStringLiteral("slot1Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("id")).toInt() != 10) {
        qCritical() << "Imported contact edits did not survive save and open";
        return 23;
    }
    contactStore.selectImportedGroup(1);
    if (!contactStore.addImportedTalkgroup(2, QStringLiteral("Statewide"), 3100,
                                            QStringLiteral("Mixed Statewide"))
        || contactStore.contacts().size() != 3
        || contactStore.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("channels")).toList().at(0).toMap().value(QStringLiteral("name")).toString()
            != QStringLiteral("Mixed Statewide")) {
        qCritical() << "Existing talkgroup could not be reused with a separate channel name";
        return 24;
    }
    const QVariantList firstSlot = store.selectedImportedGroup().value(QStringLiteral("slot1Talkgroups")).toList();
    const QVariantList secondSlot = store.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList();
    if (firstSlot.size() != 1 || firstSlot.at(0).toMap().value(QStringLiteral("id")).toInt() != 9
        || secondSlot.size() != 1 || secondSlot.at(0).toMap().value(QStringLiteral("id")).toInt() != 3100
        || !store.updateSelectedImportedRepeater(QStringLiteral("Renamed zone"), QStringLiteral("445.000"),
                                                 QStringLiteral("450.000"), 4)
        || store.selectedImportedGroup().value(QStringLiteral("name")).toString() != QStringLiteral("Renamed zone")
        || store.selectedImportedGroup().value(QStringLiteral("colorCode")).toInt() != 4) {
        qCritical() << "Imported contacts or repeater edit failed";
        return 11;
    }
    store.selectImportedGroup(1);
    const QVariantMap firstMixedChannel = store.selectedImportedGroup()
        .value(QStringLiteral("slot1Channels")).toList().value(0).toMap();
    if (store.selectedImportedGroup().value(QStringLiteral("name")).toString() != QStringLiteral("Mixed zone")
        || store.selectedImportedGroup().value(QStringLiteral("zones")).toList().size() != 1
        || store.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 1
        || store.selectedImportedGroup().value(QStringLiteral("slot1Channels")).toList().size() != 1
        || store.selectedImportedGroup().value(QStringLiteral("receiveFrequency")).toString() != QStringLiteral("444.3500")
        || firstMixedChannel.value(QStringLiteral("zonePosition")).toInt() != 1
        || !store.addSimplexToSelectedImportedRepeater(0, QStringLiteral("Extra"),
                                                       QStringLiteral("146.520"), false, 0, 0)
        || store.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 2
        || !store.saveProjectToFile(QStringLiteral("imported.waffle"))) {
        qCritical() << "Classification or project save failed";
        return 2;
    }
    RepeaterStore reopened;
    if (!reopened.openProjectFromFile(QStringLiteral("imported.waffle"))
        || !reopened.hasImportedCodeplug() || reopened.importedRepeaterNames().size() != 2
        || reopened.importedZoneNames().size() != 2) {
        qCritical() << "Imported project did not round-trip";
        return 3;
    }
    reopened.selectImportedGroup(0);
    if (reopened.selectedImportedGroup().value(QStringLiteral("name")).toString() != QStringLiteral("Renamed zone")
        || reopened.selectedImportedGroup().value(QStringLiteral("slot2Talkgroups")).toList().at(0).toMap()
            .value(QStringLiteral("id")).toInt() != 3100) {
        qCritical() << "Imported edit or contacts did not round-trip";
        return 12;
    }
    reopened.selectImportedGroup(1);
    if (reopened.selectedImportedGroup().value(QStringLiteral("name")).toString() != QStringLiteral("Mixed zone")
        || reopened.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 2) {
        qCritical() << "Imported project did not round-trip";
        return 3;
    }
    const QVariantMap extraSimplex = reopened.selectedImportedGroup()
        .value(QStringLiteral("simplexChannels")).toList().last().toMap();
    const int channelCount = reopened.selectedImportedGroup().value(QStringLiteral("zoneChannelCount")).toInt();
    if (extraSimplex.value(QStringLiteral("name")).toString() != QStringLiteral("Extra")
        || !reopened.removeSimplexFromSelectedImportedRepeater(
            extraSimplex.value(QStringLiteral("zoneInGroup")).toInt(),
            extraSimplex.value(QStringLiteral("zonePosition")).toInt())
        || reopened.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 1
        || reopened.selectedImportedGroup().value(QStringLiteral("zoneChannelCount")).toInt() != channelCount - 1
        || !reopened.selectedImportedGroup().value(QStringLiteral("isRepeater")).toBool()) {
        qCritical() << "Imported simplex frequency removal failed";
        return 3;
    }
    if (argc > 1) {
        const QString samplePath = QString::fromLocal8Bit(argv[1]);
        QFile sample(samplePath);
        if (!sample.open(QIODevice::ReadOnly)
            || !CodeplugReader::read(sample.readAll(), &parsed, &error)
            || parsed.channels.size() != 821 || parsed.zones.size() != 82
            || parsed.contacts.size() != 335
            || parsed.contacts.at(5).name != QStringLiteral("Local")
            || parsed.contacts.at(5).id != 9 || !parsed.contacts.at(5).groupCall
            || parsed.contacts.at(1).name != QStringLiteral("NY NJ Metro")
            || parsed.contacts.at(2).name != QStringLiteral("North America")
            || parsed.contacts.at(185).name != QStringLiteral("K1VSC   Ron")
            || parsed.contacts.at(38).groupCall) {
            qCritical() << "Private sample import failed:" << error;
            return 4;
        }
        RepeaterStore sampleStore;
        if (!sampleStore.importCodeplugFromFile(samplePath)
            || sampleStore.importedRepeaterNames().size() != 67
            || sampleStore.contacts().size() != 335
            || sampleStore.contacts().at(38).toMap().value(QStringLiteral("groupCall")).toBool()
            || sampleStore.contacts().at(185).toMap().value(QStringLiteral("name")).toString()
                != QStringLiteral("K1VSC Ron")
            || sampleStore.importedZoneNames().size() != 15
            || !sampleStore.saveProjectToFile(QStringLiteral("sample-import.waffle"))) {
            qCritical() << "Private sample classification or save failed";
            return 5;
        }
        RepeaterStore sampleReloaded;
        if (!sampleReloaded.openProjectFromFile(QStringLiteral("sample-import.waffle"))
            || sampleReloaded.importedRepeaterNames().size() != 67
            || sampleReloaded.importedZoneNames().size() != 15
            || sampleReloaded.radioName() != QStringLiteral("KB1JDX")
            || sampleReloaded.radioId() != 3109101
            || sampleReloaded.sourceCodeplugName() != QFileInfo(samplePath).fileName()) {
            qCritical() << "Private sample project did not round-trip";
            return 6;
        }
        if (!sampleReloaded.exportCodeplugToFile(QStringLiteral("sample-unchanged-export.rdbx"))) {
            qCritical() << "Unchanged private sample did not export";
            return 34;
        }
        QFile unchanged(QStringLiteral("sample-unchanged-export.rdbx"));
        sample.seek(0);
        if (!unchanged.open(QIODevice::ReadOnly) || unchanged.readAll() != sample.readAll()) {
            qCritical() << "Unchanged export differs from its imported source";
            return 35;
        }
        const int editBrooklyn = sampleStore.importedRepeaterNames().indexOf(QStringLiteral("CT-Brooklyn"));
        sampleStore.selectImportedGroup(editBrooklyn);
        if (editBrooklyn < 0
            || !sampleStore.addImportedTalkgroup(2, QStringLiteral("WaffleTest"), 7777777,
                                                  QStringLiteral("BKLN TEST TG"))
            || !sampleStore.exportCodeplugToFile(QStringLiteral("sample-edited-export.rdbx"))) {
            qCritical() << "Edited private sample did not export";
            return 36;
        }
        QFile edited(QStringLiteral("sample-edited-export.rdbx"));
        ImportedCodeplug editedParsed;
        if (!edited.open(QIODevice::ReadOnly)
            || !CodeplugReader::read(edited.readAll(), &editedParsed, &error)
            || editedParsed.channels.size() != 822 || editedParsed.contacts.size() != 336
            || editedParsed.zones.size() != 82
            || editedParsed.channels.last().name != QStringLiteral("BKLN TEST TG")
            || editedParsed.contacts.last().id != 7777777) {
            qCritical() << "Edited export did not reimport its new channel and contact:" << error;
            return 37;
        }
        QMap<QString, QByteArray> oldEntries;
        if (!ZipArchive::read(QStringLiteral("sample-import.waffle"), &oldEntries, &error)) {
            qCritical() << "Could not inspect sample project:" << error;
            return 22;
        }
        QJsonObject oldRoot = QJsonDocument::fromJson(oldEntries.value(QStringLiteral("project.json"))).object();
        QJsonObject oldImported = oldRoot.value(QStringLiteral("importedCodeplug")).toObject();
        QJsonArray oldContacts = oldImported.value(QStringLiteral("contacts")).toArray();
        QJsonObject shortened = oldContacts.at(1).toObject();
        shortened.insert(QStringLiteral("name"), QStringLiteral("NY NJ Metr"));
        oldContacts[1] = shortened;
        oldImported.insert(QStringLiteral("contacts"), oldContacts);
        QJsonArray oldChannels = oldImported.value(QStringLiteral("channels")).toArray();
        for (int index = 0; index < oldChannels.size(); ++index) {
            QJsonObject channel = oldChannels.at(index).toObject();
            channel.remove(QStringLiteral("highPower"));
            oldChannels[index] = channel;
        }
        oldImported.insert(QStringLiteral("channels"), oldChannels);
        oldRoot.insert(QStringLiteral("schemaVersion"), 4);
        oldRoot.insert(QStringLiteral("importedCodeplug"), oldImported);
        oldEntries.insert(QStringLiteral("project.json"), QJsonDocument(oldRoot).toJson());
        if (!ZipArchive::write(QStringLiteral("old-contact-name.waffle"), oldEntries, &error)) {
            qCritical() << "Could not create old-name project:" << error;
            return 23;
        }
        RepeaterStore migrated;
        if (!migrated.openProjectFromFile(QStringLiteral("old-contact-name.waffle"))
            || migrated.contacts().at(1).toMap().value(QStringLiteral("name")).toString()
                != QStringLiteral("NY NJ Metro")
            || migrated.radioName() != QStringLiteral("KB1JDX")
            || migrated.radioId() != 3109101) {
            qCritical() << "Old project did not recover the missing contact character";
            return 24;
        }
        const int migratedAres = migrated.importedRepeaterNames().indexOf(QStringLiteral("CT-Brooklyn Ares"));
        migrated.selectImportedGroup(migratedAres);
        if (migratedAres < 0
            || migrated.selectedImportedGroup().value(QStringLiteral("power")).toString()
                != QStringLiteral("Mixed")) {
            qCritical() << "Older project did not recover channel power from its source";
            return 28;
        }
        const int brooklyn = sampleReloaded.importedRepeaterNames().indexOf(QStringLiteral("CT-Brooklyn"));
        const int brooklynAres = sampleReloaded.importedRepeaterNames().indexOf(QStringLiteral("CT-Brooklyn Ares"));
        if (brooklyn < 0 || brooklynAres < 0 || brooklyn == brooklynAres) {
            qCritical() << "Brooklyn repeater zones were not recognized separately";
            return 8;
        }
        sampleReloaded.selectImportedGroup(brooklyn);
        const QVariantMap brooklynGroup = sampleReloaded.selectedImportedGroup();
        if (brooklynGroup.value(QStringLiteral("receiveFrequency")).toString() != QStringLiteral("445.7375")
            || brooklynGroup.value(QStringLiteral("transmitFrequency")).toString() != QStringLiteral("440.7375")
            || brooklynGroup.value(QStringLiteral("colorCode")).toInt() != 3
            || brooklynGroup.value(QStringLiteral("zones")).toList().size() != 1
            || brooklynGroup.value(QStringLiteral("simplexChannels")).toList().size() != 0
            || brooklynGroup.value(QStringLiteral("slot1Talkgroups")).toList().isEmpty()) {
            qCritical() << "Brooklyn frequency direction or mixed-zone classification is wrong";
            return 9;
        }
        bool hasLocal = false;
        for (const QVariant &value : brooklynGroup.value(QStringLiteral("slot2Talkgroups")).toList()) {
            const QVariantMap contact = value.toMap();
            if (contact.value(QStringLiteral("id")).toInt() == 9
                && contact.value(QStringLiteral("name")).toString() == QStringLiteral("Local")
                && !contact.value(QStringLiteral("channels")).toList().isEmpty())
                hasLocal = true;
        }
        if (!hasLocal) {
            qCritical() << "Brooklyn channels did not resolve their contact references";
            return 13;
        }
        sampleReloaded.selectImportedGroup(brooklynAres);
        const QVariantMap aresGroup = sampleReloaded.selectedImportedGroup();
        if (aresGroup.value(QStringLiteral("receiveFrequency")) != brooklynGroup.value(QStringLiteral("receiveFrequency"))
            || aresGroup.value(QStringLiteral("transmitFrequency")) != brooklynGroup.value(QStringLiteral("transmitFrequency"))
            || aresGroup.value(QStringLiteral("colorCode")) != brooklynGroup.value(QStringLiteral("colorCode"))
            || aresGroup.value(QStringLiteral("zones")).toList().size() != 1
            || aresGroup.value(QStringLiteral("simplexChannels")).toList().size() != 1) {
            qCritical() << "Brooklyn ARES should be a separate mixed repeater zone";
            return 10;
        }
        qInfo() << "Private sample import verified:" << parsed.channels.size() << "channels,"
                << parsed.zones.size() << "zones, 67 repeater entries";
    }
    return 0;
}
