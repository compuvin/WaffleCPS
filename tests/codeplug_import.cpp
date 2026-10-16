#include "../src/codeplugreader.h"
#include "../src/repeaterstore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
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
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QByteArray bytes(0x53000, '\0');
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
        bytes[record + 23] = 0x20;
    }
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
        || parsed.channels.at(1).timeSlot != 2) {
        qCritical() << "Synthetic codeplug import failed:" << error;
        return 1;
    }
    QByteArray unsupported = bytes;
    unsupported[0x1299] = '2';
    if (CodeplugReader::read(unsupported, &parsed, &error)) {
        qCritical() << "Unsupported CPS version was accepted";
        return 7;
    }
    RepeaterStore store;
    if (!store.importCodeplugFromBytes(bytes) || store.importedRepeaterNames().size() != 1
        || store.importedZoneNames().size() != 2
        || store.selectedImportedGroup().value(QStringLiteral("zones")).toList().size() != 2
        || store.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 1
        || !store.addSimplexToSelectedImportedRepeater(0, QStringLiteral("Extra"),
                                                       QStringLiteral("146.520"), false, 0, 0)
        || store.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 2
        || !store.saveProjectToFile(QStringLiteral("imported.waffle"))) {
        qCritical() << "Classification or project save failed";
        return 2;
    }
    RepeaterStore reopened;
    if (!reopened.openProjectFromFile(QStringLiteral("imported.waffle"))
        || !reopened.hasImportedCodeplug() || reopened.importedRepeaterNames().size() != 1
        || reopened.importedZoneNames().size() != 2
        || reopened.selectedImportedGroup().value(QStringLiteral("simplexChannels")).toList().size() != 2) {
        qCritical() << "Imported project did not round-trip";
        return 3;
    }
    if (argc > 1) {
        const QString samplePath = QString::fromLocal8Bit(argv[1]);
        QFile sample(samplePath);
        if (!sample.open(QIODevice::ReadOnly)
            || !CodeplugReader::read(sample.readAll(), &parsed, &error)
            || parsed.channels.size() != 821 || parsed.zones.size() != 82) {
            qCritical() << "Private sample import failed:" << error;
            return 4;
        }
        RepeaterStore sampleStore;
        if (!sampleStore.importCodeplugFromFile(samplePath)
            || sampleStore.importedRepeaterNames().size() != 39
            || sampleStore.importedZoneNames().size() != 15
            || !sampleStore.saveProjectToFile(QStringLiteral("sample-import.waffle"))) {
            qCritical() << "Private sample classification or save failed";
            return 5;
        }
        RepeaterStore sampleReloaded;
        if (!sampleReloaded.openProjectFromFile(QStringLiteral("sample-import.waffle"))
            || sampleReloaded.importedRepeaterNames().size() != 39
            || sampleReloaded.importedZoneNames().size() != 15
            || sampleReloaded.sourceCodeplugName() != QFileInfo(samplePath).fileName()) {
            qCritical() << "Private sample project did not round-trip";
            return 6;
        }
        const int brooklyn = sampleReloaded.importedRepeaterNames().indexOf(QStringLiteral("CT-Brooklyn"));
        if (brooklyn < 0) {
            qCritical() << "Brooklyn repeater was not recognized";
            return 8;
        }
        sampleReloaded.selectImportedGroup(brooklyn);
        const QVariantMap brooklynGroup = sampleReloaded.selectedImportedGroup();
        if (brooklynGroup.value(QStringLiteral("receiveFrequency")).toString() != QStringLiteral("445.7375")
            || brooklynGroup.value(QStringLiteral("transmitFrequency")).toString() != QStringLiteral("440.7375")
            || brooklynGroup.value(QStringLiteral("colorCode")).toInt() != 3
            || brooklynGroup.value(QStringLiteral("zones")).toList().size() != 2
            || brooklynGroup.value(QStringLiteral("simplexChannels")).toList().size() != 1) {
            qCritical() << "Brooklyn frequency direction or mixed-zone classification is wrong";
            return 9;
        }
        qInfo() << "Private sample import verified:" << parsed.channels.size() << "channels,"
                << parsed.zones.size() << "zones, 39 repeater groups";
    }
    return 0;
}
