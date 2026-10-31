#include "../src/radiodefinition.h"
#include "../src/ziparchive.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString path = QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/cs7000-m17-plus.wrd");
        RadioDefinition definition;
        QString error;
        check(RadioDefinition::load(path, &definition, &error), "Packaged radio read plan was rejected");
        check(definition.canRead() && definition.canWrite(), "Unexpected radio capabilities");
        check(definition.readOperations().size() == 3, "Wrong read operation count");
        check(definition.readOperations().at(1).region == QStringLiteral("main")
              && definition.readOperations().at(2).region == QStringLiteral("contacts")
              && definition.readOperations().at(2).checkFirstBlockSize
              && definition.readOperations().at(2).sizeCheckOffset == 4,
              "Read operations did not retain the declared order and size check");

        QMap<QString, QByteArray> wcpEntries;
        check(ZipArchive::read(QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/cs7000-m17-plus.wcp"),
                               &wcpEntries, &error), "Cannot open companion codeplug definition");
        const QJsonObject wcp = QJsonDocument::fromJson(wcpEntries.value(QStringLiteral("definition.json"))).object();
        const QJsonObject writePlan = definition.writePlan();
        check(writePlan.value(QStringLiteral("codeplug_definition_id")) == wcp.value(QStringLiteral("id")),
              "Write plan targets a different codeplug definition");
        bool representationFound = false;
        for (const QJsonValue &value : wcp.value(QStringLiteral("representations")).toArray())
            representationFound |= value.toObject().value(QStringLiteral("name"))
                == writePlan.value(QStringLiteral("representation"));
        check(representationFound, "Write representation does not exist in companion definition");
        const QJsonObject fields = wcp.value(QStringLiteral("fields")).toObject();
        const QJsonObject tables = wcp.value(QStringLiteral("tables")).toObject();
        for (const QJsonValue &value : writePlan.value(QStringLiteral("regions")).toObject()) {
            const QJsonObject region = value.toObject();
            for (const char *key : {"start", "size"}) {
                const QJsonObject reference = region.value(QLatin1String(key)).toObject();
                if (!reference.isEmpty())
                    check(fields.contains(reference.value(QStringLiteral("field")).toString()),
                          "Write region references a missing codeplug field");
            }
            for (const QJsonValue &table : region.value(QStringLiteral("selection")).toObject()
                                                .value(QStringLiteral("tables")).toArray())
                check(tables.contains(table.toString()), "Sparse write region references a missing codeplug table");
        }

        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "Cannot open packaged radio definition");
        const QJsonObject original = QJsonDocument::fromJson(file.readAll()).object();
        const auto expectRejected = [&](const QJsonArray &operations, const char *message) {
            QJsonObject root = original;
            QJsonObject methods = root.value(QStringLiteral("methods")).toObject();
            QJsonObject read = methods.value(QStringLiteral("read_codeplug")).toObject();
            read.insert(QStringLiteral("operations"), operations);
            methods.insert(QStringLiteral("read_codeplug"), read);
            root.insert(QStringLiteral("methods"), methods);
            RadioDefinition candidate;
            QString detail;
            check(!RadioDefinition::fromJson(QJsonDocument(root).toJson(), &candidate, &detail)
                  && !detail.isEmpty(), message);
        };
        const QJsonArray valid = original.value(QStringLiteral("methods")).toObject()
            .value(QStringLiteral("read_codeplug")).toObject()
            .value(QStringLiteral("operations")).toArray();
        QJsonArray unknown = valid;
        QJsonObject operation = unknown.at(1).toObject();
        operation.insert(QStringLiteral("operation"), QStringLiteral("execute_anything"));
        unknown.replace(1, operation);
        expectRejected(unknown, "Unknown read operation was accepted");

        QJsonArray missing = valid;
        missing.removeAt(2);
        expectRejected(missing, "Incomplete region list was accepted");

        QJsonArray invalidCheck = valid;
        operation = invalidCheck.at(2).toObject();
        operation.insert(QStringLiteral("first_block_size_check"),
                         QJsonObject{{QStringLiteral("offset"), 2047},
                                     {QStringLiteral("type"), QStringLiteral("u32le")}});
        invalidCheck.replace(2, operation);
        expectRejected(invalidCheck, "Out-of-bounds first-block size check was accepted");
        const auto expectWriteRejected = [&](const QJsonObject &write, const char *message) {
            QJsonObject root = original;
            QJsonObject methods = root.value(QStringLiteral("methods")).toObject();
            methods.insert(QStringLiteral("write_codeplug"), write);
            root.insert(QStringLiteral("methods"), methods);
            RadioDefinition candidate;
            QString detail;
            check(!RadioDefinition::fromJson(QJsonDocument(root).toJson(), &candidate, &detail)
                  && !detail.isEmpty(), message);
        };
        const QJsonObject write = original.value(QStringLiteral("methods")).toObject()
            .value(QStringLiteral("write_codeplug")).toObject();
        check(write.value(QStringLiteral("regions")).toObject().size() == 2,
              "Packaged write regions are missing");
        QJsonObject wrongDefinition = write;
        wrongDefinition.insert(QStringLiteral("codeplug_definition_id"), QStringLiteral("other-radio"));
        expectWriteRejected(wrongDefinition, "Mismatched codeplug definition was accepted");

        QJsonObject missingRegion = write;
        QJsonObject regions = missingRegion.value(QStringLiteral("regions")).toObject();
        regions.remove(QStringLiteral("contacts"));
        missingRegion.insert(QStringLiteral("regions"), regions);
        expectWriteRejected(missingRegion, "Missing write region was accepted");

        QJsonObject unknownWrite = write;
        QJsonArray writeOperations = unknownWrite.value(QStringLiteral("operations")).toArray();
        operation = writeOperations.at(3).toObject();
        operation.insert(QStringLiteral("operation"), QStringLiteral("erase_radio"));
        writeOperations.replace(3, operation);
        unknownWrite.insert(QStringLiteral("operations"), writeOperations);
        expectWriteRejected(unknownWrite, "Unknown write operation was accepted");

        QJsonObject invalidSelection = write;
        regions = invalidSelection.value(QStringLiteral("regions")).toObject();
        QJsonObject contacts = regions.value(QStringLiteral("contacts")).toObject();
        QJsonObject selection = contacts.value(QStringLiteral("selection")).toObject();
        selection.insert(QStringLiteral("tables"), QJsonArray{});
        contacts.insert(QStringLiteral("selection"), selection);
        regions.insert(QStringLiteral("contacts"), contacts);
        invalidSelection.insert(QStringLiteral("regions"), regions);
        expectWriteRejected(invalidSelection, "Empty sparse table list was accepted");
        QJsonObject invalidRequest = write;
        QJsonObject request = invalidRequest.value(QStringLiteral("request")).toObject();
        QJsonArray payload = request.value(QStringLiteral("payload")).toArray();
        payload.removeAt(1);
        request.insert(QStringLiteral("payload"), payload);
        invalidRequest.insert(QStringLiteral("request"), request);
        expectWriteRejected(invalidRequest, "Incomplete write request payload was accepted");
        std::cout << "Packaged radio read/write operations and malformed plans verified\n";
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
