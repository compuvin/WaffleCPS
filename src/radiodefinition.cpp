#include "radiodefinition.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

namespace {
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool byteValue(const QJsonValue &value, quint8 *result)
{
    bool ok = false;
    const QString text = value.toString();
    const uint number = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
        ? text.mid(2).toUInt(&ok, 16) : text.toUInt(&ok, 10);
    if (!ok || number > 255) return false;
    *result = quint8(number);
    return true;
}

bool wordValue(const QJsonValue &value, quint16 *result)
{
    bool ok = false;
    const QString text = value.toString();
    const uint number = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
        ? text.mid(2).toUInt(&ok, 16) : text.toUInt(&ok, 10);
    if (!ok || number > 65535) return false;
    *result = quint16(number);
    return true;
}

bool hexBytes(const QJsonValue &value)
{
    static const QRegularExpression pattern(QStringLiteral("^(?:[0-9A-Fa-f]{2})(?: [0-9A-Fa-f]{2})*$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}

bool fieldReference(const QJsonValue &value)
{
    const QJsonObject object = value.toObject();
    return object.size() == 1 && !object.value(QStringLiteral("field")).toString().isEmpty();
}

bool validateWritePlan(const QJsonObject &root, const QJsonObject &methods, QString *error)
{
    const QJsonObject plan = methods.value(QStringLiteral("write_codeplug")).toObject();
    if (plan.isEmpty()) return fail(error, QStringLiteral("Radio write plan is missing."));
    if (plan.value(QStringLiteral("codeplug_definition_id")).toString() != root.value(QStringLiteral("id")).toString()
        || plan.value(QStringLiteral("representation")).toString().isEmpty())
        return fail(error, QStringLiteral("Radio write plan lacks a matching codeplug definition."));
    const int blockBytes = plan.value(QStringLiteral("block_bytes")).toInt();
    const QJsonObject regions = plan.value(QStringLiteral("regions")).toObject();
    const QJsonArray operations = plan.value(QStringLiteral("operations")).toArray();
    if (blockBytes < 1 || blockBytes > 65536 || regions.isEmpty() || operations.size() != regions.size() + 3
        || plan.value(QStringLiteral("block_source")) != QJsonValue(QStringLiteral("full_image"))
        || plan.value(QStringLiteral("require_full_block")) != QJsonValue(true))
        return fail(error, QStringLiteral("Radio write plan has invalid block size, regions, or operations."));

    QSet<QString> usedRegions;
    for (int index = 0; index < operations.size(); ++index) {
        if (!operations.at(index).isObject())
            return fail(error, QStringLiteral("Radio write operation must be an object."));
        const QJsonObject step = operations.at(index).toObject();
        const QString operation = step.value(QStringLiteral("operation")).toString();
        if (index == 0 && operation == QStringLiteral("validate_codeplug")) {
            if (step.size() != 1) return fail(error, QStringLiteral("Invalid codeplug validation operation."));
        } else if (index == 1 && operation == QStringLiteral("identify")) {
            if (step.value(QStringLiteral("method")).toString() != QStringLiteral("identify")
                || step.value(QStringLiteral("include_read_setup")) != QJsonValue(false)
                || !methods.value(QStringLiteral("identify")).isObject())
                return fail(error, QStringLiteral("Invalid write identification operation."));
        } else if (index == 2 && operation == QStringLiteral("write_setup")) {
            quint8 requestClass = 0, command = 0;
            if (!byteValue(step.value(QStringLiteral("class")), &requestClass)
                || !byteValue(step.value(QStringLiteral("command")), &command)
                || step.value(QStringLiteral("fetch_bytes")).toInt() < 1
                || step.value(QStringLiteral("poll_limit")).toInt() < 1
                || !hexBytes(step.value(QStringLiteral("expected_response_hex"))))
                return fail(error, QStringLiteral("Invalid radio write setup."));
        } else if (index >= 3 && operation == QStringLiteral("write_region")) {
            const QString region = step.value(QStringLiteral("region")).toString();
            if (!regions.contains(region) || usedRegions.contains(region))
                return fail(error, QStringLiteral("Unknown or duplicate radio write region: %1").arg(region));
            usedRegions.insert(region);
        } else {
            return fail(error, QStringLiteral("Unknown or misplaced radio write operation: %1").arg(operation));
        }
    }
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        const QJsonObject region = it.value().toObject();
        quint8 command = 0;
        const QJsonValue start = region.value(QStringLiteral("start"));
        if (!usedRegions.contains(it.key()) || !byteValue(region.value(QStringLiteral("command")), &command)
            || !((start.isDouble() && start.toInt(-1) == 0) || fieldReference(start))
            || !fieldReference(region.value(QStringLiteral("size"))))
            return fail(error, QStringLiteral("Invalid radio write region: %1").arg(it.key()));
        const QJsonValue selection = region.value(QStringLiteral("selection"));
        if (selection == QJsonValue(QStringLiteral("all_blocks"))) {
            if (region.value(QStringLiteral("extent")) != QJsonValue(QStringLiteral("region_size")))
                return fail(error, QStringLiteral("Invalid full-region extent: %1").arg(it.key()));
        } else {
            const QJsonObject selected = selection.toObject();
            const QJsonArray tables = selected.value(QStringLiteral("tables")).toArray();
            if (selected.value(QStringLiteral("operation")) != QJsonValue(QStringLiteral("table_blocks"))
                || tables.isEmpty() || selected.value(QStringLiteral("header_bytes")).toInt(-1) < 0
                || region.value(QStringLiteral("extent")) != QJsonValue(QStringLiteral("selected_records_end")))
                return fail(error, QStringLiteral("Invalid sparse-region selection: %1").arg(it.key()));
            for (const QJsonValue &table : tables)
                if (table.toString().isEmpty())
                    return fail(error, QStringLiteral("Sparse region has an unnamed table: %1").arg(it.key()));
        }
    }
    const QJsonObject request = plan.value(QStringLiteral("request")).toObject();
    quint8 requestClass = 0;
    quint16 first = 0, other = 0, final = 0;
    const QJsonObject finalRegion = request.value(QStringLiteral("final_region_selector_or")).toObject();
    const QJsonArray payload = request.value(QStringLiteral("payload")).toArray();
    if (!byteValue(request.value(QStringLiteral("class")), &requestClass)
        || !wordValue(request.value(QStringLiteral("first_selector")), &first)
        || !wordValue(request.value(QStringLiteral("other_selector_or")), &other)
        || !regions.contains(finalRegion.value(QStringLiteral("region")).toString())
        || !wordValue(finalRegion.value(QStringLiteral("value")), &final)
        || payload.size() != 4
        || payload.at(0).toObject().value(QStringLiteral("type")) != QJsonValue(QStringLiteral("u16be"))
        || payload.at(0).toObject().value(QStringLiteral("parameter")) != QJsonValue(QStringLiteral("selector"))
        || payload.at(1).toObject().value(QStringLiteral("type")) != QJsonValue(QStringLiteral("u32be"))
        || payload.at(1).toObject().value(QStringLiteral("parameter")) != QJsonValue(QStringLiteral("extent"))
        || !hexBytes(payload.at(2).toObject().value(QStringLiteral("hex")))
        || payload.at(3).toObject().value(QStringLiteral("parameter")) != QJsonValue(QStringLiteral("image_block"))
        || payload.at(3).toObject().value(QStringLiteral("bytes")).toInt() != blockBytes)
        return fail(error, QStringLiteral("Invalid radio write request format."));
    const QJsonObject ack = plan.value(QStringLiteral("acknowledgement")).toObject();
    quint16 mask = 0;
    if (ack.value(QStringLiteral("fetch_bytes")).toInt() < 1
        || ack.value(QStringLiteral("poll_limit")).toInt() < 1
        || ack.value(QStringLiteral("payload_bytes")).toInt() < 1
        || ack.value(QStringLiteral("index_offset")).toInt(-1) < 0
        || ack.value(QStringLiteral("index_offset")).toInt(-1) + 2 > ack.value(QStringLiteral("fetch_bytes")).toInt()
        || ack.value(QStringLiteral("index_type")) != QJsonValue(QStringLiteral("u16be"))
        || !wordValue(ack.value(QStringLiteral("index_mask")), &mask)
        || ack.value(QStringLiteral("duplicate")) != QJsonValue(QStringLiteral("skip_previous")))
        return fail(error, QStringLiteral("Invalid radio write acknowledgement format."));
    return true;
}
}

bool RadioDefinition::load(const QString &path, RadioDefinition *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot read radio definition: %1").arg(path));
    return fromJson(file.readAll(), out, error);
}

bool RadioDefinition::fromJson(const QByteArray &json, RadioDefinition *out, QString *error)
{
    if (!out) return fail(error, QStringLiteral("No radio definition destination."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("Invalid radio definition JSON: %1").arg(parseError.errorString()));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("definition_type")).toString() != QStringLiteral("radio_read")
        || root.value(QStringLiteral("definition_version")).toInt() != 1
        || root.value(QStringLiteral("id")).toString().isEmpty())
        return fail(error, QStringLiteral("Unsupported radio definition identity or version."));
    const QJsonObject methods = root.value(QStringLiteral("methods")).toObject();
    const QJsonObject read = methods.value(QStringLiteral("read_codeplug")).toObject();
    const QJsonArray operations = read.value(QStringLiteral("operations")).toArray();
    const QJsonObject regionCommands = methods.value(QStringLiteral("read_block")).toObject()
        .value(QStringLiteral("region_commands")).toObject();
    const int blockBytes = methods.value(QStringLiteral("read_block")).toObject()
        .value(QStringLiteral("block_bytes")).toInt();
    const QJsonObject responseFields = methods.value(QStringLiteral("read_block")).toObject()
        .value(QStringLiteral("response")).toObject().value(QStringLiteral("fields")).toObject();
    if (operations.isEmpty() || regionCommands.isEmpty() || blockBytes < 4 || blockBytes > 65536)
        return fail(error, QStringLiteral("Radio read operations or region commands are missing."));
    if (!methods.value(QStringLiteral("write_codeplug")).isNull()
        && !validateWritePlan(root, methods, error)) return false;

    QVector<RadioReadOperation> parsed;
    QSet<QString> seenRegions;
    bool identified = false;
    for (const QJsonValue &value : operations) {
        if (!value.isObject()) return fail(error, QStringLiteral("Radio read operation must be an object."));
        const QJsonObject item = value.toObject();
        const QString operation = item.value(QStringLiteral("operation")).toString();
        RadioReadOperation step;
        if (operation == QStringLiteral("identify")) {
            if (identified || !parsed.isEmpty() || item.size() != 1)
                return fail(error, QStringLiteral("Identify must be the first and only identify operation."));
            step.kind = RadioReadOperation::Kind::Identify;
            identified = true;
        } else if (operation == QStringLiteral("read_region")) {
            step.kind = RadioReadOperation::Kind::ReadRegion;
            step.region = item.value(QStringLiteral("region")).toString();
            step.sizeField = item.value(QStringLiteral("discover_size_from")).toString();
            quint8 expectedCommand = 0;
            if (!identified || !regionCommands.contains(step.region) || seenRegions.contains(step.region)
                || !byteValue(item.value(QStringLiteral("command")), &step.command)
                || !byteValue(regionCommands.value(step.region), &expectedCommand)
                || step.command != expectedCommand || !responseFields.contains(step.sizeField))
                return fail(error, QStringLiteral("Invalid or duplicate radio read region: %1").arg(step.region));
            const QJsonValue check = item.value(QStringLiteral("first_block_size_check"));
            if (!check.isUndefined()) {
                if (!check.isObject())
                    return fail(error, QStringLiteral("Invalid first-block size check for %1.").arg(step.region));
                const QJsonObject details = check.toObject();
                const QJsonValue offset = details.value(QStringLiteral("offset"));
                step.sizeCheckType = details.value(QStringLiteral("type")).toString();
                if (!offset.isDouble() || offset.toInt(-1) < 0 || offset.toInt(-1) > blockBytes - 4
                    || (step.sizeCheckType != QStringLiteral("u32le")
                        && step.sizeCheckType != QStringLiteral("u32be")))
                    return fail(error, QStringLiteral("Invalid first-block size check for %1.").arg(step.region));
                step.checkFirstBlockSize = true;
                step.sizeCheckOffset = offset.toInt();
            }
            seenRegions.insert(step.region);
        } else {
            return fail(error, QStringLiteral("Unknown radio read operation: %1").arg(operation));
        }
        parsed.append(std::move(step));
    }
    if (!identified || seenRegions.size() != regionCommands.size())
        return fail(error, QStringLiteral("Radio read plan does not cover every declared region."));
    out->m_readOperations = std::move(parsed);
    out->m_writePlan = methods.value(QStringLiteral("write_codeplug")).toObject();
    if (error) error->clear();
    return true;
}
