#include "../src/codeplugdefinition.h"
#include "../src/cs7000default.h"
#include "../src/ziparchive.h"
#include "channel_order_fixture.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <stdexcept>

namespace {
void check(bool ok, const QString &message)
{
    if (!ok) throw std::runtime_error(message.toStdString());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), path + ": " + file.errorString());
    const QByteArray bytes = file.readAll();
    check(bytes.size() == file.size(), "Incomplete file: " + path);
    return bytes;
}

void compare(const ImportedCodeplug &a, const ImportedCodeplug &b, const QString &label)
{
    check(a.model == b.model && a.cpsVersion == b.cpsVersion && a.radioName == b.radioName && a.radioId == b.radioId,
          label + ": radio identity differs");
    check(a.channels.size() == b.channels.size() && a.contacts.size() == b.contacts.size() && a.zones.size() == b.zones.size(),
          label + ": table counts differ");
    for (qsizetype i = 0; i < a.channels.size(); ++i) {
        const auto &x = a.channels[i]; const auto &y = b.channels[i];
        check(x.name == y.name && x.frequencyAHz == y.frequencyAHz && x.frequencyBHz == y.frequencyBHz
                  && x.digital == y.digital && x.colorCode == y.colorCode && x.timeSlot == y.timeSlot
                  && x.contactIndex == y.contactIndex && x.highPower == y.highPower
                  && x.sourceRecord == y.sourceRecord && x.useDefaultTemplate == y.useDefaultTemplate,
              label + QStringLiteral(": channel %1 differs").arg(i));
    }
    for (qsizetype i = 0; i < a.contacts.size(); ++i) {
        const auto &x = a.contacts[i]; const auto &y = b.contacts[i];
        check(x.name == y.name && x.id == y.id && x.groupCall == y.groupCall,
              label + QStringLiteral(": contact %1 differs").arg(i));
    }
    for (qsizetype i = 0; i < a.zones.size(); ++i) {
        check(a.zones[i].name == b.zones[i].name && a.zones[i].channelIndexes == b.zones[i].channelIndexes
                  && a.zones[i].repeaterTemplate.has_value() == b.zones[i].repeaterTemplate.has_value(),
              label + QStringLiteral(": zone %1 differs").arg(i));
    }
}

QByteArray decoded(QByteArray bytes)
{
    const auto mask = bytes.mid(0x300, 256);
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = bytes[i] ^ mask[i % 256];
    return bytes;
}

void put32(QByteArray &bytes, qsizetype at, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + at));
}
void put16(QByteArray &bytes, qsizetype at, quint16 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data() + at));
}

quint32 get32(const QByteArray &bytes, qsizetype at)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + at));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString root = QStringLiteral(WAFFLE_SOURCE_DIR);
        const QString path = root + "/definitions/cs7000-m17-plus.wcp";
        CodeplugDefinition definition;
        QString error;
        check(CodeplugDefinition::load(path, &definition, &error), error);
        check(definition.id() == "cs7000-m17-plus" && definition.modelName() == "CS7000-M17 PLUS"
                  && definition.extensions() == QStringList{".rdbx"}, "Definition metadata differs");
        check(definition.defaultCodeplug() == cs7000DefaultCodeplug(), "Packaged and embedded defaults differ");
        const auto verify = [&](const QByteArray &bytes, const QString &label) {
            const QByteArray original = bytes;
            ImportedCodeplug existing, defined;
            check(CodeplugReader::read(bytes, &existing, &error), label + ": existing reader: " + error);
            check(definition.matchesModel(bytes, &error), label + ": model recognition: " + error);
            check(definition.read(bytes, &defined, &error), label + ": definition reader: " + error);
            compare(existing, defined, label);
            ImportedCodeplug raw;
            check(definition.read(decoded(bytes), &raw, &error), label + ": raw representation: " + error);
            compare(defined, raw, label + " raw");
            check(bytes == original, "Input bytes changed");
            qInfo().noquote() << label << ":" << defined.channels.size() << "channels,"
                              << defined.contacts.size() << "contacts," << defined.zones.size() << "zones match";
        };
        verify(definition.defaultCodeplug(), "Packaged default");
        const auto fixture = channelOrderFixture(&error);
        check(!fixture.isEmpty(), error);
        verify(fixture, "Edited/shared-channel fixture");

        // The test remains runnable without private samples. Compare every local
        // rdbx sample when present, without writing or modifying any source file.
        const QDir codeplugs(root + "/Codeplugs");
        const auto files = codeplugs.entryList({"*.rdbx"}, QDir::Files, QDir::Name);
        for (const auto &name : files) verify(readFile(codeplugs.filePath(name)), name);

        const QByteArray base = decoded(definition.defaultCodeplug());
        ImportedCodeplug sentinel;
        sentinel.model = "untouched";
        const auto refused = [&](const QByteArray &bytes, const QString &label, const QString &expected) {
            auto output = sentinel;
            check(!definition.read(bytes, &output, &error), label + ": unexpectedly accepted");
            check(output.model == "untouched", label + ": changed output on failure");
            check(error.contains(expected, Qt::CaseInsensitive), label + ": wrong error: " + error);
        };
        auto wrong = base;
        wrong.replace(0x1200, 16, QByteArray("CS750\0", 6).leftJustified(16, '\0'));
        check(!definition.matchesModel(wrong, &error), "Wrong model recognized");
        refused(wrong, "Wrong model", "Could not decode");
        refused(base.first(100), "Truncated header", "Could not decode");
        refused(base.first(base.size() - 1), "Truncated image", "relationship");
        wrong = base; put32(wrong, 0xf0, 0xfffffff0);
        check(definition.matchesModel(wrong, &error), "Structural damage changed model recognition");
        refused(wrong, "Invalid table pointer", "address");
        wrong = base; put32(wrong, 0x2424, 0xffffffff);
        refused(wrong, "Huge count", "limit");
        wrong = base; put16(wrong, 0x23768 + 48 + 24, 0xffff);
        refused(wrong, "Invalid contact reference", "reference");
        wrong = base; put16(wrong, 0x52954 + 36, 0xffff);
        refused(wrong, "Invalid zone reference", "reference");
        wrong = base; put32(wrong, 0x52954 + 32, 0xffffffff);
        refused(wrong, "Huge member count", "limit");
        wrong = base; put32(wrong, 0x1264, get32(wrong, 0x1264) + 1);
        refused(wrong, "Pointer aliases disagree", "equality");
        wrong = base;
        const auto groupIndex = get32(base, 0x1f8);
        put16(wrong, groupIndex, 2);
        wrong[groupIndex + 2] = 1; wrong[groupIndex + 3] = 1;
        refused(wrong, "Duplicate selected group", "duplicate");
        wrong = base; wrong[0x1299] = '9';
        check(definition.read(wrong, &sentinel, &error), "Different CPS version rejected: " + error);
        check(sentinel.cpsVersion == "V9.02.21.00", "Different CPS version not retained");

        // A definition really drives decoding: move the model field and update
        // only its schema/template. The original reader must reject this image.
        QTemporaryDir temporary;
        check(temporary.isValid(), "Could not create temporary test directory");
        QMap<QString, QByteArray> entries;
        check(ZipArchive::read(path, &entries, &error), error);
        const auto originalEntries = entries;
        const QString testPath = temporary.filePath("test.wcp");
        const auto packageRefused = [&](const QMap<QString, QByteArray> &bad, const QString &expected) {
            check(ZipArchive::write(testPath, bad, &error), error);
            auto output = definition;
            check(!CodeplugDefinition::load(testPath, &output, &error), "Invalid package accepted");
            check(error.contains(expected, Qt::CaseInsensitive), "Wrong package error: " + error);
            check(output.id() == definition.id(), "Changed definition on failed load");
        };
        auto json = QJsonDocument::fromJson(entries.value("definition.json")).object();
        const QString templateName = json.value("creation").toObject().value("template").toString();
        entries[templateName][100] = char(entries[templateName][100] ^ 1);
        packageRefused(entries, "checksum");
        entries = originalEntries; entries.remove(templateName);
        packageRefused(entries, "missing");
        entries = originalEntries; entries["definition.json"] = "{";
        packageRefused(entries, "JSON");
        entries = originalEntries; entries["../outside"] = "x";
        packageRefused(entries, "Unsafe");
        entries = originalEntries;
        auto badSchema = QJsonDocument::fromJson(entries.value("definition.json")).object();
        auto badTables = badSchema.value("tables").toObject();
        auto scan = badTables.value("scan_lists").toObject();
        auto scanFields = scan.value("fields").toObject();
        auto scanName = scanFields.value("name").toObject();
        scanName["type"] = "unsupported";
        scanFields["name"] = scanName; scan["fields"] = scanFields;
        badTables["scan_lists"] = scan; badSchema["tables"] = badTables;
        entries["definition.json"] = QJsonDocument(badSchema).toJson();
        packageRefused(entries, "Unsupported definition field type");
        entries = originalEntries;
        auto fields = json.value("fields").toObject();
        auto radioId = fields.value("radio_id").toObject();
        radioId["at"] = QJsonObject{{"field", "radio_id"}};
        fields["radio_id"] = radioId; json["fields"] = fields;
        entries["definition.json"] = QJsonDocument(json).toJson();
        packageRefused(entries, "Cyclic");

        entries = originalEntries;
        json = QJsonDocument::fromJson(entries.value("definition.json")).object();
        fields = json.value("fields").toObject();
        auto model = fields.value("model").toObject(); model["at"] = "0x1280";
        fields["model"] = model; json["fields"] = fields;
        wrong = base; wrong.replace(0x1280, 16, base.mid(0x1200, 16));
        wrong.replace(0x1200, 16, QByteArray(16, '\0'));
        // An identity representation keeps this fixture readable without encoding.
        auto creation = json.value("creation").toObject();
        creation["representation"] = "decoded_radio_image";
        creation["template_sha256"] = QString::fromLatin1(QCryptographicHash::hash(wrong, QCryptographicHash::Sha256).toHex());
        json["creation"] = creation;
        entries[templateName] = wrong;
        entries["definition.json"] = QJsonDocument(json).toJson();
        check(ZipArchive::write(testPath, entries, &error), error);
        CodeplugDefinition moved;
        check(CodeplugDefinition::load(testPath, &moved, &error), "Moved field schema rejected: " + error);
        ImportedCodeplug movedResult;
        check(moved.read(wrong, &movedResult, &error), "Moved field decoding failed: " + error);
        check(movedResult.model == definition.modelName(), "Moved field not used");
        check(!definition.read(wrong, &movedResult, &error), "Original definition accepted relocated model");
        check(!CodeplugReader::read(wrong, &movedResult, &error), "Existing reader accepted relocated model");
        qInfo() << "Definition recognition, validation, schema relocation and rejection tests passed";
        return 0;
    } catch (const std::exception &e) {
        qCritical().noquote() << e.what();
        return 1;
    }
}
