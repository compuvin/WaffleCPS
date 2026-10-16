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
    if (!original.updateSelectedRepeater(QStringLiteral("Edited Repeater"),
                                         QStringLiteral("444.123456"), QStringLiteral("449.123456"), 7)
        || !original.addTalkgroup(2, QStringLiteral("New group"), 4567)
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
        || loaded.slot2Talkgroups().size() != 2) {
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

    qInfo() << "Project round-trip and corruption checks passed";
    return 0;
}
