#include "../src/codeplugdefinition.h"
#include <QCoreApplication>
#include <QFile>
#include <QDebug>
#include <algorithm>
#include <stdexcept>

static void check(bool ok, const QString &message) { if (!ok) throw std::runtime_error(message.toStdString()); }
static QByteArray readFile(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "Could not open " + path);
    return file.readAll();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString root = QStringLiteral(WAFFLE_SOURCE_DIR);
        CodeplugDefinition def; QString error;
        check(CodeplugDefinition::load(root + "/definitions/md380.wcp", &def, &error), error);
        const QByteArray sample = readFile(root + "/Codeplugs/K1AGL-2015-12-13.rdt");
        ImportedCodeplug plug;
        check(def.read(sample, &plug, &error), error);
        qInfo() << plug.model << plug.radioName << plug.radioId << plug.channels.size() << plug.contacts.size() << plug.zones.size();
        QByteArray unchanged;
        check(def.write(sample, plug, plug.radioName, plug.radioId, &unchanged, &error), error);
        check(unchanged == sample, "Unchanged export differs");
        plug.radioName = "NOCALL";
        plug.channels[0].name = "MD380 TEST";
        plug.contacts[0].name = "Local Test";
        plug.zones[0].name = "Test Zone";
        QByteArray edited;
        check(def.write(sample, plug, plug.radioName, plug.radioId, &edited, &error), error);
        ImportedCodeplug again;
        check(def.read(edited, &again, &error), error);
        check(again.channels[0].name == "MD380 TEST" && again.contacts[0].name == "Local Test"
                  && again.zones[0].name == "Test Zone" && again.radioName == "NOCALL", "Edited values did not roundtrip");
        ImportedCodeplug starter;
        check(def.read(def.defaultCodeplug(), &starter, &error), "Starter image: " + error);
        check(starter.radioName == "NOCALL" && starter.radioId == 1 && starter.channels.size() == 2
                  && starter.contacts.size() == 1 && starter.zones.size() == 1, "Starter content differs");
        auto added = starter.channels[1];
        added.name = "NEW DIGITAL"; added.sourceRecord = -1; added.useDefaultTemplate = true;
        starter.channels.append(added); starter.zones[0].channelIndexes.append(2);
        starter.contacts.append({"Another", 12345, true});
        starter.channels.last().contactIndex = 2;
        QByteArray grown;
        check(def.write(def.defaultCodeplug(), starter, starter.radioName, starter.radioId, &grown, &error), "Add: " + error);
        check(def.read(grown, &again, &error), "Reimport added: " + error);
        check(again.channels.size() == 3 && again.contacts.size() == 2 && again.zones[0].channelIndexes.size() == 3,
              "Added records missing");
        auto reordered = plug;
        std::reverse(reordered.contacts.begin(), reordered.contacts.end());
        for (auto &channel : reordered.channels)
            if (channel.contactIndex) channel.contactIndex = reordered.contacts.size() + 1 - channel.contactIndex;
        std::reverse(reordered.channels.begin(), reordered.channels.end());
        for (auto &zone : reordered.zones)
            for (auto &index : zone.channelIndexes) index = reordered.channels.size() - 1 - index;
        QByteArray moved;
        check(def.write(sample, reordered, reordered.radioName, reordered.radioId, &moved, &error), "Reorder: " + error);
        check(def.read(moved, &again, &error), "Reimport reordered: " + error);
        check(again.channels.first().name == reordered.channels.first().name
                  && again.contacts.first().name == reordered.contacts.first().name,
              "Reordered records differ");
        auto reduced = starter;
        reduced.channels.removeFirst();
        reduced.zones[0].channelIndexes = {0, 1};
        QByteArray removed;
        check(def.write(grown, reduced, reduced.radioName, reduced.radioId, &removed, &error), "Delete: " + error);
        check(def.read(removed, &again, &error), "Reimport deleted: " + error);
        check(again.channels.size() == 2 && again.zones[0].channelIndexes.size() == 2,
              "Deleted record remains");
        if (argc > 1) {
            const QByteArray reference = readFile(QString::fromLocal8Bit(argv[1]));
            ImportedCodeplug publicPlug;
            check(def.read(reference, &publicPlug, &error), "Reference import: " + error);
            QByteArray same;
            check(def.write(reference, publicPlug, publicPlug.radioName, publicPlug.radioId, &same, &error), error);
            check(same == reference, "Reference unchanged export differs");
            publicPlug.channels[0].name = "MD380 REF TEST";
            check(def.write(reference, publicPlug, publicPlug.radioName, publicPlug.radioId, &same, &error),
                  "Reference edit: " + error);
            check(def.read(same, &publicPlug, &error), "Reference reimport: " + error);
            check(publicPlug.channels[0].name == "MD380 REF TEST", "Reference edit did not roundtrip");
            qInfo() << "Reference counts:" << publicPlug.channels.size() << publicPlug.contacts.size() << publicPlug.zones.size();
        }
        return 0;
    } catch (const std::exception &e) { qCritical() << e.what(); return 1; }
}
