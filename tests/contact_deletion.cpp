#include "../src/codeplugdefinition.h"
#include "../src/codeplugdefinitions.h"
#include "../src/repeaterstore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QDebug>
#include <stdexcept>

static void check(bool ok, const QString &reason)
{
    if (!ok) throw std::runtime_error(reason.toStdString());
}

static int indexOfId(const RepeaterStore &store, int id)
{
    const auto list = store.contacts();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].toMap().value("id").toInt() == id) return i;
    return -1;
}

static qint64 number(const QJsonValue &value)
{
    if (value.isDouble()) return qint64(value.toDouble());
    bool ok = false;
    const auto n = value.toString().toLongLong(&ok, 0);
    check(ok, "Invalid fixture offset");
    return n;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QDir::setCurrent(WAFFLE_SOURCE_DIR);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "Temporary directory");
        RepeaterStore blank;
        blank.newProject();
        check(blank.addContact("Unused", 9100001, false), "Add blank-project contact");
        check(blank.contactDeletionInfo(0).value("canDelete").toBool(), "Unused blank contact blocked");
        check(!blank.deleteContact(0, 123, "Unused"), "Stale confirmation accepted");
        check(blank.contacts().size() == 1, "Stale confirmation changed contacts");
        check(blank.deleteContact(0, 9100001, "Unused"), "Delete unused blank contact");
        check(blank.contacts().isEmpty(), "Blank contact remains");
        check(blank.addRepeater("Local", "444.350", "449.350", 1), "Add manual repeater");
        check(blank.addTalkgroup(1, "Local", 9, "Local TS1"), "Add manual talkgroup");
        const int manual = indexOfId(blank, 9);
        check(manual >= 0 && !blank.contactDeletionInfo(manual).value("canDelete").toBool(), "Used manual contact allowed");
        check(!blank.deleteContact(manual, 9, "Local"), "Used manual contact deleted");

        for (const QString definition : {QString("cs7000-m17-plus.wcp"), QString("dm32uv.wcp")}) {
            QString error;
            CodeplugDefinition decoder;
            check(CodeplugDefinition::load("definitions/" + definition, &decoder, &error), error);
            const auto base = decoder.defaultCodeplug();
            ImportedCodeplug target;
            check(decoder.read(base, &target, &error), error);
            target.radioId = 3109101;
            target.contacts.prepend({"Unused", 9100001, false});
            for (auto &channel : target.channels)
                if (channel.contactIndex > 0) ++channel.contactIndex;
            if (definition == "dm32uv.wcp") target.contacts.append({"RX only", 9100002, true});
            QByteArray bytes;
            check(decoder.write(base, target, target.radioName, target.radioId, &bytes, &error), error);
            if (definition == "dm32uv.wcp") {
                const auto document = decoder.document();
                const auto group = document.value("tables").toObject().value("receive_groups").toObject();
                const auto fields = group.value("fields").toObject();
                const int at = int(number(group.value("at")));
                const int stride = int(number(group.value("stride_bytes")));
                bytes.replace(at, stride, QByteArray(stride, '\0'));
                bytes.replace(at, 8, "RX Group");
                const int members = at + int(number(fields.value("members").toObject().value("offset")));
                for (int i = 0; i < 3; ++i) bytes[members + i] = char((9100002 >> (8 * i)) & 255);
            }
            ImportedCodeplug before;
            check(decoder.read(bytes, &before, &error), error);
            const QString input = temporary.filePath(definition + ".input");
            QFile file(input);
            check(file.open(QIODevice::WriteOnly), "Fixture file");
            check(file.write(bytes) == bytes.size(), "Fixture write");
            file.close();
            RepeaterStore store;
            check(store.importCodeplugFromFile(input, definition), "Fixture import");
            check(store.contactDeletionInfo(0).value("canDelete").toBool(), "Contact-order index counted as contact usage");
            for (const auto &channel : before.channels) {
                if (!channel.contactIndex) continue;
                const int i = channel.contactIndex - 1;
                check(!store.contactDeletionInfo(i).value("canDelete").toBool(), "Channel contact allowed");
                const auto c = store.contacts()[i].toMap();
                check(!store.deleteContact(i, c.value("id").toInt(), c.value("name").toString()), "Channel contact deleted");
                break;
            }
            if (definition == "dm32uv.wcp") {
                const int groupOnly = indexOfId(store, 9100002);
                const auto info = store.contactDeletionInfo(groupOnly);
                check(!info.value("canDelete").toBool() && info.value("reason").toString().contains("RX Group"), "ID-based receive-group use missed");
            }
            check(store.deleteContact(0, 9100001, "Unused"), "Delete unused imported contact");
            const auto output = store.exportCodeplugBytes();
            ImportedCodeplug after;
            check(!output.isEmpty() && decoder.read(output, &after, &error), error);
            check(after.contacts.size() == before.contacts.size() - 1 && after.channels.size() == before.channels.size(), "Incorrect deletion counts");
            for (int i = 0; i < before.channels.size(); ++i) {
                const int oldIndex = before.channels[i].contactIndex;
                const int newIndex = after.channels[i].contactIndex;
                check(!oldIndex || (newIndex > 0 && before.contacts[oldIndex - 1].id == after.contacts[newIndex - 1].id), "Channel contact identity changed");
            }
            check(indexOfId(store, 9100001) == -1 && store.modified(), "Deleted contact/state incorrect");
            const QString project = temporary.filePath(definition + ".waffle");
            check(store.saveProjectToFile(project), "Save deleted contact");
            RepeaterStore reopened;
            check(reopened.openProjectFromFile(project), "Reopen deleted contact");
            check(indexOfId(reopened, 9100001) == -1 && reopened.exportCodeplugBytes() == output, "Deletion did not persist");
            if (definition == "dm32uv.wcp")
                check(!reopened.contactDeletionInfo(indexOfId(reopened, 9100002)).value("canDelete").toBool(), "RX use lost after deletion/reopen");
        }
        qInfo() << "PASS unused/referenced contacts, ID-based usage, stale confirmation, index remapping, export and persistence";
        return 0;
    } catch (const std::exception &exception) {
        qCritical() << exception.what();
        return 1;
    }
}
