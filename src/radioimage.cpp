#include "radioimage.h"

#include <stdexcept>

QByteArray assembleRadioImage(const RadioDefinition &definition,
                              const QByteArray &codeplugTemplate,
                              const QByteArray &downloaded)
{
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
