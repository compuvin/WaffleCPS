#include "../src/codeplugdefinition.h"
#include "../src/dfuexecutor.h"
#include "../src/radioimage.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>

static void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString base = QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/md380");
        QString error;
        RadioDefinition radio;
        CodeplugDefinition codeplug;
        check(RadioDefinition::load(base + QStringLiteral(".wrd"), &radio, &error),
              "MD-380 radio definition was rejected");
        check(radio.usesDfu() && radio.canRead() && radio.canWrite(),
              "MD-380 DFU capabilities are missing");
        RadioDefinition cs700;
        check(RadioDefinition::load(QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/cs700.wrd"), &cs700, &error),
              "CS700 radio definition was rejected");
        for (const RadioDefinition *definition : {&radio, &cs700}) {
            const QJsonObject methods = definition->document().value(QStringLiteral("methods")).toObject();
            for (const char *name : {"read_codeplug", "write_codeplug"}) {
                const QJsonArray steps = methods.value(QLatin1String(name)).toObject()
                    .value(QStringLiteral("operations")).toArray();
                check(steps.last().toObject().value(QStringLiteral("operation"))
                          == QStringLiteral("exit_program_mode"),
                      "DFU transfer does not exit programming mode");
            }
        }
        check(CodeplugDefinition::load(base + QStringLiteral(".wcp"), &codeplug, &error),
              "MD-380 codeplug definition was rejected");
        check(radio.compatibleWithCodeplug(codeplug.document(), &error),
              "MD-380 radio and codeplug definitions disagree");

        const QByteArray rdt = codeplug.defaultCodeplug();
        const QByteArray raw = rdt.mid(549, 262144);
        check(raw.size() == 262144, "Packaged radio image is incomplete");
        const QByteArray assembled = assembleRadioImage(radio, codeplug.defaultCodeplug(), raw);
        check(assembled.size() == 262709 && assembled.mid(549, 262144) == raw,
              "Downloaded radio bytes did not reach the .rdt payload");
        ImportedCodeplug imported;
        check(codeplug.read(assembled, &imported, &error), "Assembled radio image cannot be imported");
        const RadioExecutor::Bytes encoded(rdt.begin(), rdt.end());
        check(DfuExecutor::writePayload(radio, encoded) == RadioExecutor::Bytes(raw.begin(), raw.end()),
              "DFU write payload differs from packaged codeplug payload");
        QJsonObject shifted = radio.document();
        QJsonObject shiftedMethods = shifted.value(QStringLiteral("methods")).toObject();
        QJsonObject shiftedRead = shiftedMethods.value(QStringLiteral("read_codeplug")).toObject();
        QJsonObject shiftedAssembly = shiftedRead.value(QStringLiteral("assembly")).toObject();
        shiftedAssembly.insert(QStringLiteral("wcp_payload_offset"), 548);
        shiftedAssembly.insert(QStringLiteral("wcp_suffix_bytes"), 17);
        shiftedRead.insert(QStringLiteral("assembly"), shiftedAssembly);
        shiftedMethods.insert(QStringLiteral("read_codeplug"), shiftedRead);
        QJsonObject shiftedWrite = shiftedMethods.value(QStringLiteral("write_codeplug")).toObject();
        QJsonObject shiftedSource = shiftedWrite.value(QStringLiteral("source")).toObject();
        shiftedSource.insert(QStringLiteral("payload_offset"), 548);
        shiftedSource.insert(QStringLiteral("suffix_bytes"), 17);
        shiftedWrite.insert(QStringLiteral("source"), shiftedSource);
        shiftedMethods.insert(QStringLiteral("write_codeplug"), shiftedWrite);
        shifted.insert(QStringLiteral("methods"), shiftedMethods);
        RadioDefinition shiftedRadio;
        check(RadioDefinition::fromJson(QJsonDocument(shifted).toJson(), &shiftedRadio, &error),
              "Consistent alternate payload offsets were rejected");
        check(DfuExecutor::writePayload(shiftedRadio, encoded).front() == uint8_t(rdt[548]),
              "DFU write payload ignored the definition offset");
        bool rejected = false;
        try { DfuExecutor::writePayload(radio, RadioExecutor::Bytes(encoded.begin(), encoded.end() - 1)); }
        catch (const std::exception &) { rejected = true; }
        check(rejected, "Truncated DFU write image was accepted");

        QFile wrd(base + QStringLiteral(".wrd"));
        check(wrd.open(QIODevice::ReadOnly), "Cannot read MD-380 radio definition");
        QJsonObject invalid = QJsonDocument::fromJson(wrd.readAll()).object();
        QJsonObject methods = invalid.value(QStringLiteral("methods")).toObject();
        QJsonObject read = methods.value(QStringLiteral("read_codeplug")).toObject();
        QJsonArray operations = read.value(QStringLiteral("operations")).toArray();
        QJsonObject blocks = operations.at(operations.size() - 2).toObject();
        blocks.insert(QStringLiteral("block_count"), 255);
        operations.replace(operations.size() - 2, blocks);
        read.insert(QStringLiteral("operations"), operations);
        methods.insert(QStringLiteral("read_codeplug"), read);
        invalid.insert(QStringLiteral("methods"), methods);
        RadioDefinition changed;
        check(!RadioDefinition::fromJson(QJsonDocument(invalid).toJson(), &changed, &error),
              "Inconsistent DFU read length was accepted");

        QJsonObject overlapping = cs700.document();
        QJsonObject csMethods = overlapping.value(QStringLiteral("methods")).toObject();
        QJsonObject csWrite = csMethods.value(QStringLiteral("write_codeplug")).toObject();
        QJsonArray csSteps = csWrite.value(QStringLiteral("operations")).toArray();
        for (int i = 0, seen = 0; i < csSteps.size(); ++i) {
            QJsonObject step = csSteps.at(i).toObject();
            if (step.value(QStringLiteral("operation")) != QStringLiteral("write_blocks")) continue;
            if (++seen == 2) {
                step.insert(QStringLiteral("source_offset"), 0);
                csSteps.replace(i, step);
                break;
            }
        }
        csWrite.insert(QStringLiteral("operations"), csSteps);
        csMethods.insert(QStringLiteral("write_codeplug"), csWrite);
        overlapping.insert(QStringLiteral("methods"), csMethods);
        check(!RadioDefinition::fromJson(QJsonDocument(overlapping).toJson(), &changed, &error),
              "Overlapping DFU write sections were accepted");
    } catch (const std::exception &exception) {
        qCritical("%s", exception.what());
        return 1;
    }
}
