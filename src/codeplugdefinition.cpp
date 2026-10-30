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
#include <cstdio>
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
        maps.append(object(table.value("fields"), it.key() + ".fields"));
        require(table.contains("count"), "Table lacks count: " + it.key());
        QJsonArray segments{table};
        if (table.contains("segments")) {
            require(table.value("segments").isArray() && !table.value("segments").toArray().isEmpty(),
                    "Table segments must be a nonempty array: " + it.key());
            segments = table.value("segments").toArray();
        }
        for (const auto &entry : segments) {
            const auto segment = object(entry, it.key() + ".segment");
            require(segment.contains("at") && segment.contains("count")
                        && (segment.contains("stride_bytes") != table.contains("record_bytes")),
                    "Table segment requires an address, count and one sizing rule: " + it.key());
            if (!segment.contains("validity")) continue;
            const auto rule = object(segment.value("validity"), it.key() + ".validity");
            const int selectors = int(rule.contains("index_table")) + int(rule.contains("bitmap_at"))
                + int(rule.contains("not_hex"));
            require(selectors == 1, "Table validity requires exactly one selection rule: " + it.key());
            if (rule.contains("index_table")) {
                const QString source = rule.value("index_table").toString();
                require(rule.size() == 3 && tables.contains(source) && rule.value("field").isString()
                            && integer(rule.value("index_base")) >= 0,
                        "Invalid table index selection: " + it.key());
                require(tables.value(source).toObject().value("fields").toObject().contains(rule.value("field").toString()),
                        "Undefined selection index field: " + it.key());
            } else if (rule.contains("bitmap_at")) {
                const QString order = rule.value("bit_order").toString("lsb_first");
                require(order == "lsb_first" || order == "msb_first", "Unknown bitmap bit order: " + it.key());
                require(rule.size() <= 2, "Unknown bitmap validity property: " + it.key());
            } else {
                require(rule.value("not_hex").isArray() && !rule.value("not_hex").toArray().isEmpty()
                            && integer(rule.value("bytes")) > 0 && rule.size() <= 3,
                        "Invalid selection probe: " + it.key());
            }
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
            require(QStringList{"u8", "u16le", "u24le", "u32le", "u16be", "u24be", "u32be",
                                "bcdle", "bcdbe", "ascii", "utf16le", "packed6"}.contains(type),
                    QStringLiteral("Unsupported definition field type: %1").arg(type));
            require(f.contains("at") || f.contains("offset"), QStringLiteral("Field lacks an address: %1").arg(it.key()));
            if (type == "ascii" || type == "utf16le" || type == "packed6" || type.startsWith("bcd"))
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
    bool providedOnly = false;
};
struct Row { qint64 index; qint64 address; QMap<QString, Value> fields; };
struct Table { qint64 count = 0; QVector<Row> rows; QSet<qint64> selected; };

QString operationName(const QString &label, const QJsonObject &rule)
{
    if (rule.contains("operation")) {
        require(rule.value("operation").isString() && !rule.value("operation").toString().isEmpty(),
                "Invalid construction operation: " + label);
        return rule.value("operation").toString();
    }
    // The version-1 target validator is the one reserved, implicit command.
    require(label == "validate_target", "Construction step lacks operation: " + label);
    return "validate_target";
}

QJsonObject constructionRule(const QJsonObject &definition, const QString &operation)
{
    const auto plan = object(definition.value("creation").toObject().value("construction"), "creation.construction");
    require(plan.contains("version") && plan.value("order").isArray(),
            "creation.construction lacks executable writer version/operation order; editor bindings cannot be established from reset instructions.");
    QJsonObject result;
    for (const auto &entry : plan.value("order").toArray()) {
        const auto rule = object(plan.value(entry.toString()), "construction." + entry.toString());
        if (operationName(entry.toString(), rule) != operation) continue;
        require(result.isEmpty(), "Ambiguous construction operation: " + operation);
        result = rule;
    }
    require(!result.isEmpty(), "Definition does not declare required editor operation: " + operation);
    return result;
}

struct FieldBinding { QString table; QString field; };

// Bind the editor's semantic values only through explicit construction writes.
// Storage/table names are supplied by the definition, never by radio identity.
class EditorBindings
{
public:
    explicit EditorBindings(const QJsonObject &definition)
    {
        // The target value names state editor meaning. Operations state how
        // bytes are written, and do not identify channel/contact/zone roles.
        const auto plan = object(definition.value("creation").toObject().value("construction"), "creation.construction");
        require(plan.contains("version") && plan.value("order").isArray(),
                "creation.construction lacks executable writer version/operation order; editor bindings cannot be established from reset instructions.");
        require(integer(plan.value("version")) == 1, "Unsupported editor construction version.");
        for (const auto &entry : plan.value("order").toArray()) {
            const auto rule = object(plan.value(entry.toString()), "construction." + entry.toString());
            const auto operation = operationName(entry.toString(), rule);
            if (operation == "write_global_fields") {
                collect({}, rule.value("writes").toArray());
            } else if (operation == "rebuild_fixed_table" || operation == "prepare_overlapping_table") {
                const QString table = rule.value("records_table").toString();
                require(!table.isEmpty(), "Construction record write lacks records_table.");
                collect(table, rule.value("writes").toArray());
                collect(table, rule.value("initialize").toObject().value("active_writes").toArray());
                const auto index = rule.value("index").toObject();
                if (rule.contains("index_table")) {
                    const FieldBinding value{rule.value("index_table").toString(), index.value("field").toString()};
                    if (indexes.contains(table)) {
                        const auto previous = indexes.value(table);
                        require(previous.table == value.table && previous.field == value.field,
                                "Conflicting record-order indexes for table: " + table);
                    }
                    indexes.insert(table, value);
                }
            } else if (operation == "replace_variable_table" || operation == "write_overlapping_fields") {
                const QString table = rule.value("table").toString();
                require(!table.isEmpty(), "Construction record write lacks table.");
                collect(table, rule.value("fields").toArray());
                collect(table, rule.value("writes").toArray());
            }
        }
        const auto defaults = definition.value("creation").toObject().value("channel_defaults");
        if (!defaults.isUndefined()) {
            const auto declared = object(defaults, "creation.channel_defaults");
            const QString table = declared.value("table").toString();
            require(!table.isEmpty(), "Channel defaults lack table.");
            bind(table, "mode_marker", {table, declared.value("mode_field").toString()});
        }
        const QMap<QString, QStringList> required{
            {"radio", {"radio_name", "radio_id"}},
            {"channels", {"name", "transmit_frequency", "receive_frequency", "mode_marker", "color_code", "time_slot", "contact_number"}},
            {"contacts", {"name", "id", "call_type"}}, {"zones", {"name", "members"}}};
        for (auto role = required.begin(); role != required.end(); ++role) {
            QStringList candidates;
            for (auto group = groups.begin(); group != groups.end(); ++group) {
                if ((role.key() == "radio") != group.key().isEmpty()) continue;
                bool complete = true;
                for (const auto &semantic : role.value()) complete &= group.value().contains(semantic);
                if (complete) candidates.append(group.key());
            }
            require(candidates.size() == 1,
                    "Definition must declare exactly one storage binding for editor role: " + role.key()
                    + (candidates.isEmpty() ? " (required target semantics: " + role.value().join(", ") + ")."
                                           : " (ambiguous tables: " + candidates.join(", ") + ")."));
            const QString table = candidates.first();
            bindings.insert(role.key(), groups.value(table));
            if (!table.isEmpty()) {
                require(!roles.contains(table), "A storage table is bound to more than one editor role: " + table);
                roles.insert(table, role.key());
            }
        }
        channels = tableForRole("channels");
        contacts = tableForRole("contacts");
        zones = tableForRole("zones");
        const auto tables = definition.value("tables").toObject();
        for (auto role = bindings.begin(); role != bindings.end(); ++role) {
            for (auto it = role.value().begin(); it != role.value().end(); ++it) {
                const auto binding = it.value();
                const auto fields = binding.table.isEmpty() ? definition.value("fields").toObject()
                    : tables.value(binding.table).toObject().value("fields").toObject();
                require(!binding.field.isEmpty() && fields.contains(binding.field),
                        "Undefined editor binding " + role.key() + "." + it.key() + ": " + binding.table + "." + binding.field);
                require((role.key() == "radio") == binding.table.isEmpty(), "Global/record editor binding scope mismatch.");
            }
        }
        limits = constructionRule(definition, "validate_target");
    }

    FieldBinding get(const QString &role, const QString &semantic) const
    {
        require(bindings.value(role).contains(semantic), "Definition lacks editor binding: " + role + "." + semantic);
        return bindings.value(role).value(semantic);
    }
    bool contains(const QString &role, const QString &semantic) const { return bindings.value(role).contains(semantic); }
    QString roleForTable(const QString &table) const
    {
        require(roles.contains(table), "Storage table lacks unambiguous editor target semantics: " + table);
        return roles.value(table);
    }
    FieldBinding indexForTable(const QString &table) const { return indexes.value(table); }
    QString channels, zones, contacts;
    QJsonObject limits;

private:
    QString tableForRole(const QString &role) const
    {
        for (auto it = roles.begin(); it != roles.end(); ++it) if (it.value() == role) return it.key();
        reject("Definition lacks storage table for role: " + role);
    }
    void bind(const QString &group, const QString &semantic, const FieldBinding &binding)
    {
        require(!semantic.isEmpty(), "Editor write lacks target semantic.");
        if (groups[group].contains(semantic)) {
            const auto previous = groups[group].value(semantic);
            require(previous.table == binding.table && previous.field == binding.field,
                    "Conflicting editor bindings: " + group + "." + semantic);
        }
        groups[group].insert(semantic, binding);
    }
    void collect(const QString &table, const QJsonArray &writes)
    {
        for (const auto &entry : writes) {
            const auto write = object(entry, "editor write");
            if (!write.contains("from_target")) continue;
            bind(table, write.value("from_target").toString(),
                 {write.value("table").toString(table), write.value("field").toString()});
        }
    }
    QMap<QString, QMap<QString, FieldBinding>> groups, bindings;
    QMap<QString, QString> roles;
    QMap<QString, FieldBinding> indexes;
};

class Decoder
{
public:
    Decoder(const QByteArray &bytes, const QJsonObject &definition)
        : image(bytes), definition(definition)
    {
        require(!image.isEmpty() && image.size() <= maximumImage, QStringLiteral("Invalid codeplug size."));
        globals.fields = object(definition.value("fields"), QStringLiteral("fields"));
    }

    Value global(const QString &name) { return field(globals, name); }
    Value scalarValue(const QJsonObject &schema, qint64 at) { return scalar(schema, at); }
    qint64 evaluate(const QJsonValue &value) { return expression(value, globals); }
    const Table &records(const QString &name) { table(name); return tables[name]; }
    QVector<qint64> recordOrder(const QString &name, const FieldBinding &index)
    {
        table(name);
        QVector<qint64> result;
        if (index.table.isEmpty()) {
            for (const auto &row : tables[name].rows) result.append(row.index);
            return result;
        }
        const auto reference = definition.value("tables").toObject().value(index.table).toObject()
            .value("fields").toObject().value(index.field).toObject().value("reference").toObject();
        require(reference.value("table").toString() == name,
                "Record-order index must declare a reference to its record table: " + index.table);
        table(index.table);
        QSet<qint64> seen;
        for (const auto &row : tables[index.table].rows) {
            const qint64 value = numeric(row.fields.value(index.field).raw);
            require(!reference.contains("null_value") || value != integer(reference.value("null_value")),
                    "Record-order indexes cannot contain null entries.");
            const qint64 physical = add(value, -integer(reference.value("index_base")));
            require(tables[name].selected.contains(physical) && !seen.contains(physical),
                    "Invalid or duplicate record-order index: " + index.table);
            seen.insert(physical); result.append(physical);
        }
        return result;
    }
    Scope recordScope(const QString &name, qint64 at, qint64 index)
    {
        return {object(definition.value("tables").toObject().value(name).toObject().value("fields"), name + ".fields"),
                at, index, {}, {}};
    }

    Value field(Scope &scope, const QString &name, int depth = 0)
    {
        require(depth < maximumDepth, QStringLiteral("Field dependency depth exceeded."));
        if (scope.values.contains(name)) return scope.values.value(name);
        require(!scope.active.contains(name), QStringLiteral("Cyclic field dependency: %1").arg(name));
        require(scope.fields.contains(name), QStringLiteral("Undefined field: %1").arg(name));
        require(!scope.providedOnly || scope.fields.value(name).toObject().contains("value"),
                "Record-size expression needs a target value not declared by its writes: " + name);
        scope.active.insert(name);
        Value value;
        try {
            value = readField(object(scope.fields.value(name), name), scope, depth + 1);
        } catch (const std::exception &exception) {
            scope.active.remove(name);
            reject(QStringLiteral("Field %1: %2").arg(name, QString::fromUtf8(exception.what())));
        }
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
        if (op == "span_count") {
            const auto scan = object(operand, "span_count");
            const qint64 start = expression(scan.value("at"), scope, depth + 1);
            const qint64 end = expression(scan.value("end"), scope, depth + 1);
            const qint64 stride = integer(scan.value("stride_bytes"));
            const qint64 probe = scan.contains("probe_offset") ? integer(scan.value("probe_offset")) : 0;
            require(stride > 0 && probe >= 0 && end >= start, "Invalid span_count range/stride.");
            range(start, end - start);
            QVector<QByteArray> empty;
            if (scan.contains("empty_hex_patterns")) {
                require(!scan.contains("empty_hex") && scan.value("empty_hex_patterns").isArray(),
                        "span_count requires one empty-pattern specification.");
                for (const auto &pattern : scan.value("empty_hex_patterns").toArray()) empty.append(hex(pattern));
            } else empty.append(hex(scan.value("empty_hex")));
            require(!empty.isEmpty() && !empty.first().isEmpty(), "span_count has no empty patterns.");
            const qint64 size = empty.first().size();
            require(size <= stride && probe <= stride - size, "span_count probe exceeds stride.");
            for (const auto &pattern : empty) require(pattern.size() == size, "span_count patterns differ in size.");
            const qint64 count = (end - start) / stride;
            reserve(count);
            qint64 span = 0;
            for (qint64 i = 0; i < count; ++i)
                if (!empty.contains(range(add(add(start, multiply(i, stride)), probe), size))) span = i + 1;
            return span;
        }
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

    void recognize()
    {
        const auto recognition = definition.value("recognition").toObject();
        require(model() == recognition.value("model_value").toString(), QStringLiteral("Codeplug model does not match definition."));
        const auto required = recognition.value("required_fields").toObject();
        for (auto it = required.begin(); it != required.end(); ++it)
            require(field(globals, it.key()).raw == it.value().toVariant(), QStringLiteral("Required marker does not match: %1").arg(it.key()));
        const auto marker = recognition.value("optional_decoded_marker").toObject();
        if (marker.value("required").toBool())
            require(range(integer(marker.value("at")), hex(marker.value("hex")).size()) == hex(marker.value("hex")),
                    QStringLiteral("Required decoded marker does not match."));
    }

    void validate()
    {
        recognize();
        // Version is metadata; structural checks remain mandatory.
        for (const auto &name : globals.fields.keys()) field(globals, name);
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

    QJsonObject inspection()
    {
        validate();
        QJsonObject fields, raw, decodedTables;
        for (auto it = globals.values.begin(); it != globals.values.end(); ++it) {
            if (!it.value().raw.isValid()) continue;
            fields.insert(it.key(), QJsonValue::fromVariant(it.value().mapped));
            raw.insert(it.key(), QJsonValue::fromVariant(it.value().raw));
        }
        for (auto it = tables.begin(); it != tables.end(); ++it) {
            QJsonArray rows;
            for (const auto &row : it.value().rows) {
                QJsonObject values, rawValues;
                for (auto f = row.fields.begin(); f != row.fields.end(); ++f) {
                    if (!f.value().raw.isValid()) continue;
                    values.insert(f.key(), QJsonValue::fromVariant(f.value().mapped));
                    rawValues.insert(f.key(), QJsonValue::fromVariant(f.value().raw));
                }
                rows.append(QJsonObject{{"index", row.index}, {"address", row.address},
                                        {"fields", values}, {"raw_values", rawValues}});
            }
            decodedTables.insert(it.key(), QJsonObject{{"count", it.value().count}, {"records", rows}});
        }
        return {{"fields", fields}, {"raw_fields", raw}, {"tables", decodedTables}};
    }

    ImportedCodeplug imported()
    {
        validate();
        const EditorBindings editor(definition);
        const auto tablesSchema = definition.value("tables").toObject();
        const auto schemaFor = [&](const FieldBinding &binding) {
            return tablesSchema.value(binding.table).toObject().value("fields").toObject().value(binding.field).toObject();
        };
        const auto read = [&](const QString &role, const QString &semantic, const QString &owner, qint64 index) -> Value {
            const auto binding = editor.get(role, semantic);
            if (binding.table.isEmpty()) return field(globals, binding.field);
            if (binding.table != owner) {
                const auto join = tablesSchema.value(binding.table).toObject().value("join").toObject();
                require(join.value("table").toString() == owner,
                        "Definition lacks a join for editor binding: " + role + "." + semantic);
            }
            const auto &rows = tables[binding.table].rows;
            const auto found = std::lower_bound(rows.cbegin(), rows.cend(), index,
                [](const Row &row, qint64 wanted) { return row.index < wanted; });
            require(found != rows.cend() && found->index == index,
                    "Editor binding references an unselected record: " + binding.table);
            require(found->fields.contains(binding.field), "Editor binding field was not decoded: " + binding.field);
            return found->fields.value(binding.field);
        };
        const auto bounded = [&](qint64 number, const QString &rule) {
            const auto bound = object(editor.limits.value(rule), "editor limits." + rule);
            require(number >= integer(bound.value("minimum")) && number <= integer(bound.value("maximum")),
                    "Value/count is outside definition limits: " + rule);
        };
        const auto editorInteger = [](qint64 value, const QString &what) {
            require(value >= 0 && value <= std::numeric_limits<int>::max(), "Value exceeds editor integer capacity: " + what);
            return int(value);
        };
        ImportedCodeplug result;
        result.model = model();
        result.channelCapacity = int(integer(editor.limits.value("channels").toObject().value("maximum")));
        result.contactCapacity = int(integer(editor.limits.value("contacts").toObject().value("maximum")));
        result.zoneCapacity = int(integer(editor.limits.value("zones").toObject().value("maximum")));
        result.zoneMemberCapacity = int(integer(editor.limits.value("zone_members_maximum")));
        const auto recognition = definition.value("recognition").toObject();
        const QString version = recognition.value("cps_version_field").toString(recognition.value("version_field").toString());
        if (!version.isEmpty()) result.cpsVersion = field(globals, version).raw.toString();
        result.radioName = read("radio", "radio_name", {}, -1).mapped.toString().trimmed();
        result.radioId = editorInteger(numeric(read("radio", "radio_id", {}, -1).raw), "radio_id");

        if (editor.contains("channels", "power")) {
            const auto values = schemaFor(editor.get("channels", "power")).value("enum").toObject();
            // Keep legacy API choices 0=low, 1=high where available; other
            // choices are carried as declared strings, with no model branches.
            for (const QString &v : {QStringLiteral("low"), QStringLiteral("high")})
                for (const auto &x : values) if (x.toString() == v) result.powerSettings.append(v);
            for (const auto &x : values) if (!result.powerSettings.contains(x.toString())) result.powerSettings.append(x.toString());
        }
        QMap<qint64, int> contactIndexes;
        for (const qint64 physical : recordOrder(editor.contacts, editor.indexForTable(editor.contacts))) {
            const auto rowIndex = physical;
            const QString type = read("contacts", "call_type", editor.contacts, rowIndex).mapped.toString();
            bool supportedType = false;
            for (const auto &value : schemaFor(editor.get("contacts", "call_type")).value("enum").toObject())
                supportedType |= value.isString() && value.toString() == type;
            require(supportedType, "Contact call type is not declared by the definition: " + type);
            const QString name = read("contacts", "name", editor.contacts, rowIndex).mapped.toString();
            require(!name.isEmpty(), "Contact name is empty.");
            contactIndexes.insert(rowIndex, result.contacts.size() + 1);
            result.contacts.append({name, editorInteger(numeric(read("contacts", "id", editor.contacts, rowIndex).raw), "contact_id"), type == "group", int(physical), type});
        }
        QMap<qint64, int> channelIndexes;
        for (const qint64 physical : recordOrder(editor.channels, editor.indexForTable(editor.channels))) {
            require(!channelIndexes.contains(physical), "Duplicate editor channel index.");
            ImportedChannel c;
            c.name = read("channels", "name", editor.channels, physical).mapped.toString().trimmed();
            const qint64 tx = numeric(read("channels", "transmit_frequency", editor.channels, physical).mapped);
            const qint64 rx = numeric(read("channels", "receive_frequency", editor.channels, physical).mapped);
            for (qint64 hz : {tx, rx}) {
                bounded(hz, "frequency_hz");
                require(hz >= 0 && quint64(hz) <= std::numeric_limits<quint32>::max(), "Frequency exceeds editor capacity.");
            }
            c.frequencyAHz = quint32(tx); c.frequencyBHz = quint32(rx);
            const QString mode = read("channels", "mode_marker", editor.channels, physical).mapped.toString();
            require(mode == "analog" || mode == "digital", "Channel mode cannot be represented by the editor: " + mode);
            c.digital = mode == "digital";
            if (c.digital) {
                c.colorCode = editorInteger(numeric(read("channels", "color_code", editor.channels, physical).mapped), "color_code");
                c.timeSlot = editorInteger(numeric(read("channels", "time_slot", editor.channels, physical).mapped), "time_slot");
                const auto reference = schemaFor(editor.get("channels", "contact_number")).value("reference").toObject();
                require(reference.value("table").toString() == editor.contacts, "Channel contact binding references a different table.");
                const qint64 number = numeric(read("channels", "contact_number", editor.channels, physical).raw);
                if (!reference.contains("null_value") || number != integer(reference.value("null_value"))) {
                    const qint64 contact = add(number, -integer(reference.value("index_base")));
                    require(contactIndexes.contains(contact), "Channel contact is unavailable in the editor.");
                    c.contactIndex = contactIndexes.value(contact);
                }
            }
            if (editor.contains("channels", "power")) {
                const QString power = read("channels", "power", editor.channels, physical).mapped.toString();
                require(!power.isEmpty(), "Channel power setting is empty.");
                c.setPowerValue(power);
            }
            c.sourceRecord = editorInteger(physical, "source_record");
            require(!c.name.isEmpty(), "Channel name is empty.");
            channelIndexes.insert(physical, result.channels.size());
            result.channels.append(c);
        }
        for (const qint64 physical : recordOrder(editor.zones, editor.indexForTable(editor.zones))) {
            ImportedZone z;
            z.sourceRecord = int(physical);
            z.name = read("zones", "name", editor.zones, physical).mapped.toString().trimmed();
            const auto members = read("zones", "members", editor.zones, physical).raw.toList();
            require(!z.name.isEmpty() && members.size() <= integer(editor.limits.value("zone_members_maximum")),
                    "Invalid zone name or member count.");
            const auto reference = schemaFor(editor.get("zones", "members")).value("reference").toObject();
            require(reference.value("table").toString() == editor.channels, "Zone binding does not reference the channel record table.");
            for (const auto &member : members) {
                const qint64 number = numeric(member);
                if (reference.contains("null_value") && number == integer(reference.value("null_value"))) continue;
                const qint64 physical = add(number, -integer(reference.value("index_base")));
                require(channelIndexes.contains(physical), "Zone channel is not selected by the declared channel index.");
                z.channelIndexes.append(channelIndexes.value(physical));
            }
            result.zones.append(z);
        }
        bounded(result.channels.size(), "channels");
        bounded(result.contacts.size(), "contacts");
        bounded(result.zones.size(), "zones");
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
        else if (type == "u16le" || type == "u16be") size = 2;
        else if (type == "u24le" || type == "u24be") size = 3;
        else if (type == "u32le" || type == "u32be") size = 4;
        else if (type == "ascii" || type == "utf16le" || type == "packed6" || type.startsWith("bcd")) size = integer(schema.value("bytes"));
        else reject(QStringLiteral("Unsupported definition field type: %1").arg(type));
        require(size > 0, QStringLiteral("Field size must be positive."));
        const QByteArray bytes = range(at, size);
        Value result;
        if (type.startsWith('u') && type != "utf16le") {
            quint64 value = 0;
            for (qsizetype i = 0; i < bytes.size(); ++i) {
                const qsizetype shiftIndex = type.endsWith("be") ? bytes.size() - 1 - i : i;
                value |= quint64(quint8(bytes[i])) << (8 * shiftIndex);
            }
            const qint64 mask = schema.contains("mask") ? integer(schema.value("mask")) : qint64((quint64(1) << (size * 8)) - 1);
            const qint64 shift = schema.contains("shift") ? integer(schema.value("shift")) : 0;
            require(mask >= 0 && quint64(mask) < (quint64(1) << (size * 8)) && shift >= 0 && shift < size * 8,
                    QStringLiteral("Invalid integer mask or shift."));
            result.raw = qint64((value & quint64(mask)) >> shift);
        } else if (type == "bcdle" || type == "bcdbe") {
            require(size <= 9, "BCD field exceeds integer capacity.");
            qint64 value = 0;
            for (qsizetype i = 0; i < bytes.size(); ++i) {
                const quint8 byte = quint8(bytes[type == "bcdle" ? bytes.size() - 1 - i : i]);
                require((byte >> 4) <= 9 && (byte & 15) <= 9, "Invalid BCD digit.");
                value = add(multiply(value, 100), (byte >> 4) * 10 + (byte & 15));
            }
            result.raw = value;
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
        require(depth < maximumDepth && !activeTables.contains(name), "Cyclic or excessive table dependencies: " + name);
        const auto schema = object(definition.value("tables").toObject().value(name), "tables." + name);
        activeTables.insert(name);
        Table decoded;
        decoded.count = expression(schema.value("count"), globals);
        require(decoded.count >= 0 && decoded.count <= maximumItems, "Table slot count is negative or exceeds the record limit: " + name);
        const auto segments = schema.contains("segments") ? schema.value("segments").toArray() : QJsonArray{schema};
        qint64 indexBase = 0;
        for (const auto &entry : segments) {
            const auto segment = object(entry, name + ".segment");
            const qint64 count = expression(segment.value("count"), globals);
            require(count >= 0 && count <= decoded.count - indexBase, "Segment count exceeds table slot count: " + name);
            reserve(count);
            qint64 cursor = expression(segment.value("at"), globals);
            const bool fixed = segment.contains("stride_bytes");
            const qint64 stride = fixed ? expression(segment.value("stride_bytes"), globals) : 0;
            const qint64 span = fixed ? (segment.contains("logical_span_bytes") ? integer(segment.value("logical_span_bytes")) : stride) : 0;
            require(!fixed || (stride > 0 && span > 0), "Invalid table stride/span: " + name);
            range(cursor, fixed && count ? add(multiply(count - 1, stride), span) : 0);
            const auto validity = segment.value("validity").toObject();
            require(validity.isEmpty() || fixed, "Selected tables require fixed strides: " + name);
            QSet<qint64> selected;
            if (validity.contains("index_table")) {
                const QString source = validity.value("index_table").toString();
                table(source, depth + 1);
                for (const auto &row : tables.value(source).rows) {
                    const qint64 index = add(numeric(row.fields.value(validity.value("field").toString()).raw),
                                            -integer(validity.value("index_base")));
                    require(index >= 0 && index < count && !selected.contains(index), "Invalid or duplicate selection index: " + name);
                    selected.insert(index);
                }
            }
            qint64 bitmap = -1;
            if (validity.contains("bitmap_at")) {
                bitmap = expression(validity.value("bitmap_at"), globals);
                range(bitmap, (count + 7) / 8);
            }
            QVector<QByteArray> excluded;
            qint64 probeOffset = 0, probeSize = 0;
            if (validity.contains("not_hex")) {
                probeOffset = validity.contains("offset") ? integer(validity.value("offset")) : 0;
                probeSize = integer(validity.value("bytes"));
                require(probeOffset >= 0 && probeSize > 0 && probeSize <= stride && probeOffset <= stride - probeSize,
                        "Selection probe exceeds table stride: " + name);
                for (const auto &pattern : validity.value("not_hex").toArray()) {
                    const auto bytes = hex(pattern);
                    require(bytes.size() == probeSize, "Selection probe pattern size mismatch: " + name);
                    excluded.append(bytes);
                }
            }
            for (qint64 local = 0; local < count; ++local) {
                const qint64 index = add(indexBase, local);
                bool active = true;
                if (validity.contains("index_table")) active = selected.contains(local);
                else if (bitmap >= 0) {
                    const int shift = validity.value("bit_order").toString("lsb_first") == "lsb_first" ? local % 8 : 7 - local % 8;
                    active = (quint8(image[bitmap + local / 8]) & (1 << shift)) != 0;
                } else if (!excluded.isEmpty()) active = !excluded.contains(range(add(cursor, probeOffset), probeSize));
                if (!active) { cursor = add(cursor, stride); continue; }
                Scope scope{object(schema.value("fields"), name + ".fields"), cursor, index, {}, {}};
                for (const auto &key : scope.fields.keys()) field(scope, key);
                const qint64 step = fixed ? stride : expression(schema.value("record_bytes"), scope);
                require(step > 0, "Invalid variable record length: " + name);
                range(cursor, fixed ? span : step);
                decoded.rows.append({index, cursor, scope.values});
                decoded.selected.insert(index);
                cursor = add(cursor, step);
            }
            indexBase = add(indexBase, count);
        }
        require(indexBase == decoded.count, "Segment counts do not equal table slot count: " + name);
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

// Canonical editor values are the bridge; binary layout stays in the definition.
using Record = QMap<QString, QVariant>;
using Records = QMap<QString, QVector<Record>>;

Records editorRecords(const ImportedCodeplug &plug)
{
    Records result{{"channels", {}}, {"contacts", {}}, {"zones", {}}};
    for (const auto &c : plug.channels) {
        Record row{{"name", c.name}, {"transmit_frequency", qint64(c.frequencyAHz)},
                   {"receive_frequency", qint64(c.frequencyBHz)}, {"mode_marker", c.digital ? "digital" : "analog"},
                   {"color_code", qint64(c.colorCode)}, {"time_slot", qint64(c.timeSlot)},
                   {"contact_number", qint64(c.contactIndex)}, {"source_record", qint64(c.sourceRecord)},
                   {"use_default_template", c.useDefaultTemplate}};
        if (!c.powerValue().isEmpty()) row["power"] = c.powerValue();
        result["channels"].append(row);
    }
    for (const auto &c : plug.contacts)
        result["contacts"].append({{"name", c.name}, {"id", qint64(c.id)}, {"call_type", c.callTypeValue()}, {"source_record", qint64(c.sourceRecord)}});
    for (const auto &z : plug.zones) {
        QVariantList members;
        for (int i : z.channelIndexes) members.append(qint64(i));
        result["zones"].append({{"name", z.name}, {"members", members}, {"source_record", qint64(z.sourceRecord)}});
    }
    return result;
}

void keys(const QJsonObject &o, std::initializer_list<const char *> allowed)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool known = false;
        for (const char *name : allowed) if (it.key() == QLatin1String(name)) known = true;
        require(known, QStringLiteral("Unsupported writer parameter: %1").arg(it.key()));
    }
}

void policy(const QJsonObject &o, const char *key, const char *expected)
{
    require(o.value(QLatin1String(key)) == QJsonValue(QLatin1String(expected)),
            QStringLiteral("Unsupported writer policy: %1").arg(QLatin1String(key)));
}

void requireNames(const QJsonValue &value, QStringList expected, const QString &message)
{
    require(value.isArray(), message);
    QStringList actual;
    for (const auto &entry : value.toArray()) {
        require(entry.isString(), message);
        actual.append(entry.toString());
    }
    actual.sort(); expected.sort();
    require(actual == expected, message); // Includes duplicate-name rejection.
}

QStringList targetFields(const QString &role)
{
    if (role == "radio") return {"radio_name", "radio_id"};
    if (role == "channels") return {"name", "transmit_frequency", "receive_frequency", "mode_marker",
                                    "color_code", "time_slot", "contact_number", "power"};
    if (role == "contacts") return {"name", "id", "call_type"};
    if (role == "zones") return {"name", "members"};
    reject("Unknown editor target role: " + role);
}

int storageWidth(const QJsonObject &f)
{
    const QString t = f.value("type").toString();
    if (t == "u8") return 1;
    if (t == "u16le" || t == "u16be") return 2;
    if (t == "u24le" || t == "u24be") return 3;
    if (t == "u32le" || t == "u32be") return 4;
    require(t == "ascii" || t == "utf16le" || t == "packed6" || t == "bcdle" || t == "bcdbe", "Unsupported writable field type: " + t);
    const qint64 n = integer(f.value("bytes"));
    require(n > 0 && n <= maximumImage, "Invalid writable field size.");
    return int(n);
}

class Encoder
{
public:
    Encoder(const QJsonObject &definition, const QByteArray &packagedTemplate, const QByteArray &source,
            const ImportedCodeplug &target, const QString &name, int id)
        : d(definition), templateInput(packagedTemplate), source(source), target(target),
          wanted(editorRecords(target)), globals{{"radio_name", name}, {"radio_id", qint64(id)}},
          plan(object(d.value("creation").toObject().value("construction"), "construction")) {}

    QByteArray run()
    {
        validatePlan();
        selectBase();
        original = editorRecords(basePlug);
        baseGlobals = {{"radio_name", basePlug.radioName}, {"radio_id", qint64(basePlug.radioId)}};
        const auto order = plan.value("order").toArray();
        for (int step = 0; step < order.size(); ++step) {
            const QString label = order[step].toString();
            const auto rule = object(plan.value(label), label);
            const QString op = operationName(label, rule);
            if (op == "validate_target") validateTarget(rule);
            else if (op == "return_original_if_equal") {
                if (tablesEqual(rule, wanted, original) && radioEqual(rule, globals, baseGlobals)) return source;
            } else if (op == "decode_representation") {
                require(!decoded, "Base decoded twice.");
                image = baseImage; decoded = true;
            } else if (op == "write_global_fields") {
                require(decoded, "Radio identity writes require a decoded image.");
                for (const auto &v : rule.value("writes").toArray()) {
                    const auto w = v.toObject();
                    writeField(image, {}, w.value("field").toString(), 0, 0,
                               globalTarget(w.value("from_target").toString()), w);
                    if (w.contains("following_bytes_hex")) {
                        const auto f = fieldSchema({}, w.value("field").toString());
                        const qint64 at = fieldAt(image, {}, w.value("field").toString(), 0, 0);
                        replace(image, add(at, storageWidth(f)), Decoder::hex(w.value("following_bytes_hex")));
                    }
                }
            } else if (op == "branch_if_tables_equal") {
                if (tablesEqual(comparisonRule(), wanted, original)) {
                    const auto found = std::find(order.cbegin(), order.cend(), rule.value("next_step"));
                    const int next = int(std::distance(order.cbegin(), found));
                    require(found != order.cend() && next > step, "Invalid writer branch destination.");
                    step = next - 1;
                }
            } else if (op == "rebuild_fixed_table") rebuildFixedTable(rule);
            else if (op == "remap_references") remapReferences(rule);
            else if (op == "replace_variable_table") replaceVariableTable(rule);
            else if (op == "relocate_pointers") relocate(rule);
            else if (op == "rewrite_relative_index") rewriteRelativeIndex(rule);
            else if (op == "prepare_overlapping_table") prepareOverlappingTable(rule);
            else if (op == "write_overlapping_fields") writeOverlappingFields(rule);
            else if (op == "encode_base_representation") {
                require(decoded, "Cannot encode before decoding.");
                require(preparedTables == packedTables, "Overlapping tables must finish packing before encoding.");
                output = image;
                const auto encoding = representation.value("decode").toObject();
                if (encoding.value("operation") == QJsonValue("repeating_xor")) {
                    Decoder reader(source, d);
                    const auto mask = reader.range(integer(encoding.value("mask_input_offset")), integer(encoding.value("mask_bytes")));
                    qint64 phase = encoding.contains("mask_phase_at_input_zero") ? integer(encoding.value("mask_phase_at_input_zero")) : 0;
                    phase %= mask.size(); if (phase < 0) phase += mask.size();
                    for (qsizetype i = 0; i < output.size(); ++i) output[i] = image[i] ^ mask[(i % mask.size() + phase) % mask.size()];
                }
            } else if (op == "decode_and_compare_target") {
                require(!output.isEmpty(), "No encoded output to validate.");
                const auto raw = decode(output, representation);
                const auto checkedPlug = Decoder(raw, d).imported();
                require(tablesEqual(comparisonRule(), wanted, editorRecords(checkedPlug)), "Exported records differ after reimport.");
                const Record checkedGlobals{{"radio_name", checkedPlug.radioName}, {"radio_id", qint64(checkedPlug.radioId)}};
                require(radioEqual(rule, globals, checkedGlobals), "Exported radio identity differs after reimport.");
                validated = true;
            }
        }
        require(validated, "Writer did not validate its output.");
        return output;
    }

private:
    QJsonObject tableSchema(const QString &name) const
    { return object(d.value("tables").toObject().value(name), "table " + name); }
    QJsonObject fieldSchema(const QString &table, const QString &name) const
    {
        return object((table.isEmpty() ? d : tableSchema(table)).value("fields").toObject().value(name), "field " + name);
    }
    QVariant globalTarget(const QString &name) const
    { require(globals.contains(name), "Unknown target global: " + name); return globals.value(name); }
    qint64 evaluate(const QJsonValue &v, const QByteArray &bytes, Scope *scope = nullptr, int depth = 0)
    {
        require(depth < maximumDepth, "Writer expression depth exceeded.");
        if (!v.isObject()) return integer(v);
        const auto e = v.toObject(); require(e.size() == 1, "Invalid writer expression.");
        const QString op = e.begin().key(); const auto operand = e.begin().value();
        if (op == "context") { require(context.contains(operand.toString()), "Undefined writer context."); return context.value(operand.toString()); }
        if (op == "base_count" || op == "target_count") {
            const auto &records = op == "base_count" ? original : wanted;
            require(records.contains(operand.toString()), "Unknown count table.");
            return records.value(operand.toString()).size();
        }
        if (op == "add" || op == "subtract" || op == "multiply" || op == "max") {
            require(operand.isArray(), "Writer operands must be an array.");
            const auto args = operand.toArray();
            require(!args.isEmpty() && (op != "subtract" || args.size() == 2), "Invalid writer operands.");
            qint64 n = evaluate(args[0], bytes, scope, depth + 1);
            for (int i = 1; i < args.size(); ++i) {
                const qint64 next = evaluate(args[i], bytes, scope, depth + 1);
                if (op == "add") n = add(n, next);
                else if (op == "multiply") n = multiply(n, next);
                else if (op == "max") n = std::max(n, next);
                else { require(next != std::numeric_limits<qint64>::min(), "Writer subtraction overflow."); n = add(n, -next); }
            }
            return n;
        }
        Decoder reader(bytes, d);
        return scope ? reader.expression(v, *scope) : reader.evaluate(v);
    }
    qint64 tableAt(const QByteArray &bytes, const QString &name) { return evaluate(tableSchema(name).value("at"), bytes); }
    qint64 stride(const QString &name) {
        const auto schema = tableSchema(name);
        if (schema.contains("stride_bytes")) return integer(schema.value("stride_bytes"));
        const auto segments = schema.value("segments").toArray();
        require(!segments.isEmpty(), "Missing record stride.");
        const qint64 size = integer(segments[0].toObject().value("stride_bytes"));
        for (const auto &v : segments) require(integer(v.toObject().value("stride_bytes")) == size, "Fixed rebuild requires uniform segment strides.");
        return size;
    }
    qint64 recordAt(const QByteArray &bytes, const QString &name, qint64 index)
    {
        const auto schema = tableSchema(name);
        if (!schema.contains("segments")) return add(tableAt(bytes, name), multiply(stride(name), index));
        require(index >= 0, "Negative record index.");
        for (const auto &v : schema.value("segments").toArray()) {
            const auto segment = v.toObject(); const qint64 count = evaluate(segment.value("count"), bytes);
            if (index < count) return add(evaluate(segment.value("at"), bytes), multiply(integer(segment.value("stride_bytes")), index));
            index -= count;
        }
        reject("Record index exceeds declared segments.");
    }
    qint64 fieldAt(const QByteArray &bytes, const QString &table, const QString &name, qint64 at, qint64 index)
    {
        const auto f = fieldSchema(table, name);
        Decoder reader(bytes, d);
        if (table.isEmpty()) return reader.evaluate(f.value("at"));
        auto scope = reader.recordScope(table, at, index);
        return f.contains("at") ? reader.expression(f.value("at"), scope) : add(at, reader.expression(f.value("offset"), scope));
    }
    void range(const QByteArray &bytes, qint64 at, qint64 size)
    { require(at >= 0 && size >= 0 && at <= bytes.size() && size <= bytes.size() - at, "Write range is outside image."); }
    void replace(QByteArray &bytes, qint64 at, const QByteArray &value)
    { range(bytes, at, value.size()); std::copy(value.begin(), value.end(), bytes.begin() + at); }
    char fillByte(const QJsonValue &value)
    { const auto b = Decoder::hex(value); require(b.size() == 1, "Fill requires one byte."); return b[0]; }
    void fill(QByteArray &bytes, qint64 at, qint64 size, const QJsonValue &value)
    { range(bytes, at, size); std::fill(bytes.begin() + at, bytes.begin() + at + size, fillByte(value)); }
    qint64 readNumber(const QByteArray &bytes, qint64 at, const QString &type)
    {
        const int size = storageWidth(QJsonObject{{"type", type}});
        require(type.startsWith('u') && type != "utf16le", "Pointer must use an integer type.");
        range(bytes, at, size); qint64 n = 0;
        for (int i = 0; i < size; ++i) n |= qint64(quint8(bytes[at + i])) << (8 * (type.endsWith("be") ? size - 1 - i : i));
        return n;
    }
    void writeScalar(QByteArray &bytes, qint64 at, const QJsonObject &f, QVariant value, const QJsonObject &options)
    {
        const int size = storageWidth(f); range(bytes, at, size);
        const QString type = f.value("type").toString();
        if (options.contains("clear_storage_hex")) fill(bytes, at, size, options.value("clear_storage_hex"));
        if (type == "ascii" || type == "utf16le") {
            require(value.metaType().id() == QMetaType::QString, "Text field requires text.");
            const QString text = value.toString();
            require(text.isValidUtf16() && text.size() <= size / (type == "ascii" ? 1 : 2), "Text exceeds field storage or is invalid.");
            for (qsizetype i = 0; i < text.size(); ++i) {
                const ushort code = text[i].unicode();
                require(type != "ascii" || code < 128, "Text is not ASCII.");
                bytes[at + i * (type == "ascii" ? 1 : 2)] = char(code & 255);
                if (type == "utf16le") bytes[at + i * 2 + 1] = char(code >> 8);
            }
            return;
        }
        if (type == "packed6") {
            const QString alphabet = f.value("alphabet").toString(), text = value.toString();
            const qint64 count = integer(f.value("characters"));
            require(value.metaType().id() == QMetaType::QString && alphabet.size() == 64 && count >= text.size()
                        && count <= qint64(size) * 8 / 6, "Invalid packed name.");
            for (qint64 i = 0; i < count; ++i) {
                const int code = int(i >= text.size() ? integer(f.value("padding_code"))
                                      : text[i] == QChar(' ') ? integer(f.value("space_code")) : alphabet.indexOf(text[i]));
                require(code >= 0 && code < 64, "Character is absent from packed alphabet.");
                for (int j = 0; j < 6; ++j) {
                    const qint64 bit = i * 6 + j, pos = at + bit / 8;
                    const quint8 mask = quint8(1 << (7 - bit % 8));
                    bytes[pos] = char((quint8(bytes[pos]) & ~mask) | (((code >> (5 - j)) & 1) ? mask : 0));
                }
            }
            return;
        }
        const auto enums = f.value("enum").toObject();
        if (!enums.isEmpty()) {
            bool found = false;
            for (auto it = enums.begin(); it != enums.end(); ++it) {
                const QVariant mapped = it.value().isDouble() ? QVariant(integer(it.value())) : it.value().toVariant();
                if (mapped == value) { value = integer(QJsonValue(it.key())); found = true; break; }
            }
            require(found, "Writable enum value is unknown.");
        }
        qint64 n = numeric(value);
        if (f.contains("scale")) {
            const qint64 scale = integer(f.value("scale"));
            require(scale > 0 && n % scale == 0, "Value does not fit field scale."); n /= scale;
        }
        require(n >= 0 && (!f.contains("minimum") || n >= integer(f.value("minimum")))
                    && (!f.contains("maximum") || n <= integer(f.value("maximum"))), "Value violates field bounds.");
        if (type == "bcdle" || type == "bcdbe") {
            require(size <= 9 && !f.contains("mask") && !f.contains("shift"), "Invalid BCD write descriptor.");
            for (int i = 0; i < size; ++i) {
                const int low = n % 10; n /= 10;
                const int high = n % 10; n /= 10;
                bytes[at + (type == "bcdle" ? i : size - 1 - i)] = char((high << 4) | low);
            }
            require(n == 0, "Integer exceeds BCD field capacity.");
            return;
        }
        const quint64 full = (quint64(1) << (size * 8)) - 1;
        const qint64 shift = f.contains("shift") ? integer(f.value("shift")) : 0;
        const qint64 mask = f.contains("mask") ? integer(f.value("mask")) : qint64(full);
        require(mask >= 0 && quint64(mask) <= full && shift >= 0 && shift < size * 8
                    && quint64(n) <= (full >> shift), "Integer exceeds writable storage.");
        const quint64 encoded = quint64(n) << shift;
        require((encoded & ~quint64(mask)) == 0, "Integer exceeds field mask.");
        quint64 old = 0;
        for (int i = 0; i < size; ++i) old |= quint64(quint8(bytes[at + i])) << (8 * (type.endsWith("be") ? size - 1 - i : i));
        const quint64 updated = (old & ~quint64(mask)) | encoded;
        for (int i = 0; i < size; ++i) bytes[at + i] = char((updated >> (8 * (type.endsWith("be") ? size - 1 - i : i))) & 255);
    }
    void writeField(QByteArray &bytes, const QString &table, const QString &name, qint64 at, qint64 index,
                    const QVariant &value, const QJsonObject &options = {})
    {
        const auto f = fieldSchema(table, name);
        require(!f.contains("value"), "Cannot write a computed field.");
        const qint64 address = fieldAt(bytes, table, name, at, index);
        if (f.contains("only_if")) {
            require(!table.isEmpty(), "Conditional writes require a record scope.");
            Decoder reader(bytes, d); auto scope = reader.recordScope(table, at, index);
            const auto condition = f.value("only_if").toObject();
            const qint64 actual = reader.expression(condition.value("value"), scope);
            bool applies = false;
            for (const auto &v : condition.value("in").toArray()) if (integer(v) == actual) applies = true;
            if (!applies) return;
        }
        if (f.contains("count")) {
            require(value.metaType().id() == QMetaType::QVariantList, "Array field requires an array.");
            Decoder reader(bytes, d); auto scope = reader.recordScope(table, at, index);
            const auto values = value.toList();
            require(values.size() == reader.expression(f.value("count"), scope), "Array count differs from record count.");
            const qint64 step = reader.expression(f.value("stride_bytes"), scope);
            require(step > 0, "Invalid writable array stride.");
            for (qsizetype i = 0; i < values.size(); ++i) writeScalar(bytes, add(address, multiply(i, step)), f, values[i], options);
        } else writeScalar(bytes, address, f, value, options);
    }
    void writeGlobal(const QString &field, qint64 value) { writeField(image, {}, field, 0, 0, value); }
    QJsonObject comparisonRule() const
    { return object(plan.value(constructionRule(d, "decode_and_compare_target").value("compare_as").toString()), "comparison rule"); }
    bool radioEqual(const QJsonObject &rule, const Record &a, const Record &b) const
    {
        for (const auto &v : rule.value("compare_radio_fields").toArray()) {
            const QString name = v.toString();
            require(a.contains(name) && b.contains(name), "Unknown compared radio field.");
            if (a.value(name) != b.value(name)) return false;
        }
        return true;
    }
    bool tablesEqual(const QJsonObject &rule, const Records &a, const Records &b) const
    {
        const auto compared = rule.value("compare_tables").toObject();
        const auto optional = rule.value("compare_if_supplied").toObject();
        for (auto it = compared.begin(); it != compared.end(); ++it) {
            const QString name = it.key();
            require(a.contains(name) && b.contains(name), "Unknown compared table.");
            const auto &x = a[name]; const auto &y = b[name];
            if (x.size() != y.size()) return false;
            for (qsizetype i = 0; i < x.size(); ++i) {
                for (const auto &v : it.value().toArray()) {
                    const QString key = v.toString();
                    require(x[i].contains(key) && y[i].contains(key), "Unknown compared record field.");
                    if (x[i].value(key) != y[i].value(key)) return false;
                }
                for (const auto &v : optional.value(name).toArray())
                    if (x[i].contains(v.toString()) && x[i].value(v.toString()) != y[i].value(v.toString())) return false;
            }
        }
        return true;
    }
    void selectBase()
    {
        QSet<QByteArray> images; QStringList reasons;
        for (const auto &v : d.value("representations").toArray()) {
            try {
                const auto r = v.toObject(); const auto raw = decode(source, r);
                Decoder reader(raw, d);
                if (reader.model() != d.value("recognition").toObject().value("model_value").toString()) continue;
                const auto plug = reader.imported();
                const auto digest = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
                if (images.contains(digest)) continue;
                images.insert(digest);
                if (images.size() == 1) { baseImage = raw; representation = r; basePlug = plug; }
            } catch (const std::exception &e) { reasons.append(QString::fromUtf8(e.what())); }
        }
        require(images.size() == 1, "Export base is invalid or ambiguous: " + reasons.join(' '));
    }
    void nameValid(const QString &name, QString ruleName, const QJsonObject &names)
    {
        for (int depth = 0; depth < maximumDepth; ++depth) {
            const auto rule = object(names.value(ruleName), "name rule");
            if (rule.contains("same_as")) { keys(rule, {"same_as"}); ruleName = rule.value("same_as").toString(); continue; }
            if (rule.value("type") == QJsonValue("utf16le")) {
                keys(rule, {"type", "minimum_code_units", "maximum_code_units", "minimum_code_unit", "forbidden_code_units"});
                require(name.isValidUtf16() && name.size() >= integer(rule.value("minimum_code_units"))
                            && name.size() <= integer(rule.value("maximum_code_units")), "Name does not fit the definition.");
                for (QChar ch : name) {
                    require(ch.unicode() >= integer(rule.value("minimum_code_unit")), "Control character in name.");
                    for (const auto &code : rule.value("forbidden_code_units").toArray())
                        require(ch.unicode() != integer(code), "Forbidden character in name.");
                }
            } else {
                keys(rule, {"minimum_characters", "maximum_characters", "allowed_characters"});
                require(name.size() >= integer(rule.value("minimum_characters")) && name.size() <= integer(rule.value("maximum_characters")), "Contact name does not fit the definition.");
                const QString alphabet = rule.value("allowed_characters").toString();
                for (QChar ch : name) require(alphabet.contains(ch), "Unsupported contact name character.");
            }
            return;
        }
        reject("Cyclic name validation rule.");
    }
    void bounded(qint64 n, const QJsonObject &limits, const QString &what)
    {
        keys(limits, {"minimum", "maximum"});
        require(n >= integer(limits.value("minimum")) && n <= integer(limits.value("maximum")), what + " exceeds definition limits.");
    }
    void validateTarget(const QJsonObject &r)
    {
        require(target.model == basePlug.model && target.cpsVersion == basePlug.cpsVersion, "Target identity does not match its export base.");
        const auto names = r.value("names").toObject();
        nameValid(globals.value("radio_name").toString(), "radio_name", names);
        bounded(numeric(globals.value("radio_id")), r.value("radio_id").toObject(), "Radio ID");
        for (const QString &table : {QStringLiteral("channels"), QStringLiteral("contacts"), QStringLiteral("zones")})
            bounded(wanted.value(table).size(), r.value(table).toObject(), table);
        for (const auto &c : wanted.value("channels")) {
            require(!c.contains("power") || editor->contains("channels", "power"),
                    "Definition does not declare a writable channel power binding.");
            nameValid(c.value("name").toString(), "channel_name", names);
            for (const QString &key : {QStringLiteral("transmit_frequency"), QStringLiteral("receive_frequency")})
                bounded(numeric(c.value(key)), r.value("frequency_hz").toObject(), "Frequency");
            bounded(numeric(c.value("color_code")), r.value("color_code").toObject(), "Color code");
            const auto ref = r.value("contact_reference").toObject();
            const qint64 number = numeric(c.value("contact_number"));
            const qint64 index = add(number, -integer(ref.value("index_base")));
            require(number == integer(ref.value("null_value")) || (index >= 0 && index < wanted.value(ref.value("target_table").toString()).size()), "Channel references an unknown contact.");
            if (c.value("mode_marker") == QVariant("digital")) {
                bool valid = false;
                for (const auto &v : r.value("digital_time_slots").toArray()) if (integer(v) == numeric(c.value("time_slot"))) valid = true;
                require(valid, "Invalid digital time slot.");
            }
        }
        for (const auto &c : wanted.value("contacts")) {
            nameValid(c.value("name").toString(), "contact_name", names);
            bounded(numeric(c.value("id")), r.value("contact_id").toObject(), "Contact ID");
        }
        const auto ref = r.value("zone_reference").toObject();
        for (const auto &z : wanted.value("zones")) {
            nameValid(z.value("name").toString(), "zone_name", names);
            const auto members = z.value("members").toList();
            require(members.size() <= integer(r.value("zone_members_maximum")), "Zone member count exceeds definition limit.");
            for (const auto &v : members) {
                const qint64 index = add(numeric(v), -integer(ref.value("input_index_base")));
                require(index >= 0 && index < wanted.value(ref.value("target_table").toString()).size(), "Zone references an unknown channel.");
            }
        }
    }
    QByteArray selectedRecord(const Record &targetRow, qint64 index, const QJsonObject &rule)
    {
        const QString table = rule.value("records_table").toString();
        for (const auto &v : rule.value("record_selection").toArray()) {
            const auto choice = v.toObject();
            if (choice.contains("when_target_flag") && !targetRow.value(choice.value("when_target_flag").toString()).toBool()) continue;
            if (choice.contains("when_target_field_supplied")) {
                const QString key = choice.value("when_target_field_supplied").toString();
                if (!targetRow.contains(key) || numeric(targetRow.value(key)) == integer(choice.value("null_value"))) continue;
            }
            const bool fromBase = choice.value("source") == QJsonValue("base");
            if (!fromBase && templateImage.isEmpty()) {
                for (const auto &r : d.value("representations").toArray()) {
                    if (r.toObject().value("name") == d.value("creation").toObject().value("representation")) {
                        templateImage = decode(templateInput, r.toObject()); break;
                    }
                }
                require(!templateImage.isEmpty(), "Template representation is missing.");
                templatePlug = Decoder(templateImage, d).imported();
            }
            const auto &bytes = fromBase ? baseImage : templateImage;
            const auto records = fromBase ? original : editorRecords(templatePlug);
            const auto rows = records.value(editor->roleForTable(table));
            qint64 candidate = -1;
            QJsonArray match = choice.contains("match") ? choice.value("match").toArray() : choice.value("require_match").toArray();
            const auto matches = [&](qint64 i) {
                if (i < 0 || i >= rows.size()) return false;
                for (const auto &field : match) {
                    require(rows[i].contains(field.toString()) && targetRow.contains(field.toString()),
                            "Record selection refers to an unavailable target field: " + field.toString());
                    if (rows[i].value(field.toString()) != targetRow.value(field.toString())) return false;
                }
                return true;
            };
            if (choice.contains("index_from_target")) {
                const auto sourceIndex = targetRow.value(choice.value("index_from_target").toString());
                auto &positions = sourcePositions[table + (fromBase ? ":base" : ":template")];
                if (positions.isEmpty()) for (int i = 0; i < rows.size(); ++i)
                    positions.insert(numeric(rows[i].value("source_record")), i);
                candidate = positions.value(numeric(sourceIndex), -1);
            }
            else if (choice.contains("index")) candidate = index;
            else for (qint64 i = 0; i < rows.size(); ++i) if (matches(i)) { candidate = i; break; }
            if (matches(candidate)) {
                auto &cache = fromBase ? baseRecordCache : templateRecordCache;
                if (!cache.contains(table)) {
                    Decoder reader(bytes, d);
                    cache.insert(table, reader.records(table).rows);
                    auto &orders = fromBase ? baseOrderCache : templateOrderCache;
                    orders.insert(table, reader.recordOrder(table, editor->indexForTable(table)));
                }
                const auto &physical = cache[table];
                const auto &orders = fromBase ? baseOrderCache : templateOrderCache;
                require(candidate < orders.value(table).size(), "Source record order is incomplete.");
                const qint64 storageIndex = orders.value(table)[candidate];
                const auto found = std::lower_bound(physical.cbegin(), physical.cend(), storageIndex,
                    [](const Row &row, qint64 wanted) { return row.index < wanted; });
                require(found != physical.cend() && found->index == storageIndex, "Selected source record is missing.");
                const qint64 size = stride(table);
                range(bytes, found->address, size);
                return bytes.mid(found->address, size);
            }
            const QString failure = choice.value(choice.contains("on_invalid") ? "on_invalid" : "on_missing").toString();
            require(failure == "continue", "Invalid source record or missing channel-mode default.");
        }
        reject("No matching source record.");
    }
    void applyWrites(QByteArray &bytes, const QString &defaultTable, qint64 at, qint64 index,
                     const Record &row, const QJsonArray &writes)
    {
        for (const auto &entry : writes) {
            const auto w = entry.toObject(); const QString targetField = w.value("from_target").toString();
            if (w.value("only_if_supplied").toBool() && !row.contains(targetField)) continue;
            if (w.contains("when")) {
                const auto when = w.value("when").toObject();
                if (row.value(when.value("target_field").toString()) != when.value("equals").toVariant()) continue;
            }
            QVariant value;
            if (w.contains("value_from_target_array_length")) value = qint64(row.value(w.value("value_from_target_array_length").toString()).toList().size());
            else { require(row.contains(targetField), "Missing target field: " + targetField); value = row.value(targetField); }
            if (w.contains("input_index_base")) {
                const qint64 delta = add(integer(w.value("stored_index_base")), -integer(w.value("input_index_base")));
                if (value.metaType().id() == QMetaType::QVariantList) {
                    QVariantList adjusted;
                    for (const auto &v : value.toList()) adjusted.append(add(numeric(v), delta));
                    value = adjusted;
                } else value = add(numeric(value), delta);
            }
            const QString table = w.value("table").toString(defaultTable);
            const qint64 address = w.contains("table") ? recordAt(bytes, table, index) : at;
            if (w.value("pad_to_schema_count").toBool()) {
                Decoder reader(bytes, d); auto scope = reader.recordScope(table, address, index);
                const auto f = fieldSchema(table, w.value("field").toString());
                const qint64 count = reader.expression(f.value("count"), scope);
                auto values = value.toList(); require(values.size() <= count, "Target array exceeds fixed storage.");
                while (values.size() < count) values.append(integer(w.value("padding_value")));
                value = values;
            }
            writeField(bytes, table, w.value("field").toString(), address, index, value, w);
        }
    }
    void rebuildFixedTable(const QJsonObject &r)
    {
        require(decoded, "Fixed-table rebuild requires a decoded image.");
        const QString table = r.value("records_table").toString(), indexes = r.value("index_table").toString();
        const QString role = editor->roleForTable(table);
        require(!packedTables.contains(table), "Physical record writes after overlapping packing are forbidden: " + table);
        const auto rows = wanted.value(role);
        if (r.contains("capacity")) require(rows.size() <= integer(r.value("capacity")), "Record capacity exceeded.");
        if (r.value("resize_to_end").toBool()) {
            require(add(tableAt(baseImage, table), multiply(original.value(role).size(), stride(table))) == baseImage.size(),
                    "End-resize table does not occupy the end of the source image.");
            const qint64 end = add(tableAt(image, table), multiply(rows.size(), stride(table)));
            require(end <= std::numeric_limits<int>::max(), "Resized image exceeds capacity.");
            image.resize(int(end));
        }
        if (r.contains("count_field")) writeGlobal(r.value("count_field").toString(), rows.size());
        const auto removed = r.value("removed_records").toObject();
        const qint64 extent = r.value("unused_records").toString() == "clear"
            ? integer(r.value("capacity")) : std::max(rows.size(), original.value(role).size());
        for (qint64 i = 0; i < extent; ++i) {
            if (i >= rows.size()) {
                for (const auto &v : removed.value("tables").toArray()) {
                    const QString name = v.toString();
                    if (!r.value("resize_to_end").toBool()) fill(image, recordAt(image, name, i), stride(name), removed.value("fill_hex"));
                }
                continue;
            }
            const auto index = r.value("index").toObject();
            if (!indexes.isEmpty()) writeField(image, indexes, index.value("field").toString(), recordAt(image, indexes, i), i,
                       add(integer(index.value("first")), multiply(i, integer(index.value("step")))));
            const auto record = selectedRecord(rows[i], i, r);
            require(record.size() == stride(table), "Selected record has the wrong size.");
            replace(image, recordAt(image, table, i), record);
            for (const auto &entry : r.value("clear_ranges").toArray()) {
                const auto c = entry.toObject();
                fill(image, add(recordAt(image, table, i), integer(c.value("offset"))), integer(c.value("bytes")), c.value("fill_hex"));
            }
            applyWrites(image, table, recordAt(image, table, i), i, rows[i], r.value("writes").toArray());
        }
        if (r.value("reset_selection").toBool()) {
            const auto schema = tableSchema(table);
            const auto segments = schema.contains("segments") ? schema.value("segments").toArray() : QJsonArray{schema};
            qint64 physical = 0;
            for (const auto &entry : segments) {
                const auto segment = entry.toObject();
                const auto valid = segment.value("validity").toObject();
                const qint64 count = evaluate(segment.value("count"), image);
                require(valid.contains("bitmap_at"), "Selection reset requires a declared bitmap.");
                const qint64 at = evaluate(valid.value("bitmap_at"), image);
                const QString order = valid.value("bit_order").toString();
                require(order == "lsb_first" || order == "msb_first", "Unknown bitmap order.");
                range(image, at, (count + 7) / 8);
                for (qint64 j = 0; j < count; ++j, ++physical) {
                    const int bit = 1 << (order == "lsb_first" ? j % 8 : 7 - j % 8);
                    const qint64 address = at + j / 8;
                    image[address] = char(physical < rows.size() ? (quint8(image[address]) | bit) : (quint8(image[address]) & ~bit));
                }
            }
        }
    }
    // Reference locations, index bases and deletion policies are schema data.
    // This operates on the immutable source, so reordered records never read
    // references already rewritten in the destination.
    void remapReferences(const QJsonObject &rule)
    {
        QMap<QString, QMap<qint64, qint64>> maps;
        for (const QString &role : {QStringLiteral("channels"), QStringLiteral("contacts"), QStringLiteral("zones")}) {
            const auto rows = wanted.value(role);
            for (int i = 0; i < rows.size(); ++i) {
                const qint64 sourceIndex = numeric(rows[i].value("source_record", qint64(-1)));
                if (sourceIndex >= 0 && !maps[role].contains(sourceIndex)) maps[role][sourceIndex] = i;
            }
        }
        Decoder reader(baseImage, d);
        for (const auto &entry : rule.value("lists").toArray()) {
            const auto r = entry.toObject(); const QString table = r.value("table").toString();
            const auto schema = fieldSchema(table, r.value("field").toString());
            const auto ref = schema.value("reference").toObject();
            const QString role = editor->roleForTable(ref.value("table").toString());
            const qint64 base = integer(ref.value("index_base"));
            for (const auto &row : reader.records(table).rows) {
                auto scope = reader.recordScope(table, row.address, row.index);
                const auto values = reader.field(scope, r.value("field").toString()).raw.toList();
                QVariantList mapped;
                for (const auto &value : values) {
                    const qint64 physical = add(numeric(value), -base);
                    if (maps[role].contains(physical)) mapped.append(add(maps[role][physical], base));
                }
                const qint64 address = fieldAt(baseImage, table, r.value("field").toString(), row.address, row.index);
                const qint64 step = reader.expression(schema.value("stride_bytes"), scope);
                require(step > 0, "Invalid reference-list stride.");
                for (int i = 0; i < values.size(); ++i)
                    writeScalar(image, add(address, multiply(i, step)), schema, qint64(0), {});
                writeField(image, table, r.value("count_field").toString(), row.address, row.index, add(qint64(mapped.size()), r.contains("count_bias") ? integer(r.value("count_bias")) : 0));
                writeField(image, table, r.value("field").toString(), row.address, row.index, mapped);
            }
        }
        for (const auto &entry : rule.value("globals").toArray()) {
            const auto r = entry.toObject(); const QString field = r.value("field").toString();
            const QString role = r.value("target_role").toString();
            const qint64 old = numeric(reader.global(field).raw);
            writeGlobal(field, maps[role].value(old, 0));
        }
        for (const auto &entry : rule.value("image_size_fields").toArray()) writeGlobal(entry.toString(), image.size());
    }
    void replaceVariableTable(const QJsonObject &r)
    {
        require(decoded, "Variable-table replacement requires a decoded image.");
        const QString table = r.value("table").toString();
        const QString role = editor->roleForTable(table);
        require(!replacedTables.contains(table) && !packedTables.contains(table), "Variable table replaced twice or after packing: " + table);
        const auto schema = tableSchema(table); QByteArray serialized;
        require(schema.contains("record_bytes"), "Variable record sizing is missing.");
        const auto rows = wanted.value(role);
        for (qsizetype i = 0; i < rows.size(); ++i) {
            Decoder reader(image, d); auto scope = reader.recordScope(table, 0, i);
            scope.providedOnly = true;
            for (const auto &entry : r.value("fields").toArray()) {
                const auto w = entry.toObject();
                const QString semantic = w.value("from_target").toString();
                if (w.value("only_if_supplied").toBool() && !rows[i].contains(semantic)) continue;
                const auto when = w.value("when").toObject();
                if (!when.isEmpty() && rows[i].value(when.value("target_field").toString()) != when.value("equals").toVariant()) continue;
                const QString fieldName = w.value("field").toString();
                const auto f = fieldSchema(table, fieldName);
                if (f.contains("count")) continue; // Record-size expressions require scalars.
                QVariant value;
                if (w.contains("value_from_target_array_length"))
                    value = qint64(rows[i].value(w.value("value_from_target_array_length").toString()).toList().size());
                else {
                    require(rows[i].contains(semantic), "Missing target field: " + semantic);
                    value = rows[i].value(semantic);
                }
                if (w.contains("input_index_base"))
                    value = add(numeric(value), add(integer(w.value("stored_index_base")), -integer(w.value("input_index_base"))));
                QByteArray encoded(storageWidth(f), '\0');
                writeScalar(encoded, 0, f, value, w);
                scope.values[fieldName] = Decoder(encoded, d).scalarValue(f, 0);
            }
            const qint64 size = reader.expression(schema.value("record_bytes"), scope);
            require(size > 0 && size <= maximumImage - serialized.size(), "Serialized variable table exceeds size limit.");
            QByteArray record(size, '\0');
            applyWrites(record, table, 0, i, rows[i], r.value("fields").toArray());
            serialized.append(record);
        }
        Decoder reader(baseImage, d); const auto &old = reader.records(table).rows;
        const qint64 at = tableAt(baseImage, table); qint64 oldSize = 0;
        for (const auto &row : old) {
            require(row.address == add(at, oldSize), "Variable records are not contiguous.");
            auto scope = reader.recordScope(table, row.address, row.index); scope.values = row.fields;
            oldSize = add(oldSize, reader.expression(schema.value("record_bytes"), scope));
        }
        const qint64 destination = tableAt(image, table);
        range(image, destination, oldSize);
        require(image.size() - oldSize <= maximumImage - serialized.size(), "Resized image exceeds size limit.");
        const auto save = r.value("save_context").toObject();
        for (auto it = save.begin(); it != save.end(); ++it) {
            if (it.value() == QJsonValue("new_span_bytes_minus_base_span_bytes")) context[it.key()] = serialized.size() - oldSize;
            else if (it.value() == QJsonValue("base_span_end")) context[it.key()] = add(at, oldSize);
            else reject("Unsupported saved writer context.");
        }
        image.replace(destination, oldSize, serialized); replacedTables.insert(table);
    }
    void relocate(const QJsonObject &r)
    {
        require(decoded, "Pointer relocation requires a decoded image.");
        const auto directory = r.value("pointer_directory").toObject(), limits = r.value("constraints").toObject();
        const qint64 first = integer(directory.value("first_at")), last = integer(directory.value("last_at")), step = integer(directory.value("stride_bytes"));
        require(first >= 0 && last >= first && step > 0 && (last - first) / step < maximumItems, "Invalid pointer directory.");
        const qint64 delta = evaluate(r.value("delta"), image), minimum = evaluate(limits.value("base_minimum"), image);
        const auto relocateValue = [&](qint64 old, qint64 lower) {
            const qint64 value = add(old, delta);
            require(old >= minimum && value >= lower && value < evaluate(limits.value("relocated_maximum_exclusive"), image), "Invalid relocated pointer.");
            return value;
        };
        for (qint64 at = first; at <= last; at = add(at, step)) {
            const QString type = directory.value("type").toString();
            const qint64 old = readNumber(baseImage, at, type);
            if (old != integer(directory.value("skip_value")))
                writeScalar(image, at, QJsonObject{{"type", type}}, relocateValue(old, integer(limits.value("relocated_minimum"))), {});
        }
        const QString additional = r.value("additional_pointer_field").toString();
        Decoder reader(baseImage, d);
        writeGlobal(additional, relocateValue(numeric(reader.global(additional).raw), integer(limits.value("additional_pointer_minimum"))));
    }
    void rewriteRelativeIndex(const QJsonObject &r)
    {
        require(decoded, "Relative-index writes require a decoded image.");
        const QString table = r.value("records_table").toString(), indexes = r.value("index_table").toString();
        const auto rows = wanted.value(editor->roleForTable(table)); writeGlobal(r.value("count_field").toString(), rows.size());
        const qint64 clear = std::max(evaluate(r.value("clear_entries"), image), integer(r.value("clear_minimum")));
        fill(image, tableAt(image, indexes), multiply(clear, stride(indexes)), r.value("fill_hex"));
        qint64 offset = integer(r.value("implicit_first_offset"));
        const qint64 first = integer(r.value("first_indexed_record"));
        for (qsizetype i = 0; i < rows.size(); ++i) {
            if (i >= first) writeField(image, indexes, r.value("index_field").toString(), recordAt(image, indexes, i - first), i - first, offset);
            Decoder reader(image, d); auto scope = reader.recordScope(table, add(tableAt(image, table), offset), i);
            offset = add(offset, reader.expression(tableSchema(table).value("record_bytes"), scope));
        }
    }
    void prepareOverlappingTable(const QJsonObject &r)
    {
        require(decoded, "Overlapping initialization requires a decoded image.");
        const QString table = r.value("records_table").toString(), indexes = r.value("index_table").toString();
        const QString role = editor->roleForTable(table);
        require(!preparedTables.contains(table) && !packedTables.contains(table), "Overlapping table must be initialized once, before packing: " + table);
        const auto rows = wanted.value(role); const qint64 count = std::max(rows.size(), original.value(role).size());
        const qint64 at = tableAt(image, table), size = stride(table);
        const auto capacity = r.value("capacity").toObject();
        require(add(tableAt(image, indexes), multiply(count, stride(indexes))) <= add(at, -integer(capacity.value("prefix_bytes")))
                    && add(at, multiply(add(rows.size(), integer(capacity.value("trailing_strides_required"))), size)) <= image.size(),
                "Contact table has insufficient capacity.");
        writeGlobal(r.value("count_field").toString(), rows.size());
        const auto index = r.value("index").toObject(), init = r.value("initialize").toObject();
        for (qint64 i = 0; i < count; ++i) {
            const qint64 address = add(at, multiply(i, size));
            const bool removed = i >= rows.size(), added = i >= original.value(role).size();
            if ((removed && init.value("zero_physical_strides").toArray().contains("removed"))
                || (added && init.value("zero_physical_strides").toArray().contains("new")))
                fill(image, address, size, QJsonValue("00"));
            if (removed) fill(image, recordAt(image, indexes, i), stride(indexes), index.value("removed_fill_hex"));
            else {
                if (!indexes.isEmpty()) writeField(image, indexes, index.value("field").toString(), recordAt(image, indexes, i), i,
                           add(integer(index.value("first")), multiply(i, integer(index.value("step")))));
                applyWrites(image, table, address, i, rows[i], init.value("active_writes").toArray());
                for (const auto &v : init.value("active_clear_ranges").toArray()) {
                    const auto clear = v.toObject(); fill(image, add(address, integer(clear.value("offset"))), integer(clear.value("bytes")), clear.value("fill_hex"));
                }
            }
        }
        preparedTables.insert(table);
    }
    void writeOverlappingFields(const QJsonObject &r)
    {
        const QString table = r.value("table").toString();
        require(preparedTables.contains(table) && !packedTables.contains(table), "Overlapping fields require completed initialization: " + table);
        const auto rows = wanted.value(editor->roleForTable(table));
        for (qsizetype i = 0; i < rows.size(); ++i) applyWrites(image, table, recordAt(image, table, i), i, rows[i], r.value("writes").toArray());
        packedTables.insert(table);
    }
    void validateWrites(const QJsonArray &writes, const QString &table)
    {
        require(!writes.isEmpty(), "Empty writer field list.");
        const QString role = table.isEmpty() ? QStringLiteral("radio") : editor->roleForTable(table);
        const auto fields = targetFields(role);
        for (const auto &entry : writes) {
            const auto w = object(entry, "write");
            keys(w, {"table", "field", "from_target", "clear_storage_hex", "only_if_supplied", "when", "value_from_target_array_length", "input_index_base", "stored_index_base", "following_bytes_hex", "use_schema_alphabet", "use_schema_space_code", "use_schema_padding_code", "pad_to_schema_count", "padding_value"});
            const auto f = fieldSchema(w.value("table").toString(table), w.value("field").toString());
            storageWidth(f);
            if (w.contains("pad_to_schema_count")) {
                require(w.value("pad_to_schema_count") == QJsonValue(true) && f.contains("count") && w.contains("padding_value"), "Invalid array padding rule.");
                integer(w.value("padding_value"));
            }
            require(w.contains("from_target") != w.contains("value_from_target_array_length"), "Write requires one target value source.");
            if (w.contains("from_target"))
                require(fields.contains(w.value("from_target").toString()), "Unknown target write semantic: " + w.value("from_target").toString());
            else require(role == "zones" && w.value("value_from_target_array_length") == QJsonValue("members"),
                         "Unknown target array-length semantic.");
            if (w.contains("only_if_supplied")) require(w.value("only_if_supplied").isBool(), "Invalid conditional write option.");
            if (w.contains("when")) { const auto when = object(w.value("when"), "write condition"); keys(when, {"target_field", "equals"}); require(fields.contains(when.value("target_field").toString()) && when.contains("equals"), "Invalid writer condition."); }
            for (const char *key : {"use_schema_alphabet", "use_schema_space_code", "use_schema_padding_code"})
                if (w.contains(key)) require(w.value(key) == QJsonValue(true), "Unsupported packed-name option.");
            if (w.contains("clear_storage_hex")) fillByte(w.value("clear_storage_hex"));
            if (w.contains("following_bytes_hex")) {
                require(table.isEmpty(), "following_bytes_hex is supported only for global writes.");
                Decoder::hex(w.value("following_bytes_hex"));
            }
        }
    }
    void validateExpression(const QJsonValue &value, int depth = 0)
    {
        require(depth < maximumDepth, "Writer expression depth exceeded.");
        if (!value.isObject()) { integer(value); return; }
        const auto e = value.toObject(); require(e.size() == 1, "Invalid writer expression.");
        const QString op = e.begin().key(); const auto operand = e.begin().value();
        if (op == "context") {
            require(operand.isString() && contextPositions.contains(operand.toString()), "Undefined writer context.");
            require(contextPositions.value(operand.toString()) < validationPosition,
                    "Writer context must be produced before it is used: " + operand.toString());
        } else if (op == "base_count" || op == "target_count") {
            require(operand.isString() && wanted.contains(operand.toString()), "Undefined writer count table.");
        } else if (op == "image_size") require(operand == QJsonValue(true), "Invalid image-size expression.");
        else if (op == "field") require(d.value("fields").toObject().contains(operand.toString()), "Undefined writer global.");
        else {
            require(op == "add" || op == "subtract" || op == "multiply" || op == "max", "Unsupported writer expression: " + op);
            require(operand.isArray() && !operand.toArray().isEmpty() && (op != "subtract" || operand.toArray().size() == 2), "Invalid writer operands.");
            for (const auto &child : operand.toArray()) validateExpression(child, depth + 1);
        }
    }
    void validatePlan()
    {
        require(plan.contains("version") && plan.contains("order"),
                QStringLiteral("creation.construction lacks the executable writer version and operation order."));
        require(integer(plan.value("version")) == 1 && plan.value("order").isArray(), "Unsupported construction plan version.");
        const auto order = plan.value("order").toArray(); require(!order.isEmpty(), "Missing writer operations.");
        const QSet<QString> supportedOperations{
            "validate_target", "return_original_if_equal", "decode_representation", "write_global_fields",
            "branch_if_tables_equal", "rebuild_fixed_table", "replace_variable_table", "relocate_pointers",
            "rewrite_relative_index", "prepare_overlapping_table", "write_overlapping_fields",
            "encode_base_representation", "decode_and_compare_target", "remap_references"};
        QMap<QString, QVector<int>> positions;
        QMap<QString, int> labels;
        QSet<QString> seen;
        for (qsizetype i = 0; i < order.size(); ++i) {
            const QString label = order[i].toString();
            require(order[i].isString() && !label.isEmpty() && !seen.contains(label), "Invalid or duplicate construction step: " + label);
            const auto rule = object(plan.value(label), "construction." + label);
            const QString operation = operationName(label, rule);
            require(supportedOperations.contains(operation), "Unsupported writer operation: " + operation);
            seen.insert(label); labels.insert(label, i); positions[operation].append(i);
            if (operation == "replace_variable_table") {
                const auto saved = rule.value("save_context").toObject();
                for (const auto &name : saved.keys()) {
                    require(!contextPositions.contains(name), "Duplicate writer context: " + name);
                    contextPositions.insert(name, i);
                }
            }
        }
        editor.emplace(d);
        for (qsizetype i = 0; i < order.size(); ++i) {
            validationPosition = i;
            const QString label = order[i].toString();
            const auto r = plan.value(label).toObject();
            const QString operation = operationName(label, r);
            if (operation == "validate_target") {
                keys(r, {"operation", "model_and_cps_version", "channels", "zones", "contacts", "zone_members_maximum", "radio_id", "frequency_hz", "color_code", "digital_time_slots", "contact_id", "contact_reference", "zone_reference", "names", "bounds"});
                const auto eq = r.value("model_and_cps_version").toObject(); keys(eq, {"operation", "fields"}); policy(eq, "operation", "equal_to_base");
                requireNames(eq.value("fields"), {"model", "cps_version"}, "Incomplete base identity comparison.");
                const auto bounds = r.value("bounds").toObject(); keys(bounds, {"check_arithmetic_overflow", "require_storage_in_image", "require_capacity", "on_failure"});
                for (const char *key : {"check_arithmetic_overflow", "require_storage_in_image", "require_capacity"}) require(bounds.value(key) == QJsonValue(true), "Writer bounds checks cannot be disabled.");
                policy(bounds, "on_failure", "reject");
                keys(r.value("contact_reference").toObject(), {"target_table", "index_base", "null_value"});
                keys(r.value("zone_reference").toObject(), {"target_table", "input_index_base", "stored_index_base"});
                for (const char *name : {"contact_reference", "zone_reference"}) {
                    const auto ref = r.value(name).toObject();
                    require(wanted.contains(ref.value("target_table").toString()), "Unknown reference target table.");
                    for (const char *key : {"index_base", "input_index_base", "stored_index_base"})
                        if (ref.contains(key)) require(integer(ref.value(key)) >= 0, "Negative reference index base.");
                }
            } else if (operation == "return_original_if_equal") {
                keys(r, {"operation", "compare_radio_fields", "compare_tables", "compare_if_supplied", "ordered_tables", "ordered_array_fields"});
                requireNames(r.value("compare_radio_fields"), {"radio_name", "radio_id"}, "Incomplete radio comparison.");
                requireNames(r.value("ordered_tables"), {"channels", "contacts", "zones"}, "Incomplete table ordering policy.");
                require(r.value("ordered_array_fields").toObject() == QJsonObject{{"zones", QJsonArray{"members"}}}, "Unsupported array ordering policy.");
                const auto compared = r.value("compare_tables").toObject(); require(compared.size() == 3, "Incomplete record comparison.");
                const QMap<QString, QStringList> required{
                    {"channels", {"name", "transmit_frequency", "receive_frequency", "mode_marker", "color_code", "time_slot", "contact_number"}},
                    {"contacts", {"name", "id", "call_type"}}, {"zones", {"name", "members"}}};
                for (auto it = required.begin(); it != required.end(); ++it)
                    requireNames(compared.value(it.key()), it.value(), "Unsupported or incomplete record comparison.");
                require(r.value("compare_if_supplied").toObject() == QJsonObject{{"channels", QJsonArray{"power"}}}, "Unsupported optional-field comparison.");
            } else if (operation == "decode_representation") {
                keys(r, {"operation", "representation", "retain_original_decoded_image", "retain_original_encoding_parameters"}); policy(r, "representation", "matched_base");
                require(r.value("retain_original_decoded_image").toBool() && r.value("retain_original_encoding_parameters").toBool(), "Original data must be retained.");
            } else if (operation == "write_global_fields") {
                keys(r, {"operation", "writes"}); validateWrites(r.value("writes").toArray(), {});
            } else if (operation == "branch_if_tables_equal") {
                keys(r, {"operation", "tables", "next_step"}); requireNames(r.value("tables"), {"channels", "contacts", "zones"}, "Incomplete identity-only comparison."); require(operationName(r.value("next_step").toString(), object(plan.value(r.value("next_step").toString()), "branch destination")) == "encode_base_representation", "Identity-only branch must lead to encoding.");
            } else if (operation == "rebuild_fixed_table") {
                keys(r, {"operation", "count_field", "index_table", "names_table", "records_table", "index", "removed_records", "unused_records", "record_selection", "writes", "preserve_fields", "masked_field_writes", "capacity", "resize_to_end", "clear_ranges", "reset_selection"});
                require(r.value("unused_records") == QJsonValue("preserve") || r.value("unused_records") == QJsonValue("clear"), "Unknown unused-record policy.");
                if (r.value("unused_records") == QJsonValue("clear")) require(r.contains("capacity"), "Clearing unused records requires capacity.");
                if (r.contains("reset_selection")) require(r.value("reset_selection").isBool(), "Invalid selection reset option.");
                policy(r, "masked_field_writes", "preserve_unmasked_bits");
                if (r.contains("capacity")) require(integer(r.value("capacity")) > 0, "Invalid fixed-table capacity.");
                if (r.contains("resize_to_end")) require(r.value("resize_to_end").isBool(), "Invalid end-resize option.");
                for (const auto &entry : r.value("clear_ranges").toArray()) {
                    const auto c = object(entry, "record clear range"); keys(c, {"offset", "bytes", "fill_hex"});
                    const qint64 offset = integer(c.value("offset")), count = integer(c.value("bytes"));
                    require(offset >= 0 && count >= 0 && add(offset, count) <= stride(r.value("records_table").toString()), "Clear range exceeds record.");
                    fillByte(c.value("fill_hex"));
                }
                const auto preserved = r.value("preserve_fields").toArray();
                for (const auto &field : preserved) fieldSchema(r.value("records_table").toString(), field.toString());
                for (const auto &entry : r.value("writes").toArray()) {
                    const auto write = entry.toObject();
                    const QString table = write.value("table").toString(r.value("records_table").toString());
                    require(table != r.value("records_table").toString() || !preserved.contains(write.value("field")),
                            "A write conflicts with preserve_fields: " + write.value("field").toString());
                }
                keys(r.value("index").toObject(), {"field", "first", "step"});
                const auto removed = r.value("removed_records").toObject(); keys(removed, {"tables", "fill_hex", "range"}); policy(removed, "range", "target_count_to_base_count"); fillByte(removed.value("fill_hex"));
                validateWrites(r.value("writes").toArray(), r.value("records_table").toString());
                require(!r.value("record_selection").toArray().isEmpty(), "Missing record selection rules.");
                for (const auto &v : r.value("record_selection").toArray()) {
                    const auto choice = object(v, "record selection");
                    keys(choice, {"when_target_flag", "source", "match", "choose", "on_missing", "when_target_field_supplied", "index_from_target", "null_value", "require_match", "on_invalid", "index"});
                    require(choice.value("source") == QJsonValue("base") || choice.value("source") == QJsonValue("packaged_template"), "Unknown record source.");
                    if (choice.contains("choose")) policy(choice, "choose", "first");
                    if (choice.contains("index")) policy(choice, "index", "target_index");
                    for (const char *key : {"on_missing", "on_invalid"}) if (choice.contains(key)) require(choice.value(key) == QJsonValue("continue") || choice.value(key) == QJsonValue("reject"), "Unknown record-selection failure policy.");
                }
            } else if (operation == "remap_references") {
                keys(r, {"operation", "lists", "globals", "image_size_fields"});
                for (const auto &v : r.value("lists").toArray()) {
                    const auto x = object(v, "reference list"); keys(x, {"table", "field", "count_field", "on_removed", "count_bias"});
                    if (x.contains("count_bias")) integer(x.value("count_bias"));
                    policy(x, "on_removed", "remove_member");
                    const auto f = fieldSchema(x.value("table").toString(), x.value("field").toString());
                    require(f.contains("count") && f.contains("reference"), "Reference list lacks count or reference schema.");
                    editor->roleForTable(f.value("reference").toObject().value("table").toString());
                    fieldSchema(x.value("table").toString(), x.value("count_field").toString());
                }
                for (const auto &v : r.value("globals").toArray()) {
                    const auto x = object(v, "reference global"); keys(x, {"field", "target_role", "on_removed"});
                    policy(x, "on_removed", "first_record");
                    fieldSchema({}, x.value("field").toString());
                    require(wanted.contains(x.value("target_role").toString()), "Unknown global reference role.");
                }
                for (const auto &v : r.value("image_size_fields").toArray()) fieldSchema({}, v.toString());
            } else if (operation == "replace_variable_table") {
                keys(r, {"operation", "table", "fields", "record_size_from", "replace_span", "preserve_following_bytes", "save_context"});
                policy(r, "replace_span", "base_records_only"); require(r.value("preserve_following_bytes") == QJsonValue(true), "Following bytes must be preserved.");
                require(r.value("record_size_from").toString() == "tables." + r.value("table").toString() + ".record_bytes", "Unsupported variable record sizing source.");
                validateWrites(r.value("fields").toArray(), r.value("table").toString());
                const auto save = r.value("save_context").toObject();
                require(r.value("save_context").isObject(), "Variable-table save_context must be an object.");
                for (auto it = save.begin(); it != save.end(); ++it)
                    require(it.value() == QJsonValue("new_span_bytes_minus_base_span_bytes") || it.value() == QJsonValue("base_span_end"), "Unknown saved writer context.");
            } else if (operation == "relocate_pointers") {
                keys(r, {"operation", "pointer_directory", "additional_pointer_field", "read_from", "delta", "constraints"}); policy(r, "read_from", "original_decoded_image");
                keys(r.value("pointer_directory").toObject(), {"first_at", "last_at", "stride_bytes", "type", "skip_value"});
                keys(r.value("constraints").toObject(), {"base_minimum", "relocated_minimum", "relocated_maximum_exclusive", "additional_pointer_minimum"});
                const auto pointers = r.value("pointer_directory").toObject();
                const qint64 first = integer(pointers.value("first_at")), last = integer(pointers.value("last_at")), step = integer(pointers.value("stride_bytes"));
                const int width = storageWidth(QJsonObject{{"type", pointers.value("type")}});
                require(first >= 0 && last >= first && step >= width && last <= source.size() - width
                            && (last - first) / step < maximumItems, "Invalid pointer directory bounds.");
                validateExpression(r.value("delta"));
                const auto limits = r.value("constraints").toObject();
                validateExpression(limits.value("base_minimum")); validateExpression(limits.value("relocated_maximum_exclusive"));
                require(integer(limits.value("relocated_minimum")) >= 0 && integer(limits.value("additional_pointer_minimum")) >= 0, "Invalid relocation bounds.");
            } else if (operation == "rewrite_relative_index") {
                keys(r, {"operation", "count_field", "index_table", "records_table", "index_field", "implicit_first_offset", "first_indexed_record", "offset_origin", "clear_entries", "clear_minimum", "fill_hex"}); policy(r, "offset_origin", "table_start");
                validateExpression(r.value("clear_entries"));
                require(integer(r.value("implicit_first_offset")) >= 0 && integer(r.value("first_indexed_record")) >= 0
                            && integer(r.value("clear_minimum")) >= 0, "Unsupported relative-index bounds.");
            } else if (operation == "prepare_overlapping_table") {
                keys(r, {"operation", "count_field", "index_table", "records_table", "capacity", "index", "initialize", "complete_before"}); require(operationName(r.value("complete_before").toString(), object(plan.value(r.value("complete_before").toString()), "overlap packing step")) == "write_overlapping_fields", "Overlapping initialization must complete before packing.");
                const auto cap = r.value("capacity").toObject(); keys(cap, {"prefix_bytes", "directory_end_maximum", "trailing_strides_required"}); policy(cap, "directory_end_maximum", "record_table_start_minus_prefix_bytes");
                require(integer(cap.value("prefix_bytes")) >= 0 && integer(cap.value("trailing_strides_required")) >= 0, "Invalid contact-capacity bounds.");
                keys(r.value("index").toObject(), {"field", "first", "step", "removed_fill_hex"});
                const auto init = r.value("initialize").toObject(); keys(init, {"zero_physical_strides", "preserve_existing_prefix", "active_writes", "active_clear_ranges"});
                require(init.value("preserve_existing_prefix") == QJsonValue(true), "Existing record prefixes must be preserved.");
                require(init.value("zero_physical_strides").isArray(), "Stride initialization requires an explicit array.");
                QSet<QString> cleared;
                for (const auto &entry : init.value("zero_physical_strides").toArray()) {
                    require(entry.isString() && (entry.toString() == "new" || entry.toString() == "removed")
                                && !cleared.contains(entry.toString()), "Unsupported or duplicate stride initialization policy.");
                    cleared.insert(entry.toString());
                }
                validateWrites(init.value("active_writes").toArray(), r.value("records_table").toString());
                for (const auto &clear : init.value("active_clear_ranges").toArray()) {
                    const auto range = clear.toObject(); keys(range, {"offset", "bytes", "fill_hex"});
                    const qint64 offset = integer(range.value("offset")), size = integer(range.value("bytes"));
                    require(offset >= 0 && size >= 0 && add(offset, size) <= stride(r.value("records_table").toString()), "Invalid contact initialization bounds.");
                    fillByte(range.value("fill_hex"));
                }
            } else if (operation == "write_overlapping_fields") {
                keys(r, {"operation", "table", "writes", "preserve_bits_outside_fields", "physical_stride_initialization_after_step"});
                require(r.value("preserve_bits_outside_fields") == QJsonValue(true), "Unmapped bits must be preserved."); policy(r, "physical_stride_initialization_after_step", "forbidden");
                validateWrites(r.value("writes").toArray(), r.value("table").toString());
            } else if (operation == "encode_base_representation") {
                keys(r, {"operation", "encoding_parameters", "update_timestamp", "additional_checksum"}); policy(r, "encoding_parameters", "saved_original_base"); policy(r, "additional_checksum", "none");
                require(r.value("update_timestamp") == QJsonValue(false), "Timestamp updates are unsupported.");
            } else if (operation == "decode_and_compare_target") {
                keys(r, {"operation", "decoder", "require_layout_checks", "require_reference_checks", "compare_as", "compare_radio_fields", "unspecified_power"}); policy(r, "decoder", "same_definition"); policy(r, "unspecified_power", "preserve_selected_record");
                require(r.value("require_layout_checks").toBool() && r.value("require_reference_checks").toBool(), "Output validation cannot be disabled.");
                require(operationName(r.value("compare_as").toString(), object(plan.value(r.value("compare_as").toString()), "output comparison")) == "return_original_if_equal", "Output must use the complete target comparison.");
                requireNames(r.value("compare_radio_fields"), {"radio_name", "radio_id"}, "Incomplete output identity comparison.");
            }
        }
        const QStringList singletonOperations{"validate_target", "return_original_if_equal", "decode_representation",
                                              "encode_base_representation", "decode_and_compare_target"};
        for (const auto &operation : singletonOperations)
            require(positions.value(operation).size() == 1, "Construction plan requires exactly one operation: " + operation);
        require(positions.contains("write_global_fields"), "Construction plan lacks radio identity writes.");
        const int decoder = positions.value("decode_representation").first();
        const int encoder = positions.value("encode_base_representation").first();
        require(positions.value("validate_target").first() == 0
                    && positions.value("decode_and_compare_target").first() == order.size() - 1,
                "Target validation must be first and output validation last.");
        require(positions.value("return_original_if_equal").first() < decoder && decoder < encoder,
                "Invalid construction operation order: comparison, decoding, encoding.");
        const QSet<QString> structural(singletonOperations.cbegin(), singletonOperations.cend());
        for (auto operation = positions.begin(); operation != positions.end(); ++operation) {
            if (structural.contains(operation.key())) continue;
            for (const int position : operation.value())
                require(position > decoder && position < encoder, "Invalid construction operation order: " + operation.key());
        }
        for (const int position : positions.value("branch_if_tables_equal")) {
            const auto rule = plan.value(order[position].toString()).toObject();
            require(labels.contains(rule.value("next_step").toString()) && labels.value(rule.value("next_step").toString()) > position,
                    "Invalid construction branch destination.");
            require(positions.value("write_global_fields").last() < position,
                    "Radio identity writes must precede a tables-only branch.");
        }
        QSet<QString> initialized, packed;
        for (int i = 0; i < order.size(); ++i) {
            const QString label = order[i].toString();
            const auto rule = plan.value(label).toObject();
            const QString operation = operationName(label, rule);
            const QString table = rule.value("records_table").toString(rule.value("table").toString());
            if (operation == "prepare_overlapping_table") {
                const QString destination = rule.value("complete_before").toString();
                require(labels.contains(destination) && labels.value(destination) > i,
                        "Invalid construction operation order: initialization must precede its packing step.");
                require(plan.value(destination).toObject().value("table").toString() == table,
                        "Overlapping initialization and packing identify different tables.");
                require(!initialized.contains(table), "Duplicate overlapping initialization: " + table);
                initialized.insert(table);
            } else if (operation == "write_overlapping_fields") {
                require(initialized.contains(table) && !packed.contains(table),
                        "Invalid construction operation order: overlapping packing requires initialization for " + table);
                packed.insert(table);
            } else if (operation == "rebuild_fixed_table" || operation == "replace_variable_table") {
                require(!packed.contains(table), "Physical writes after overlapping packing are forbidden: " + table);
            }
        }
        require(initialized == packed, "Every overlapping initialization requires its matching packing step.");
        for (auto it = plan.begin(); it != plan.end(); ++it)
            require(seen.contains(it.key()) || it.key() == "version" || it.key() == "default_generation" || it.key() == "order", "Unknown construction rule: " + it.key());
        const auto base = d.value("creation").toObject().value("base_image").toObject(); keys(base, {"new", "imported", "retain_unknown_bytes", "retain_unused_space"});
        policy(base, "new", "packaged_template"); policy(base, "imported", "complete_original_input");
        require(base.value("retain_unknown_bytes").toBool() && base.value("retain_unused_space").toBool(), "Source preservation cannot be disabled.");
        if (d.value("creation").toObject().contains("channel_defaults")) {
            const auto defaults = object(d.value("creation").toObject().value("channel_defaults"), "creation.channel_defaults");
            keys(defaults, {"table", "selection", "record_bytes", "mode_field", "modes", "base", "missing_mode"});
            policy(defaults, "selection", "first_matching_mode"); policy(defaults, "base", "packaged_template"); policy(defaults, "missing_mode", "reject");
            require(defaults.value("table").toString() == editor->channels
                        && integer(defaults.value("record_bytes")) == stride(defaults.value("table").toString()), "Default channel record sizing differs from its table.");
            fieldSchema(defaults.value("table").toString(), defaults.value("mode_field").toString());
            const auto modes = defaults.value("modes").toObject(); keys(modes, {"analog", "digital"});
            const auto enums = fieldSchema(defaults.value("table").toString(), defaults.value("mode_field").toString()).value("enum").toObject();
            for (auto it = modes.begin(); it != modes.end(); ++it) {
                require(!it.value().toArray().isEmpty(), "Missing default mode markers.");
                for (const auto &marker : it.value().toArray())
                    require(enums.value(QString::number(integer(marker))) == QJsonValue(it.key()), "Default mode markers disagree with field encoding.");
            }
        }
    }

    const QJsonObject &d;
    const QByteArray &templateInput, &source;
    const ImportedCodeplug &target;
    Records wanted, original;
    Record globals, baseGlobals;
    QJsonObject plan, representation;
    QByteArray baseImage, templateImage, image, output;
    ImportedCodeplug basePlug, templatePlug;
    QMap<QString, qint64> context;
    QMap<QString, QMap<qint64, qint64>> sourcePositions;
    QMap<QString, QVector<Row>> baseRecordCache, templateRecordCache;
    QMap<QString, QVector<qint64>> baseOrderCache, templateOrderCache;
    std::optional<EditorBindings> editor;
    QSet<QString> replacedTables, preparedTables, packedTables;
    QMap<QString, int> contextPositions;
    int validationPosition = -1;
    bool decoded = false, validated = false;
};

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
QByteArray CodeplugDefinition::iconPng() const { return m_iconPng; }
bool CodeplugDefinition::defaultForNew() const { return m_definition.value("creation").toObject().value("default_for_new").toBool(); }

QStringList CodeplugDefinition::extensions() const
{
    QStringList result;
    for (const auto &r : m_definition.value("representations").toArray()) {
        const QString extension = r.toObject().value("extension").toString();
        if (!extension.isEmpty() && !result.contains(extension)) result.append(extension);
    }
    return result;
}

QString CodeplugDefinition::diskProvenance() const
{
    return QStringLiteral("Definition read from disk: %1\nDefinition ID: %2\ndefinition.json SHA-256: %3")
        .arg(m_diskPath, id(), QString::fromLatin1(m_definitionSha256));
}

bool CodeplugDefinition::exportExtension(const QByteArray &source,
                                        QString *extension, QString *error) const
{
    return checked(error, [&] {
        require(extension, QStringLiteral("Missing export extension output."));
        QSet<QByteArray> images;
        QString selected;
        QStringList reasons;
        for (const auto &entry : m_definition.value("representations").toArray()) {
            const auto representation = entry.toObject();
            try {
                const auto image = decode(source, representation);
                Decoder reader(image, m_definition);
                if (reader.model() != modelName()) continue;
                reader.imported();
                const auto digest = QCryptographicHash::hash(image, QCryptographicHash::Sha256);
                if (images.contains(digest)) continue;
                images.insert(digest);
                selected = representation.value("extension").toString();
            } catch (const std::exception &exception) {
                reasons.append(QStringLiteral("%1: %2")
                    .arg(representation.value("name").toString(),
                         QString::fromUtf8(exception.what())));
            }
        }
        require(!images.isEmpty(),
                QStringLiteral("No representation can decode the export base. %1").arg(reasons.join("; ")));
        require(images.size() == 1, QStringLiteral("Export base has ambiguous representations."));
        require(!selected.isEmpty(),
                QStringLiteral("The matching representation declares no filename extension."));
        *extension = selected;
    });
}

bool CodeplugDefinition::load(const QString &path, CodeplugDefinition *result, QString *error)
{
    return checked(error, [&] {
        require(result, QStringLiteral("Missing definition output."));
        const QString diskPath = QFileInfo(path).absoluteFilePath();
        std::fprintf(stdout, "[definitions] Reading definition FILE: %s\n", qUtf8Printable(diskPath));
        std::fflush(stdout);
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
        loaded.m_iconPng = entries.value("icon.png");
        loaded.m_diskPath = diskPath;
        loaded.m_definitionSha256 = QCryptographicHash::hash(entries.value("definition.json"), QCryptographicHash::Sha256).toHex();
        std::fprintf(stdout, "[definitions] Read definition.json from FILE: %s\n[definitions] definition.json SHA-256: %s\n",
                     qUtf8Printable(diskPath), loaded.m_definitionSha256.constData());
        std::fflush(stdout);
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
                Decoder(image, d).validate(); // Binary validation does not require editable-record semantics.
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
                Decoder reader(image, m_definition);
                reader.recognize();
                return;
            } catch (const std::exception &e) { reasons.append(QString::fromUtf8(e.what())); }
        }
        reject(QStringLiteral("No representation matches model %1. %2").arg(modelName(), reasons.join(' ')));
    });
}

bool CodeplugDefinition::inspect(const QByteArray &input, QJsonObject *result, QString *error) const
{
    return checked(error, [&] {
        require(result, "Missing binary inspection output.");
        QSet<QByteArray> images;
        QJsonObject decoded;
        QStringList reasons;
        for (const auto &entry : m_definition.value("representations").toArray()) {
            try {
                const auto image = decode(input, entry.toObject());
                const auto digest = QCryptographicHash::hash(image, QCryptographicHash::Sha256);
                if (images.contains(digest)) continue;
                auto candidate = Decoder(image, m_definition).inspection();
                images.insert(digest);
                decoded = std::move(candidate);
            } catch (const std::exception &exception) {
                reasons.append(QString::fromUtf8(exception.what()));
            }
        }
        require(images.size() == 1, "Binary interpretation is invalid or ambiguous: " + reasons.join("; "));
        *result = std::move(decoded);
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

bool CodeplugDefinition::write(const QByteArray &source, const ImportedCodeplug &target,
                              const QString &radioName, int radioId, QByteArray *result, QString *error) const
{
    return checked(error, [&] {
        require(result, QStringLiteral("Missing export output."));
        auto output = Encoder(m_definition, m_defaultCodeplug, source, target, radioName, radioId).run();
        *result = std::move(output);
    });
}
