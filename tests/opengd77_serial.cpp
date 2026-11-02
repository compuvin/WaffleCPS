#include "../src/codeplugdefinition.h"
#include "../src/radioimage.h"

#include <QCoreApplication>
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
        const QString base = QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/opengd77-128k");
        QString error;
        RadioDefinition radio;
        CodeplugDefinition codeplug;
        check(RadioDefinition::load(base + QStringLiteral(".wrd"), &radio, &error),
              qPrintable(error));
        check(CodeplugDefinition::load(base + QStringLiteral(".wcp"), &codeplug, &error),
              qPrintable(error));
        check(radio.usesSerial() && radio.canRead() && radio.canWrite()
              && radio.compatibleWithCodeplug(codeplug.document(), &error),
              "Serial definition is not compatible with its codeplug definition");
        const QByteArray source = codeplug.defaultCodeplug();
        check(source.size() == 131072, "Unexpected packaged codeplug size");
        QByteArray downloaded(source.size(), char(0x55));
        const QByteArray assembled = assembleRadioImage(radio, source, downloaded);
        check(assembled.size() == source.size()
              && assembled.mid(0x80, 0x5fe0) == downloaded.mid(0x80, 0x5fe0)
              && assembled.mid(0x7500, 0x3b00) == downloaded.mid(0x7500, 0x3b00)
              && assembled.mid(0xb000, 0x13e60) == downloaded.mid(0xb000, 0x13e60)
              && assembled.left(0x80) == source.left(0x80)
              && assembled.mid(0x6060, 0x14a0) == source.mid(0x6060, 0x14a0)
              && assembled.mid(0x1ee60) == source.mid(0x1ee60),
              "Serial assembly changed bytes outside mapped flash regions");
        downloaded.fill(char(0));
        for (const QJsonValue &value : radio.document().value(QStringLiteral("methods")).toObject()
                 .value(QStringLiteral("read_codeplug")).toObject().value(QStringLiteral("segments")).toArray()) {
            const QJsonObject part = value.toObject();
            bool offsetOk = false, lengthOk = false;
            const int offset = part.value(QStringLiteral("image_offset")).toString().toInt(&offsetOk, 0);
            const int length = part.value(QStringLiteral("length")).toString().toInt(&lengthOk, 0);
            check(offsetOk && lengthOk, "Invalid segment in packaged definition");
            downloaded.replace(offset, length, source.constData() + offset, length);
        }
        ImportedCodeplug parsed;
        check(codeplug.read(assembleRadioImage(radio, source, downloaded), &parsed, &error),
              "Mapped serial data could not be imported through its WCP definition");
        QJsonObject altered = radio.document();
        QJsonObject methods = altered.value(QStringLiteral("methods")).toObject();
        QJsonObject write = methods.value(QStringLiteral("write_codeplug")).toObject();
        QJsonArray parts = write.value(QStringLiteral("segments")).toArray();
        QJsonObject first = parts[0].toObject();
        first.insert(QStringLiteral("radio_address"), QStringLiteral("0x100"));
        parts[0] = first;
        write.insert(QStringLiteral("segments"), parts);
        methods.insert(QStringLiteral("write_codeplug"), write);
        altered.insert(QStringLiteral("methods"), methods);
        RadioDefinition invalid;
        check(!RadioDefinition::fromJson(QJsonDocument(altered).toJson(), &invalid, &error),
              "Mismatched read and write mapping was accepted");
    } catch (const std::exception &exception) {
        qCritical("%s", exception.what());
        return 1;
    }
}
