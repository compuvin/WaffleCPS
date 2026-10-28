#include "codeplugdefinition.h"
#include "ziparchive.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr qint64 maximumImage = 64 * 1024 * 1024;
constexpr qint64 maximumItems = 100000;
constexpr int maximumDepth = 64;

[[noreturn]] void reject(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void require(bool condition, const QString &message)
{
    if (!condition) reject(message);
}

qint64 integer(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double n = value.toDouble();
        require(std::isfinite(n) && std::floor(n) == n && std::abs(n) <= 9007199254740991.0,
                QStringLiteral("Expected an exact integer."));
        return qint64(n);
    }
    require(value.isString(), QStringLiteral("Expected an integer or hexadecimal string."));
    bool ok = false;
    const QString text = value.toString();
    const qint64 n = text.toLongLong(&ok, text.startsWith(QStringLiteral("0x")) ? 16 : 10);
    require(ok, QStringLiteral("Invalid integer: %1").arg(text));
    return n;
}

qint64 numeric(const QVariant &value)
{
    require(value.metaType().id() == QMetaType::LongLong,
            QStringLiteral("Expression requires an integer field."));
    return value.toLongLong();
}

qint64 add(qint64 a, qint64 b)
{
    require(!((b > 0 && a > std::numeric_limits<qint64>::max() - b)
              || (b < 0 && a < std::numeric_limits<qint64>::min() - b)),
            QStringLiteral("Integer addition overflow."));
    return a + b;
}

qint64 multiply(qint64 a, qint64 b)
{
    // Expression products used for addresses/counts are nonnegative.
    require(a >= 0 && b >= 0 && (b == 0 || a <= std::numeric_limits<qint64>::max() / b),
            QStringLiteral("Invalid or overflowing expression product."));
    return a * b;
}

QJsonObject object(const QJsonValue &value, const QString &where)
{
    require(value.isObject(), where + QStringLiteral(" must be an object."));
    return value.toObject();
}

// Validate descriptors even when a template has no selected rows in a table.
void validateSchema(const QJsonObject &definition)
{
    const auto tables = object(definition.value("tables"), QStringLiteral("tables"));
    QList<QJsonObject> maps{object(definition.value("fields"), QStringLiteral("fields"))};
    for (auto it = tables.begin(); it != tables.end(); ++it) {
        const auto table = object(it.value(), it.key());
        require(table.contains("at") && table.contains("count")
                    && (table.contains("stride_bytes") != table.contains("record_bytes")),
                QStringLiteral("Table requires an address, count and one record sizing rule: %1").arg(it.key()));
        require(!table.contains("segments"), QStringLiteral("Segmented tables are not supported in this CS7000 test engine."));
        maps.append(object(table.value("fields"), it.key() + ".fields"));
        if (table.contains("validity")) {
            const auto rule = object(table.value("validity"), QStringLiteral("validity"));
            const QString source = rule.value("index_table").toString();
            require(rule.size() == 3 && tables.contains(source) && rule.value("field").isString()
                        && integer(rule.value("index_base")) >= 0,
                    QStringLiteral("Unsupported or invalid table selection rule."));
            require(tables.value(source).toObject().value("fields").toObject().contains(rule.value("field").toString()),
                    QStringLiteral("Undefined selection index field."));
        }
        if (table.contains("join")) {
            const auto join = object(table.value("join"), QStringLiteral("join"));
            require(tables.contains(join.value("table").toString())
                        && join.value("rule").toString() == "Records with the same zero-based storage index describe the same channel.",
                    QStringLiteral("Unsupported table join rule."));
        }
    }
    for (const auto &fields : maps) {
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            const auto f = object(it.value(), it.key());
            if (f.contains("value")) continue;
            const QString type = f.value("type").toString();
            require(QStringList{"u8", "u16le", "u24le", "u32le", "ascii", "utf16le", "packed6"}.contains(type),
                    QStringLiteral("Unsupported definition field type: %1").arg(type));
            require(f.contains("at") || f.contains("offset"), QStringLiteral("Field lacks an address: %1").arg(it.key()));
            if (type == "ascii" || type == "utf16le" || type == "packed6")
                require(integer(f.value("bytes")) > 0, QStringLiteral("Field size must be positive."));
            for (const QString &key : {QStringLiteral("enum"), QStringLiteral("only_if"), QStringLiteral("reference")})
                if (f.contains(key)) object(f.value(key), key);
            if (f.contains("reference")) {
                const auto ref = f.value("reference").toObject();
                require(tables.contains(ref.value("table").toString()) && integer(ref.value("index_base")) >= 0,
                        QStringLiteral("Invalid reference definition: %1").arg(it.key()));
            }
        }
    }
    const auto layout = object(definition.value("layout_checks"), QStringLiteral("layout_checks"));
    for (const QString &key : {QStringLiteral("equal_fields"), QStringLiteral("relationships")})
        if (layout.contains(key)) require(layout.value(key).isArray(), QStringLiteral("Invalid layout checks: %1").arg(key));
    if (layout.contains("expected_addresses")) object(layout.value("expected_addresses"), QStringLiteral("expected_addresses"));
}

struct Value { QVariant raw; QVariant mapped; };
struct Scope {
    QJsonObject fields;
    qint64 base = -1;
    qint64 index = -1;
    QMap<QString, Value> values;
    QSet<QString> active;
};
struct Row { qint64 index; QMap<QString, Value> fields; };
struct Table { qint64 count = 0; QVector<Row> rows; QSet<qint64> selected; };

class Decoder
{
public:
    Decoder(const QByteArray &bytes, const QJsonObject &definition)
        : image(bytes), definition(definition)
    {
        require(!image.isEmpty() && image.size() <= maximumImage, QStringLiteral("Invalid codeplug size."));
        globals.fields = object(definition.value("fields"), QStringLiteral("fields"));
    }

    Value field(Scope &scope, const QString &name, int depth = 0)
    {
        require(depth < maximumDepth, QStringLiteral("Field dependency depth exceeded."));
        if (scope.values.contains(name)) return scope.values.value(name);
        require(!scope.active.contains(name), QStringLiteral("Cyclic field dependency: %1").arg(name));
        require(scope.fields.contains(name), QStringLiteral("Undefined field: %1").arg(name));
        scope.active.insert(name);
        Value value = readField(object(scope.fields.value(name), name), scope, depth + 1);
        scope.active.remove(name);
        scope.values.insert(name, value);
        return value;
    }

    qint64 expression(const QJsonValue &value, Scope &scope, int depth = 0)
    {
        require(depth < maximumDepth, QStringLiteral("Expression depth exceeded."));
        if (!value.isObject()) return integer(value);
        const auto rule = value.toObject();
        require(rule.size() == 1, QStringLiteral("Expression must contain one operator."));
        const QString op = rule.begin().key();
        const auto operand = rule.begin().value();
        if (op == "image_size" && operand == QJsonValue(true)) return image.size();
        if (op == "record_index" && operand == QJsonValue(true)) {
            require(scope.index >= 0, QStringLiteral("record_index outside a record."));
            return scope.index;
        }
        if (op == "field" || op == "record_field") {
            require(operand.isString() && (op == "field" || scope.base >= 0),
                    QStringLiteral("Invalid field expression."));
            return numeric(field(op == "field" ? globals : scope, operand.toString(), depth + 1).raw);
        }
        require(op == "add" || op == "subtract" || op == "multiply" || op == "max",
                QStringLiteral("Unsupported expression operator: %1").arg(op));
        require(operand.isArray(), QStringLiteral("Expression operands must be an array."));
        const auto args = operand.toArray();
        require(!args.isEmpty() && (op != "subtract" || args.size() == 2), QStringLiteral("Invalid expression operands."));
        qint64 result = expression(args.first(), scope, depth + 1);
        for (int i = 1; i < args.size(); ++i) {
            const qint64 next = expression(args[i], scope, depth + 1);
            if (op == "add") result = add(result, next);
            else if (op == "multiply") result = multiply(result, next);
            else if (op == "max") result = std::max(result, next);
            else {
                require(next != std::numeric_limits<qint64>::min(), QStringLiteral("Subtraction overflow."));
                result = add(result, -next);
            }
        }
        return result;
    }

    QByteArray range(qint64 at, qint64 size) const
    {
        require(at >= 0 && size >= 0 && at <= image.size() && size <= image.size() - at,
                QStringLiteral("Byte range at %1, length %2 exceeds %3 input bytes.").arg(at).arg(size).arg(image.size()));
        return image.mid(at, size);
    }

    void reserve(qint64 count)
    {
        require(count >= 0 && count <= maximumItems - used,
                QStringLiteral("Definition record/array limit exceeded."));
        used += count;
    }

    QString model()
    {
        const auto recognition = object(definition.value("recognition"), QStringLiteral("recognition"));
        const auto value = field(globals, recognition.value("model_field").toString()).raw;
        require(value.metaType().id() == QMetaType::QString, QStringLiteral("Model field must be text."));
        return value.toString();
    }

    void validate()
    {
        const auto recognition = definition.value("recognition").toObject();
        require(model() == recognition.value("model_value").toString(), QStringLiteral("Codeplug model does not match definition."));
        // Version is metadata, not a recognition requirement. Layout checks remain mandatory.
        for (const auto &name : globals.fields.keys()) field(globals, name);
        const auto required = recognition.value("required_fields").toObject();
        for (auto it = required.begin(); it != required.end(); ++it)
            require(field(globals, it.key()).raw == it.value().toVariant(), QStringLiteral("Required marker does not match: %1").arg(it.key()));
        const auto marker = recognition.value("optional_decoded_marker").toObject();
        if (marker.value("required").toBool())
            require(range(integer(marker.value("at")), hex(marker.value("hex")).size()) == hex(marker.value("hex")),
                    QStringLiteral("Required decoded marker does not match."));
        const auto checks = definition.value("layout_checks").toObject();
        for (const auto &entry : checks.value("equal_fields").toArray()) {
            const auto pair = entry.toArray();
            require(pair.size() == 2 && field(globals, pair[0].toString()).raw == field(globals, pair[1].toString()).raw,
                    QStringLiteral("Layout field equality does not match."));
        }
        const auto addresses = checks.value("expected_addresses").toObject();
        for (auto it = addresses.begin(); it != addresses.end(); ++it)
            require(numeric(field(globals, it.key()).raw) == integer(it.value()),
                    QStringLiteral("Unexpected address: %1").arg(it.key()));
        for (const auto &entry : checks.value("relationships").toArray()) {
            const auto rule = object(entry, QStringLiteral("relationship"));
            require(expression(rule.value("left"), globals) == expression(rule.value("right"), globals),
                    QStringLiteral("Layout relationship does not match."));
        }
        const auto definitions = object(definition.value("tables"), QStringLiteral("tables"));
        for (const auto &name : definitions.keys()) table(name);
        for (auto it = definitions.begin(); it != definitions.end(); ++it) {
            const auto schema = it.value().toObject();
            const auto source = tables.value(it.key());
            const auto fields = schema.value("fields").toObject();
            for (auto f = fields.begin(); f != fields.end(); ++f) {
                const auto reference = f.value().toObject().value("reference");
                if (reference.isUndefined()) continue;
                const auto ref = object(reference, QStringLiteral("reference"));
                const QString targetName = ref.value("table").toString();
                require(tables.contains(targetName), QStringLiteral("Undefined reference table: %1").arg(targetName));
                const auto &target = tables[targetName];
                for (const auto &row : source.rows) {
                    const auto value = row.fields.value(f.key()).raw;
                    if (!value.isValid()) continue; // A conditional field does not apply.
                    const auto values = value.metaType().id() == QMetaType::QVariantList ? value.toList() : QVariantList{value};
                    for (const auto &v : values) {
                        const qint64 number = numeric(v);
                        if (ref.contains("null_value") && number == integer(ref.value("null_value"))) continue;
                        const qint64 index = add(number, -integer(ref.value("index_base")));
                        require(index >= 0 && index < target.count && target.selected.contains(index),
                                QStringLiteral("Invalid reference in %1[%2].%3: %4").arg(it.key()).arg(row.index).arg(f.key()).arg(number));
                    }
                }
            }
            if (schema.contains("join")) {
                const auto join = schema.value("join").toObject();
                require(join.value("rule").toString() == "Records with the same zero-based storage index describe the same channel.",
                        QStringLiteral("Unsupported table join rule."));
                const QString name = join.value("table").toString();
                require(tables.contains(name) && source.selected == tables.value(name).selected,
                        QStringLiteral("Joined tables have different records."));
            }
        }
    }

    ImportedCodeplug imported()
    {
        validate();
        ImportedCodeplug result;
        result.model = model();
        const auto recognition = definition.value("recognition").toObject();
        const QString version = recognition.value("cps_version_field").toString(recognition.value("version_field").toString());
        if (!version.isEmpty()) result.cpsVersion = field(globals, version).raw.toString();
        result.radioName = field(globals, "radio_name").mapped.toString().trimmed();
        result.radioId = int(numeric(field(globals, "radio_id").raw));
        // Explicit bridge for the CS7000 definition's semantic field names.
        // No binary locations, record sizes or encodings belong in this bridge.
        for (const QString &name : {QStringLiteral("channels"), QStringLiteral("channel_names"), QStringLiteral("zones"), QStringLiteral("contacts")})
            require(tables.contains(name), QStringLiteral("Definition lacks the CS7000 editor mapping: %1").arg(name));
        const auto &channels = tables["channels"].rows;
        const auto &names = tables["channel_names"].rows;
        require(channels.size() == names.size(), QStringLiteral("Channel names and records differ in count."));
        for (qsizetype i = 0; i < channels.size(); ++i) {
            const auto &row = channels[i];
            const auto &f = row.fields;
            ImportedChannel c;
            c.name = names[i].fields.value("name").mapped.toString().trimmed();
            c.frequencyAHz = quint32(numeric(f.value("transmit_frequency").mapped));
            c.frequencyBHz = quint32(numeric(f.value("receive_frequency").mapped));
            const QString mode = f.value("mode_marker").mapped.toString();
            require(mode == "analog" || mode == "digital", QStringLiteral("Unknown channel mode."));
            c.digital = mode == "digital";
            c.colorCode = c.digital ? int(numeric(f.value("color_code").mapped)) : 0;
            c.timeSlot = c.digital ? int(numeric(f.value("time_slot").mapped)) : 0;
            c.contactIndex = c.digital ? int(numeric(f.value("contact_number").raw)) : 0;
            const QString power = f.value("power").mapped.toString();
            require(power == "low" || power == "high", QStringLiteral("Unknown channel power."));
            c.highPower = power == "high";
            c.sourceRecord = int(row.index);
            require(!c.name.isEmpty(), QStringLiteral("Channel name is empty."));
            const auto limits = definition.value("creation").toObject().value("construction").toObject().value("validate_target").toObject();
            const auto frequency = limits.value("frequency_hz").toObject();
            for (quint32 hz : {c.frequencyAHz, c.frequencyBHz})
                require(hz >= integer(frequency.value("minimum")) && hz <= integer(frequency.value("maximum")),
                        QStringLiteral("Channel frequency is outside definition limits."));
            result.channels.append(c);
        }
        for (const auto &row : tables["contacts"].rows) {
            const auto &f = row.fields;
            const QString type = f.value("call_type").mapped.toString();
            require(type == "private" || type == "group", QStringLiteral("Unknown contact call type."));
            const QString name = f.value("name").mapped.toString();
            require(!name.isEmpty(), QStringLiteral("Contact name is empty."));
            result.contacts.append({name, int(numeric(f.value("id").raw)), type == "group"});
        }
        const auto limits = definition.value("creation").toObject().value("construction").toObject().value("validate_target").toObject();
        for (const QString &name : {QStringLiteral("channels"), QStringLiteral("zones"), QStringLiteral("contacts")}) {
            const auto bound = object(limits.value(name), name + " limits");
            const qint64 count = tables.value(name).rows.size();
            require(count >= integer(bound.value("minimum")) && count <= integer(bound.value("maximum")),
                    QStringLiteral("%1 count is outside definition limits.").arg(name));
        }
        for (const auto &row : tables["zones"].rows) {
            ImportedZone z;
            z.name = row.fields.value("name").mapped.toString().trimmed();
            const auto members = row.fields.value("members").raw.toList();
            require(!z.name.isEmpty() && members.size() <= integer(limits.value("zone_members_maximum")),
                    QStringLiteral("Invalid zone name or member count."));
            const auto ref = definition.value("tables").toObject().value("zones").toObject().value("fields").toObject()
                                 .value("members").toObject().value("reference").toObject();
            for (const auto &member : members) z.channelIndexes.append(int(numeric(member) - integer(ref.value("index_base"))));
            result.zones.append(z);
        }
        return result;
    }

    static QByteArray hex(const QJsonValue &value)
    {
        require(value.isString(), QStringLiteral("Expected hexadecimal bytes."));
        QString text = value.toString();
        text.remove(QRegularExpression(QStringLiteral("\\s")));
        require(text.size() % 2 == 0 && QRegularExpression(QStringLiteral("^[0-9a-fA-F]*$")).match(text).hasMatch(),
                QStringLiteral("Invalid hexadecimal bytes."));
        return QByteArray::fromHex(text.toLatin1());
    }

private:
    Value scalar(const QJsonObject &schema, qint64 at)
    {
        const QString type = schema.value("type").toString();
        qint64 size = 0;
        if (type == "u8") size = 1;
        else if (type == "u16le") size = 2;
        else if (type == "u24le") size = 3;
        else if (type == "u32le") size = 4;
        else if (type == "ascii" || type == "utf16le" || type == "packed6") size = integer(schema.value("bytes"));
        else reject(QStringLiteral("Unsupported definition field type: %1").arg(type));
        require(size > 0, QStringLiteral("Field size must be positive."));
        const QByteArray bytes = range(at, size);
        Value result;
        if (type.startsWith('u') && type != "utf16le") {
            quint64 value = 0;
            for (qsizetype i = 0; i < bytes.size(); ++i) value |= quint64(quint8(bytes[i])) << (8 * i);
            const qint64 mask = schema.contains("mask") ? integer(schema.value("mask")) : qint64((quint64(1) << (size * 8)) - 1);
            const qint64 shift = schema.contains("shift") ? integer(schema.value("shift")) : 0;
            require(mask >= 0 && quint64(mask) < (quint64(1) << (size * 8)) && shift >= 0 && shift < size * 8,
                    QStringLiteral("Invalid integer mask or shift."));
            result.raw = qint64((value & quint64(mask)) >> shift);
        } else if (type == "packed6") {
            const QString alphabet = schema.value("alphabet").toString();
            const qint64 count = integer(schema.value("characters"));
            require(alphabet.size() == 64 && count >= 0 && count <= size * 8 / 6, QStringLiteral("Invalid packed6 schema."));
            QString text;
            for (qint64 i = 0; i < count; ++i) {
                int code = 0;
                for (int j = 0; j < 6; ++j) {
                    const qint64 bit = i * 6 + j;
                    code = (code << 1) | ((quint8(bytes[bit / 8]) >> (7 - bit % 8)) & 1);
                }
                text.append(alphabet[code]);
            }
            result.raw = text;
        } else {
            require(type != "utf16le" || size % 2 == 0, QStringLiteral("Odd UTF-16LE storage size."));
            QSet<qint64> terminators{0};
            if (schema.contains("terminators")) {
                require(schema.value("terminators").isArray(), QStringLiteral("Invalid terminators."));
                terminators.clear();
                for (const auto &v : schema.value("terminators").toArray()) terminators.insert(integer(v));
            }
            QString text;
            bool terminated = false;
            for (qsizetype i = 0; i < bytes.size(); i += type == "ascii" ? 1 : 2) {
                const ushort code = type == "ascii" ? quint8(bytes[i]) : quint8(bytes[i]) | (quint16(quint8(bytes[i + 1])) << 8);
                if (terminators.contains(code)) { terminated = true; break; }
                require(type != "ascii" || code < 128, QStringLiteral("Invalid ASCII text."));
                text.append(QChar(code));
            }
            require(!schema.value("require_terminator").toBool() || terminated, QStringLiteral("Required text terminator is missing."));
            require(text.isValidUtf16(), QStringLiteral("Invalid UTF-16 text."));
            result.raw = text;
        }
        if (result.raw.metaType().id() == QMetaType::LongLong) {
            const qint64 n = numeric(result.raw);
            require(!schema.contains("minimum") || n >= integer(schema.value("minimum")), QStringLiteral("Value violates field minimum."));
            require(!schema.contains("maximum") || n <= integer(schema.value("maximum")), QStringLiteral("Value violates field maximum."));
        }
        result.mapped = result.raw;
        const auto enums = schema.value("enum").toObject();
        const auto e = enums.value(result.raw.toString());
        if (!e.isUndefined()) result.mapped = e.isDouble() ? QVariant(integer(e)) : e.toVariant();
        if (schema.contains("scale")) result.mapped = multiply(numeric(result.mapped), integer(schema.value("scale")));
        if (schema.contains("trim")) {
            const QString trim = schema.value("trim").toString();
            require(trim == "ascii_spaces_both_ends" || trim == "Remove ASCII spaces from both ends of the decoded display string; retain all original packed bits.",
                    QStringLiteral("Unsupported text trim rule."));
            QString text = result.mapped.toString();
            while (text.startsWith(' ')) text.remove(0, 1);
            while (text.endsWith(' ')) text.chop(1);
            result.mapped = text;
        }
        return result;
    }

    Value readField(const QJsonObject &schema, Scope &scope, int depth)
    {
        if (schema.contains("only_if")) {
            const auto rule = object(schema.value("only_if"), QStringLiteral("only_if"));
            const qint64 value = expression(rule.value("value"), scope, depth + 1);
            bool present = false;
            require(rule.value("in").isArray(), QStringLiteral("Invalid conditional field."));
            for (const auto &v : rule.value("in").toArray()) if (integer(v) == value) present = true;
            if (!present) return {};
        }
        if (schema.contains("value")) {
            const qint64 value = expression(schema.value("value"), scope, depth + 1);
            return {value, value};
        }
        require(schema.contains("at") || scope.base >= 0, QStringLiteral("Field has no address."));
        const qint64 at = schema.contains("at") ? expression(schema.value("at"), scope, depth + 1)
                                              : add(scope.base, expression(schema.value("offset"), scope, depth + 1));
        if (!schema.contains("count")) return scalar(schema, at);
        const qint64 count = expression(schema.value("count"), scope, depth + 1);
        const qint64 stride = expression(schema.value("stride_bytes"), scope, depth + 1);
        require(stride > 0, QStringLiteral("Array stride must be positive."));
        reserve(count);
        QVariantList raw, mapped;
        for (qint64 i = 0; i < count; ++i) {
            const auto value = scalar(schema, add(at, multiply(i, stride)));
            raw.append(value.raw); mapped.append(value.mapped);
        }
        if (!count) range(at, 0);
        return {raw, mapped};
    }

    void table(const QString &name, int depth = 0)
    {
        if (tables.contains(name)) return;
        require(depth < maximumDepth && !activeTables.contains(name), QStringLiteral("Cyclic or excessive table dependencies."));
        const auto schemas = definition.value("tables").toObject();
        const auto schema = object(schemas.value(name), name);
        require(!schema.contains("segments"), QStringLiteral("Segmented tables are not supported in this CS7000 test engine."));
        activeTables.insert(name);
        Table decoded;
        decoded.count = expression(schema.value("count"), globals);
        reserve(decoded.count);
        qint64 cursor = expression(schema.value("at"), globals);
        const bool fixed = schema.contains("stride_bytes");
        const qint64 stride = fixed ? expression(schema.value("stride_bytes"), globals) : 0;
        const qint64 span = fixed ? (schema.contains("logical_span_bytes") ? integer(schema.value("logical_span_bytes")) : stride) : 0;
        if (fixed) {
            require(stride > 0 && span > 0, QStringLiteral("Invalid table stride/span."));
            range(cursor, decoded.count ? add(multiply(decoded.count - 1, stride), span) : 0);
        }
        QSet<qint64> selected;
        const bool filtered = schema.contains("validity");
        if (filtered) {
            require(fixed, QStringLiteral("Selected tables require fixed strides."));
            const auto validity = object(schema.value("validity"), QStringLiteral("validity"));
            require(validity.contains("index_table"), QStringLiteral("Unsupported table selection rule."));
            const QString source = validity.value("index_table").toString();
            table(source, depth + 1);
            for (const auto &row : tables.value(source).rows) {
                const qint64 index = add(numeric(row.fields.value(validity.value("field").toString()).raw),
                                        -integer(validity.value("index_base")));
                require(index >= 0 && index < decoded.count && !selected.contains(index), QStringLiteral("Invalid or duplicate selection index."));
                selected.insert(index);
            }
        }
        for (qint64 i = 0; i < decoded.count; ++i) {
            if (filtered && !selected.contains(i)) { cursor = add(cursor, stride); continue; }
            Scope scope{object(schema.value("fields"), name + ".fields"), cursor, i, {}, {}};
            for (const auto &key : scope.fields.keys()) field(scope, key);
            const qint64 step = fixed ? stride : expression(schema.value("record_bytes"), scope);
            require(step > 0, QStringLiteral("Invalid variable record length."));
            range(cursor, fixed ? span : step);
            decoded.rows.append({i, scope.values});
            decoded.selected.insert(i);
            cursor = add(cursor, step);
        }
        tables.insert(name, decoded);
        activeTables.remove(name);
    }

    const QByteArray &image;
    const QJsonObject &definition;
    Scope globals;
    qint64 used = 0;
    QMap<QString, Table> tables;
    QSet<QString> activeTables;
};

QByteArray decode(const QByteArray &input, const QJsonObject &representation)
{
    require(!input.isEmpty() && input.size() <= maximumImage, QStringLiteral("Invalid codeplug size."));
    const auto rule = object(representation.value("decode"), QStringLiteral("representation.decode"));
    const auto range = [&](qint64 at, qint64 size) {
        require(at >= 0 && size >= 0 && at <= input.size() && size <= input.size() - at, QStringLiteral("Encoding range exceeds input."));
        return input.mid(at, size);
    };
    for (const auto &entry : rule.value("input_equal_ranges").toArray()) {
        const auto pair = object(entry, QStringLiteral("equal input ranges"));
        const qint64 size = integer(pair.value("bytes"));
        require(range(integer(pair.value("first_offset")), size) == range(integer(pair.value("second_offset")), size),
                QStringLiteral("Encoding ranges do not match."));
    }
    const QString operation = rule.value("operation").toString();
    if (operation == "identity") return input;
    require(operation == "repeating_xor", QStringLiteral("Unsupported representation operation: %1").arg(operation));
    const qint64 size = integer(rule.value("mask_bytes"));
    require(size > 0, QStringLiteral("XOR mask must not be empty."));
    const QByteArray mask = range(integer(rule.value("mask_input_offset")), size);
    qint64 phase = rule.contains("mask_phase_at_input_zero") ? integer(rule.value("mask_phase_at_input_zero")) % size : 0;
    if (phase < 0) phase += size;
    QByteArray result(input.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < input.size(); ++i) result[i] = input[i] ^ mask[(i % size + phase) % size];
    return result;
}

template<class Operation> bool checked(QString *error, Operation operation)
{
    try {
        operation();
        if (error) error->clear();
        return true;
    } catch (const std::exception &exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return false;
    }
}
} // namespace

QString CodeplugDefinition::id() const { return m_definition.value("id").toString(); }
QString CodeplugDefinition::modelName() const { return m_definition.value("recognition").toObject().value("model_value").toString(); }
QByteArray CodeplugDefinition::defaultCodeplug() const { return m_defaultCodeplug; }

QStringList CodeplugDefinition::extensions() const
{
    QStringList result;
    for (const auto &r : m_definition.value("representations").toArray()) {
        const QString extension = r.toObject().value("extension").toString();
        if (!extension.isEmpty() && !result.contains(extension)) result.append(extension);
    }
    return result;
}

bool CodeplugDefinition::load(const QString &path, CodeplugDefinition *result, QString *error)
{
    return checked(error, [&] {
        require(result, QStringLiteral("Missing definition output."));
        require(QFileInfo(path).size() <= maximumImage, QStringLiteral("Definition package exceeds size limit."));
        QMap<QString, QByteArray> entries;
        QString zipError;
        require(ZipArchive::read(path, &entries, &zipError), QStringLiteral("Could not load definition package: %1").arg(zipError));
        qint64 total = 0;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            require(!it.key().startsWith('/') && !it.key().contains('\\') && !it.key().contains(':')
                        && !it.key().split('/').contains("..") && !it.key().split('/').contains("."),
                    QStringLiteral("Unsafe definition member name."));
            total += it.value().size();
            require(total <= maximumImage, QStringLiteral("Definition contents exceed size limit."));
        }
        require(entries.contains("definition.json") && entries.value("definition.json").size() <= 4 * 1024 * 1024,
                QStringLiteral("Missing or oversized definition.json."));
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(entries.value("definition.json"), &parseError);
        require(parseError.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("Invalid definition JSON: %1").arg(parseError.errorString()));
        CodeplugDefinition loaded;
        loaded.m_definition = document.object();
        const auto &d = loaded.m_definition;
        require(d.value("definition_type") == QJsonValue("codeplug") && integer(d.value("definition_version")) == 2,
                QStringLiteral("Expected a packaged version 2 codeplug definition."));
        require(!loaded.id().isEmpty() && !loaded.modelName().isEmpty(), QStringLiteral("Definition lacks its ID or model name."));
        require(d.value("fields").isObject() && d.value("tables").isObject() && d.value("recognition").isObject()
                    && d.value("representations").isArray() && !d.value("representations").toArray().isEmpty(),
                QStringLiteral("Definition lacks fields, tables, recognition or representations."));
        validateSchema(d);
        const auto creation = object(d.value("creation"), QStringLiteral("creation"));
        const QString name = creation.value("template").toString();
        require(!name.isEmpty() && entries.contains(name), QStringLiteral("Default template is missing."));
        loaded.m_defaultCodeplug = entries.value(name);
        require(integer(creation.value("template_bytes")) == loaded.m_defaultCodeplug.size(), QStringLiteral("Default template size does not match."));
        require(QCryptographicHash::hash(loaded.m_defaultCodeplug, QCryptographicHash::Sha256).toHex()
                    == creation.value("template_sha256").toString().toLatin1(), QStringLiteral("Default template checksum does not match."));
        bool found = false;
        QSet<QString> names;
        for (const auto &entry : d.value("representations").toArray()) {
            const auto r = object(entry, QStringLiteral("representation"));
            const QString representationName = r.value("name").toString();
            require(!representationName.isEmpty() && !names.contains(representationName), QStringLiteral("Invalid or duplicate representation name."));
            names.insert(representationName);
            if (r.contains("extension")) require(QRegularExpression(QStringLiteral("^\\.[A-Za-z0-9]+$")).match(r.value("extension").toString()).hasMatch(),
                                                 QStringLiteral("Invalid filename extension."));
            const auto rule = object(r.value("decode"), QStringLiteral("decode"));
            require(rule.value("operation") == QJsonValue("identity") || rule.value("operation") == QJsonValue("repeating_xor"),
                    QStringLiteral("Unsupported representation operation."));
            if (representationName == creation.value("representation").toString()) {
                const auto image = decode(loaded.m_defaultCodeplug, r);
                Decoder(image, d).imported(); // Validate schema and template together before publishing the object.
                found = true;
            }
        }
        require(found, QStringLiteral("Template representation is undefined."));
        *result = std::move(loaded);
    });
}

bool CodeplugDefinition::matchesModel(const QByteArray &input, QString *error) const
{
    return checked(error, [&] {
        QStringList reasons;
        for (const auto &r : m_definition.value("representations").toArray()) {
            try {
                const auto image = decode(input, r.toObject());
                if (Decoder(image, m_definition).model() == modelName()) return;
            } catch (const std::exception &e) { reasons.append(QString::fromUtf8(e.what())); }
        }
        reject(QStringLiteral("No representation matches model %1. %2").arg(modelName(), reasons.join(' ')));
    });
}

bool CodeplugDefinition::read(const QByteArray &input, ImportedCodeplug *result, QString *error) const
{
    return checked(error, [&] {
        require(result, QStringLiteral("Missing codeplug output."));
        QSet<QByteArray> decodedImages;
        QStringList reasons;
        std::optional<ImportedCodeplug> parsed;
        for (const auto &r : m_definition.value("representations").toArray()) {
            try {
                const auto image = decode(input, r.toObject());
                if (Decoder(image, m_definition).model() != modelName()) continue;
                const auto digest = QCryptographicHash::hash(image, QCryptographicHash::Sha256);
                if (decodedImages.contains(digest)) continue;
                auto candidate = Decoder(image, m_definition).imported();
                decodedImages.insert(digest);
                if (!parsed) parsed = std::move(candidate);
            } catch (const std::exception &e) { reasons.append(QString::fromUtf8(e.what())); }
        }
        require(parsed.has_value(), QStringLiteral("Could not decode %1: %2").arg(modelName(), reasons.join(' ')));
        require(decodedImages.size() == 1, QStringLiteral("Ambiguous representations decode to different valid images."));
        *result = std::move(*parsed);
    });
}
