#include "satellitedata.h"
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void require(bool valid, const QString &message)
{
    if (!valid) throw std::runtime_error(message.toStdString());
}
quint32 number(const QJsonValue &value)
{
    bool ok = false;
    quint64 result = 0;
    if (value.isString()) {
        const QString text = value.toString();
        result = text.toULongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
    } else if (value.isDouble()) {
        const double raw = value.toDouble();
        ok = std::isfinite(raw) && raw >= 0 && raw <= 4294967295.0 && std::floor(raw) == raw;
        if (ok) result = quint64(raw);
    }
    require(ok && result <= 0xffffffffULL, "Invalid satellite definition number.");
    return quint32(result);
}
void bounds(const QByteArray &bytes, quint32 at, quint32 length)
{
    require(quint64(at) + length <= quint64(bytes.size()), "Satellite field exceeds its record or settings area.");
}
quint32 readLe(const QByteArray &bytes, quint32 at)
{
    bounds(bytes, at, 4);
    quint32 result = 0;
    for (int i = 0; i < 4; ++i) result |= quint32(quint8(bytes[at + i])) << (8 * i);
    return result;
}
void writeLe(QByteArray &bytes, quint32 at, quint32 value, int width)
{
    bounds(bytes, at, width);
    for (int i = 0; i < width; ++i) bytes[at + i] = char(value >> (8 * i));
}
QByteArray ascii(const QString &text)
{
    for (QChar ch : text) require(ch.unicode() >= 32 && ch.unicode() <= 126, "Satellite text must use printable ASCII.");
    return text.toLatin1();
}
int catalogId(const QVariant &value)
{
    QString text = value.toString().trimmed();
    if (text.endsWith('U')) text.chop(1);
    require(QRegularExpression("^[0-9]{1,5}$").match(text).hasMatch() && text.toInt() > 0,
            "Invalid satellite catalog ID.");
    return text.toInt();
}
}

QByteArray SatelliteData::encode(const QJsonObject &definition, const QVariantList &rows,
                                  const QVariantList &orbits, QVariantList *writtenIds)
{
    require(definition.value("definition_version") == 1, "Unsupported satellite definition version.");
    const quint32 capacity = number(definition.value("capacity"));
    const quint32 stride = number(definition.value("record_bytes"));
    const QJsonObject settings = definition.value("settings").toObject();
    const quint32 payloadBytes = number(settings.value("satellite_payload_bytes"));
    const quint32 fill = number(definition.value("empty_record_fill"));
    require(capacity > 0 && capacity <= 1024 && stride > 0 && fill <= 255
            && payloadBytes <= 1024 * 1024 && quint64(capacity) * stride <= payloadBytes
            && rows.size() <= capacity, "Invalid satellite capacity or record size.");
    QMap<int, QVariantMap> orbital;
    for (const auto &value : orbits) {
        const auto item = value.toMap();
        const int id = catalogId(item.value("catalogId"));
        require(!orbital.contains(id), "Duplicate downloaded satellite catalog ID.");
        orbital.insert(id, item);
    }
    const QMap<QString, int> columns{{"rx1_hz", 2}, {"tx1_hz", 3}, {"tx_ctcss_tenths_hz", 4},
        {"arming_ctcss_tenths_hz", 5}, {"rx2_hz", 6}, {"tx2_hz", 7}, {"rx3_hz", 8}, {"tx3_hz", 9}};
    const QJsonObject fields = definition.value("fields").toObject();
    require(fields.contains("name") && fields.contains("orbital_line1") && fields.contains("orbital_line2"),
            "Satellite definition lacks name or orbital fields.");
    QByteArray payload(payloadBytes, char(fill));
    QSet<int> selected;
    QVariantList encodedIds;
    for (const auto &value : rows) {
        const QVariantList row = value.toList();
        require(row.size() == 11, "Invalid satellite row.");
        const int id = catalogId(row[0]);
        require(!selected.contains(id), "Duplicate selected satellite catalog ID.");
        selected.insert(id);
        if (!orbital.contains(id)) continue;
        QByteArray record(stride, char(fill));
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const QJsonObject field = it.value().toObject();
            const quint32 at = number(field.value("offset"));
            const QString type = field.value("type").toString();
            if (it.key() == "aprs_paths") {
                const quint32 count = number(field.value("count"));
                const quint32 pathStride = number(field.value("stride"));
                const quint32 nameBytes = number(field.value("callsign_bytes"));
                const QByteArray padding = field.value("callsign_padding").toString().toLatin1();
                require(count > 0 && count <= 16 && pathStride > nameBytes && nameBytes > 0
                        && padding.size() == 1 && field.value("ssid_type") == "ascii_digit",
                        "Unsupported satellite APRS path layout.");
                require(quint64(count) * pathStride <= quint64(record.size()), "Satellite APRS paths exceed their record.");
                bounds(record, at, quint32(quint64(count) * pathStride));
                const QString pathText = row[10].toString().trimmed().toUpper();
                if (pathText.isEmpty()) continue;
                const auto paths = pathText.split(QRegularExpression("[ ,;]+"), Qt::SkipEmptyParts);
                require(paths.size() <= count, "Too many satellite APRS paths.");
                for (quint32 i = 0; i < count; ++i) {
                    QString name;
                    int ssid = 0;
                    if (i < quint32(paths.size())) {
                        const auto match = QRegularExpression("^([A-Z0-9]{1,6})(?:-([0-9]))?$").match(paths[i]);
                        require(match.hasMatch(), "Use APRS paths such as RS0ISS or ARISS-1, separated by spaces; SSIDs must be 0 to 9.");
                        name = match.captured(1);
                        ssid = match.captured(2).toInt();
                    }
                    const QByteArray bytes = ascii(name);
                    require(bytes.size() <= nameBytes, "Satellite APRS callsign exceeds its field.");
                    record.replace(at + i * pathStride, nameBytes, bytes.leftJustified(nameBytes, padding[0]));
                    record[at + i * pathStride + nameBytes] = char('0' + ssid);
                }
            } else if (type == "ascii") {
                require(it.key() == "name", "Unsupported satellite text field.");
                const quint32 width = number(field.value("bytes"));
                const quint32 pad = number(field.value("padding"));
                bounds(record, at, width);
                const QByteArray name = ascii(row[1].toString().trimmed());
                require(!name.isEmpty() && name.size() <= width && pad <= 255, "Satellite name is empty or too long.");
                record.replace(at, width, name.leftJustified(width, char(pad)));
            } else if (type == "packed_text") {
                const QString source = field.value("source").toString();
                require(source == "tle_line1" || source == "tle_line2", "Unknown satellite orbital source.");
                const QString line = orbital[id].value(source == "tle_line1" ? "line1" : "line2").toString();
                require(line.size() == 69, "Incomplete satellite TLE line.");
                const QJsonArray segments = field.value("source_segments").toArray();
                require(!segments.isEmpty(), "Satellite packed text lacks source segments.");
                QString text;
                for (const auto &segment : segments) {
                    const auto part = segment.toObject();
                    const quint32 start = number(part.value("offset")), length = number(part.value("bytes"));
                    require(quint64(start) + length <= quint64(line.size()), "Satellite TLE segment exceeds its source.");
                    text += line.mid(start, length);
                }
                const quint32 width = number(field.value("bytes"));
                bounds(record, at, width);
                require(text.size() <= quint64(width) * 2, "Packed satellite orbital field is too long.");
                const QString alphabet = field.value("alphabet").toString();
                require(alphabet.size() > 0 && alphabet.size() <= 16, "Invalid satellite packed-text alphabet.");
                const QString pad = field.value("padding").toString();
                if (text.size() < quint64(width) * 2) {
                    require(pad.size() == 1, "Packed satellite field lacks padding.");
                    text = text.leftJustified(width * 2, pad[0]);
                }
                const bool highFirst = field.value("high_nibble_first").toBool();
                for (quint32 i = 0; i < width; ++i) {
                    const int first = alphabet.indexOf(text[2 * i]), second = alphabet.indexOf(text[2 * i + 1]);
                    require(first >= 0 && second >= 0, "Satellite orbital data contains an unsupported character.");
                    record[at + i] = char(highFirst ? (first << 4) | second : (second << 4) | first);
                }
            } else if (type == "u16le" || type == "u32le") {
                require(columns.contains(it.key()), "Unknown satellite numeric field.");
                const int column = columns.value(it.key());
                bool ok = false;
                const double input = row[column].toString().toDouble(&ok);
                const double scaled = input * ((column == 4 || column == 5) ? 10.0 : 1000000.0);
                const int width = type == "u16le" ? 2 : 4;
                const double maximum = width == 2 ? 65535.0 : 4294967295.0;
                require(ok && std::isfinite(scaled) && scaled >= 0 && scaled <= maximum
                        && std::abs(scaled - std::round(scaled)) < 0.001,
                        "Satellite frequency or tone cannot be represented exactly.");
                writeLe(record, at, quint32(std::llround(scaled)), width);
            } else require(false, "Unsupported satellite field encoding.");
        }
        payload.replace(encodedIds.size() * stride, stride, record);
        encodedIds.append(id);
    }
    require(!encodedIds.isEmpty(), "None of the selected satellites has orbital data in this download. The radio was not changed.");
    *writtenIds = encodedIds;
    return payload;
}

QByteArray SatelliteData::merge(const QJsonObject &definition, const QByteArray &original,
                                 const QByteArray &payload)
{
    const QJsonObject settings = definition.value("settings").toObject();
    const quint32 bytes = number(settings.value("bytes"));
    const quint32 versionAt = number(settings.value("version_offset"));
    const quint32 version = number(settings.value("supported_version"));
    const quint32 first = number(settings.value("first_block_offset"));
    const quint32 header = number(settings.value("block_header_bytes"));
    const quint32 lengthAt = number(settings.value("block_length_offset"));
    const quint32 type = number(settings.value("satellite_block_type"));
    const quint32 empty = number(settings.value("empty_block_type"));
    const QByteArray magic = ascii(settings.value("magic").toString());
    require(original.size() == bytes && payload.size() == number(settings.value("satellite_payload_bytes"))
            && settings.value("preserve_other_blocks") == true && settings.value("reject_unknown_version") == true
            && settings.value("version_type") == "u32le" && settings.value("block_type_type") == "u32le"
            && settings.value("block_length_type") == "u32le"
            && header >= 8 && lengthAt >= 4 && lengthAt + 4 <= header
            && !magic.isEmpty() && magic.size() <= versionAt && first >= versionAt + 4,
            "Unsupported satellite settings layout.");
    QByteArray result = original;
    if (!result.startsWith(magic)) {
        result.fill(char(0xff));
        result.replace(0, magic.size(), magic);
        writeLe(result, versionAt, version, 4);
    } else require(readLe(result, versionAt) == version, "Unsupported radio additional-settings version; nothing was written.");
    quint32 found = 0, appendAt = 0;
    bool terminated = false;
    for (quint64 at = first; at + header <= bytes;) {
        const quint32 currentType = readLe(result, quint32(at));
        const quint32 length = readLe(result, quint32(at) + lengthAt);
        if (currentType == empty) { appendAt = quint32(at); terminated = true; break; }
        require(length > 0 && length != 0xffffffffU && at + header + length <= bytes,
                "Invalid radio custom-settings block; nothing was written.");
        if (currentType == type) {
            require(found == 0 && length == quint32(payload.size()), "Duplicate or unsupported satellite block size; nothing was written.");
            found = quint32(at);
        }
        at += header + length;
        if (at == bytes) terminated = true;
    }
    require(terminated, "Radio custom-settings blocks have no valid end; nothing was written.");
    if (!found) {
        require(appendAt != 0 && quint64(appendAt) + header + payload.size() + header <= bytes,
                "Not enough free space for satellite data; nothing was written.");
        found = appendAt;
        writeLe(result, found, type, 4);
        writeLe(result, found + lengthAt, payload.size(), 4);
        result.replace(found + header + payload.size(), header, QByteArray(header, char(0xff)));
    }
    result.replace(found + header, payload.size(), payload);
    return result;
}
