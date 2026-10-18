#include "../src/repeaterstore.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString sourcePath = QStringLiteral("roundtrip-source.rdbx");
    const QString projectPath = QStringLiteral("roundtrip.waffle");

    QByteArray source;
    source.reserve(13 * 1024 * 1024);
    const QByteArray pattern("WaffleCPS binary source\0\xff", 25);
    while (source.size() < 13 * 1024 * 1024)
        source.append(pattern);
    source.resize(13 * 1024 * 1024);
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::WriteOnly) || sourceFile.write(source) != source.size())
        return 1;
    sourceFile.close();

    RepeaterStore original;
    if (original.contacts().size() != 4)
        return 11;
    RepeaterStore collision;
    if (!collision.addContact(QStringLiteral("Local"), 1234567, false)
        || collision.addContact(QStringLiteral("Different name"), 1234567, true)
        || !collision.addContact(QStringLiteral("Local"), 7654321, true)
        || collision.updateContact(4, QStringLiteral("Other name"), 1234567)) {
        qCritical() << "Contact DMR ID collision across tabs was not handled";
        return 13;
    }
    if (!original.addContact(QStringLiteral("K1ABC Ron"), 1234567, false)
        || !original.addContact(QStringLiteral("New group"), 4567, true)
        || original.updateContact(0, QStringLiteral("Another name"), 4567)
        || !original.updateContact(2, QStringLiteral("Local edited"), 10)
        || original.slot1Talkgroups().at(0).toMap().value(QStringLiteral("id")).toInt() != 10) {
        qCritical() << "Manual contacts could not be added or edited";
        return 12;
    }
    if (!original.updateSelectedRepeater(QStringLiteral("Edited Repeater"),
                                         QStringLiteral("444.123456"), QStringLiteral("449.123456"), 7)
        || original.addTalkgroup(1, QStringLiteral("K1ABC Ron"), 1234567, QStringLiteral("Invalid private"))
        || !original.addTalkgroup(2, QStringLiteral("New group"), 4567,
                                  QStringLiteral("Ridge New Group"))
        || original.slot2Talkgroups().at(1).toMap().value(QStringLiteral("channelName")).toString()
            != QStringLiteral("Ridge New Group")
        || !original.attachSourceCodeplugFromFile(sourcePath)
        || !original.saveProjectToFile(projectPath)) {
        qCritical() << "Could not create test project";
        return 2;
    }

    RepeaterStore loaded;
    if (!loaded.openProjectFromFile(projectPath)
        || loaded.selectedName() != QStringLiteral("Edited Repeater")
        || loaded.rxFrequency() != QStringLiteral("444.123456")
        || loaded.txFrequency() != QStringLiteral("449.123456")
        || loaded.colorCode() != 7
        || loaded.sourceCodeplugName() != sourcePath
        || loaded.modified()
        || loaded.slot2Talkgroups().size() != 2
        || loaded.contacts().size() != 6
        || loaded.contacts().at(0).toMap().value(QStringLiteral("name")).toString() != QStringLiteral("K1ABC Ron")
        || loaded.slot1Talkgroups().at(0).toMap().value(QStringLiteral("id")).toInt() != 10
        || loaded.slot2Talkgroups().at(1).toMap().value(QStringLiteral("channelName")).toString()
            != QStringLiteral("Ridge New Group")) {
        qCritical() << "Project did not round-trip";
        return 3;
    }

    QFile archive(projectPath);
    if (!archive.open(QIODevice::ReadOnly))
        return 4;
    QByteArray damaged = archive.readAll();
    archive.close();
    if (damaged.size() < 200)
        return 5;
    damaged[100] = static_cast<char>(damaged.at(100) ^ 0x01);
    QFile damagedFile(QStringLiteral("damaged.waffle"));
    if (!damagedFile.open(QIODevice::WriteOnly) || damagedFile.write(damaged) != damaged.size())
        return 6;
    damagedFile.close();
    if (loaded.openProjectFromFile(damagedFile.fileName())
        || loaded.selectedName() != QStringLiteral("Edited Repeater")) {
        qCritical() << "Damaged project was accepted or replaced the current project";
        return 7;
    }

    loaded.newProject();
    if (!loaded.repeaterNames().isEmpty() || !loaded.sourceCodeplugName().isEmpty()
        || loaded.modified() || !loaded.saveProjectToFile(QStringLiteral("empty.waffle"))) {
        qCritical() << "New project was not empty or could not be saved";
        return 8;
    }
    RepeaterStore empty;
    if (!empty.openProjectFromFile(QStringLiteral("empty.waffle"))
        || !empty.repeaterNames().isEmpty()
        || !empty.addRepeater(QStringLiteral("Brooklyn"), QStringLiteral("445.7375"),
                              QStringLiteral("440.7375"), 3)
        || !empty.addSimplexToSelectedRepeater(QStringLiteral("Local simplex"),
                                                QStringLiteral("446.000"), true, 3, 1)
        || !empty.saveProjectToFile(QStringLiteral("simplex.waffle"))) {
        qCritical() << "Could not add a repeater and simplex frequency";
        return 9;
    }
    RepeaterStore withSimplex;
    if (!withSimplex.openProjectFromFile(QStringLiteral("simplex.waffle"))
        || withSimplex.selectedName() != QStringLiteral("Brooklyn")
        || withSimplex.selectedSimplexFrequencies().size() != 1) {
        qCritical() << "Simplex frequency did not round-trip";
        return 10;
    }
    if (withSimplex.removeSimplexFromSelectedRepeater(1)
        || !withSimplex.removeSimplexFromSelectedRepeater(0)
        || !withSimplex.selectedSimplexFrequencies().isEmpty()
        || !withSimplex.modified()) {
        qCritical() << "Manual simplex frequency removal failed";
        return 10;
    }

    RepeaterStore newContactFlow;
    newContactFlow.newProject();
    if (!newContactFlow.addRepeater(QStringLiteral("Test"), QStringLiteral("445.7375"),
                                    QStringLiteral("440.7375"), 3)
        || !newContactFlow.addTalkgroup(2, QStringLiteral("Statewide"), 3100,
                                        QStringLiteral("Brooklyn CT Wide"))
        || newContactFlow.contacts().size() != 1
        || newContactFlow.slot2Talkgroups().at(0).toMap().value(QStringLiteral("name")).toString()
            != QStringLiteral("Statewide")
        || newContactFlow.slot2Talkgroups().at(0).toMap().value(QStringLiteral("channelName")).toString()
            != QStringLiteral("Brooklyn CT Wide")
        || !newContactFlow.saveProjectToFile(QStringLiteral("named-channel.waffle"))) {
        qCritical() << "New talkgroup and separate channel name failed";
        return 14;
    }
    RepeaterStore namedReloaded;
    if (!namedReloaded.openProjectFromFile(QStringLiteral("named-channel.waffle"))
        || namedReloaded.contacts().size() != 1
        || namedReloaded.slot2Talkgroups().at(0).toMap().value(QStringLiteral("channelName")).toString()
            != QStringLiteral("Brooklyn CT Wide")) {
        qCritical() << "Separate channel name did not round-trip";
        return 15;
    }
    namedReloaded.removeTalkgroup(2, 3100);
    if (namedReloaded.contacts().size() != 1) {
        qCritical() << "Reusable talkgroup was lost after removing its channel";
        return 16;
    }

    qInfo() << "Project round-trip and corruption checks passed";
    return 0;
}
