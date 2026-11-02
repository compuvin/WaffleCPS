#include "radioimage.h"
#include <QJsonArray>

#include <stdexcept>

QByteArray assembleRadioImage(const RadioDefinition &definition,
                              const QByteArray &codeplugTemplate,
                              const QByteArray &downloaded)
{
    if (definition.usesSerial()) {
        const QJsonObject read = definition.document().value(QStringLiteral("methods")).toObject()
            .value(QStringLiteral("read_codeplug")).toObject();
        const QJsonObject assembly = read.value(QStringLiteral("assembly")).toObject();
        const int bytes = assembly.value(QStringLiteral("bytes")).toInt(-1);
        if (assembly.value(QStringLiteral("operation")) != QStringLiteral("replace_regions_in_template")
            || bytes < 1 || codeplugTemplate.size() != bytes || downloaded.size() != bytes)
            throw std::runtime_error("Serial image or packaged codeplug template has the wrong size");
        QByteArray result = codeplugTemplate;
        for (const QJsonValue &value : read.value(QStringLiteral("segments")).toArray()) {
            const QJsonObject segment = value.toObject();
            bool offsetOk = false, lengthOk = false;
            const QString offsetText = segment.value(QStringLiteral("image_offset")).toString();
            const QString lengthText = segment.value(QStringLiteral("length")).toString();
            const int offset = offsetText.toInt(&offsetOk, 0);
            const int length = lengthText.toInt(&lengthOk, 0);
            if (!offsetOk || !lengthOk || offset < 0 || length < 1 || offset > bytes || length > bytes - offset)
                throw std::runtime_error("Serial image segment is outside the template");
            result.replace(offset, length, downloaded.constData() + offset, length);
        }
        return result;
    }
    if (!definition.usesDfu()) return downloaded;
    const QJsonObject assembly = definition.document().value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("read_codeplug")).toObject()
        .value(QStringLiteral("assembly")).toObject();
    if (assembly.value(QStringLiteral("operation")) != QStringLiteral("replace_payload_in_template"))
        throw std::runtime_error("Unsupported DFU radio image assembly");
    const int offset = assembly.value(QStringLiteral("wcp_payload_offset")).toInt(-1);
    const int bytes = assembly.value(QStringLiteral("wcp_payload_bytes")).toInt(-1);
    const int wrapper = assembly.value(QStringLiteral("wcp_wrapper_bytes")).toInt(-1);
    if (downloaded.size() != bytes || codeplugTemplate.size() != wrapper
        || offset < 0 || bytes < 1 || offset > wrapper || bytes > wrapper - offset)
        throw std::runtime_error("DFU image or packaged codeplug template has the wrong size");
    QByteArray result = codeplugTemplate;
    result.replace(offset, bytes, downloaded);
    return result;
}
