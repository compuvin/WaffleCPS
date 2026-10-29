#include "../src/codeplugdefinition.h"
#include "../src/codeplugdefinitions.h"
#include "../src/codeplugwriter.h"
#include "../src/cs7000default.h"
#include "../src/ziparchive.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QtEndian>
#include <functional>
#include <stdexcept>

namespace {
void check(bool ok, const QString &message) { if (!ok) throw std::runtime_error(message.toStdString()); }
QByteArray fileBytes(const QString &path) {
    QFile f(path); check(f.open(QIODevice::ReadOnly), "Cannot read " + path); return f.readAll();
}
QByteArray raw(QByteArray bytes) {
    const auto mask = bytes.mid(0x300, 256);
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = bytes[i] ^ mask[i % 256];
    return bytes;
}
void put32(QByteArray &b, qsizetype at, quint32 n) { qToLittleEndian(n, reinterpret_cast<uchar *>(b.data() + at)); }
quint32 get32(const QByteArray &b, qsizetype at) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + at)); }
void fixedLayoutRoundtrip(const QJsonObject &completeDefinition, const QString &path)
{
    QByteArray image(4096, '\0');
    const auto text = [&](int at, int size, const QByteArray &value) {
        QByteArray padded(size, '\0'); padded.replace(0, value.size(), value); image.replace(at, size, padded);
    };
    text(0, 16, "TABLE-TEST"); text(16, 8, "V1"); text(32, 16, "TEST RADIO");
    put32(image, 48, 3109101);
    for (const int at : {52, 54, 56, 64, 96, 128}) qToLittleEndian<quint16>(1, reinterpret_cast<uchar *>(image.data() + at));
    text(256, 16, "DCH 1");
    image.replace(272, 4, QByteArray::fromHex("00505214"));
    qToBigEndian<quint32>(145250000, reinterpret_cast<uchar *>(image.data() + 276));
    image[280] = 1; image[281] = 1; image[282] = 1; image[284] = 1; image[286] = 1;
    text(1024, 16, "LOCAL"); image[1040] = 9; image[1043] = 9;
    text(2048, 16, "ZONE 1"); image[2064] = 1; image[2066] = 1;
    image[3500] = char(0xb7);
    const auto field = [](int offset, const QString &type) { return QJsonObject{{"offset", offset}, {"type", type}}; };
    const auto ascii = [&](int offset, int bytes) { auto f = field(offset, "ascii"); f["bytes"] = bytes; return f; };
    const auto count = [](const QString &name) { return QJsonObject{{"field", name}}; };
    const auto ref = [](const QString &table) { return QJsonObject{{"table", table}, {"index_base", 1}}; };
    QJsonObject globals{{"type", QJsonObject{{"at", 0}, {"type", "ascii"}, {"bytes", 16}}},
                        {"version", QJsonObject{{"at", 16}, {"type", "ascii"}, {"bytes", 8}}},
                        {"owner", QJsonObject{{"at", 32}, {"type", "ascii"}, {"bytes", 16}}},
                        {"number", QJsonObject{{"at", 48}, {"type", "u32le"}}}};
    const QStringList tables{"bank_a", "bank_b", "bank_c"};
    const QStringList counts{"count_a", "count_b", "count_c"};
    const QStringList indexes{"order_a", "order_b", "order_c"};
    QJsonObject schemas;
    for (int i = 0; i < 3; ++i) {
        globals[counts[i]] = QJsonObject{{"at", 52 + 2 * i}, {"type", "u16le"}, {"minimum", 0}, {"maximum", 8}};
        auto number = field(0, "u16le"); number["reference"] = ref(tables[i]);
        schemas[indexes[i]] = QJsonObject{{"at", 64 + 32 * i}, {"count", count(counts[i])}, {"stride_bytes", 2},
                                         {"fields", QJsonObject{{"n", number}}}};
    }
    auto tx = field(16, "bcdle"); tx["bytes"] = 4; tx["scale"] = 10;
    auto mode = field(24, "u8"); mode["enum"] = QJsonObject{{"0", "analog"}, {"1", "digital"}};
    auto power = field(30, "u8"); power["enum"] = QJsonObject{{"0", "low"}, {"1", "high"}};
    const QJsonObject digital{{"value", QJsonObject{{"record_field", "signalling"}}}, {"in", QJsonArray{1}}};
    auto color = field(25, "u8"); color["only_if"] = digital;
    auto slot = field(26, "u8"); slot["only_if"] = digital;
    auto destination = field(28, "u16le"); destination["only_if"] = digital;
    auto contactRef = ref("bank_b"); contactRef["null_value"] = 0; destination["reference"] = contactRef;
    schemas["bank_a"] = QJsonObject{{"at", 256}, {"count", count("count_a")}, {"stride_bytes", 48},
        {"fields", QJsonObject{{"label", ascii(0, 16)}, {"out_hz", tx}, {"in_hz", field(20, "u32be")},
                              {"signalling", mode}, {"cc", color}, {"slot", slot}, {"destination", destination}, {"drive", power}}}};
    auto call = field(19, "u8"); call["enum"] = QJsonObject{{"0", "private"}, {"9", "group"}};
    schemas["bank_b"] = QJsonObject{{"at", 1024}, {"count", count("count_b")}, {"stride_bytes", 32},
        {"fields", QJsonObject{{"label", ascii(0, 16)}, {"identifier", field(16, "u24le")}, {"kind", call}}}};
    auto members = field(18, "u16le"); members["count"] = QJsonObject{{"record_field", "length"}};
    members["stride_bytes"] = 2; members["reference"] = ref("bank_a");
    auto length = field(16, "u8"); length["maximum"] = 8;
    schemas["bank_c"] = QJsonObject{{"at", 2048}, {"count", count("count_c")}, {"stride_bytes", 64},
        {"fields", QJsonObject{{"label", ascii(0, 16)}, {"length", length}, {"entries", members}}}};
    const auto write = [](const QString &field, const QString &target) {
        return QJsonObject{{"field", field}, {"from_target", target}};
    };
    const auto nameWrite = [&](const QString &field, const QString &target) {
        auto w = write(field, target); w["clear_storage_hex"] = "00"; return w;
    };
    QJsonArray channelWrites{nameWrite("label", "name"), write("out_hz", "transmit_frequency"), write("in_hz", "receive_frequency"),
                             write("signalling", "mode_marker"), write("drive", "power")};
    for (const auto &pair : QList<QPair<QString, QString>>{{"cc", "color_code"}, {"slot", "time_slot"}, {"destination", "contact_number"}}) {
        auto w = write(pair.first, pair.second);
        w["when"] = QJsonObject{{"target_field", "mode_marker"}, {"equals", "digital"}};
        channelWrites.append(w);
    }
    auto memberWrite = write("entries", "members"); memberWrite["input_index_base"] = 0; memberWrite["stored_index_base"] = 1;
    const QList<QJsonArray> writes{channelWrites,
        QJsonArray{nameWrite("label", "name"), write("identifier", "id"), write("kind", "call_type")},
        QJsonArray{nameWrite("label", "name"), QJsonObject{{"field", "length"}, {"value_from_target_array_length", "members"}}, memberWrite}};
    const auto originalPlan = completeDefinition["creation"].toObject()["construction"].toObject();
    QJsonObject plan;
    for (const QString &name : {QStringLiteral("validate_target"), QStringLiteral("unchanged_copy"), QStringLiteral("decode_base"),
                               QStringLiteral("identity_only_output"), QStringLiteral("encode"), QStringLiteral("validate_output")})
        plan[name] = originalPlan[name];
    // These lists declare coverage, not a magic field order from one file.
    const auto reverse = [](const QJsonArray &array) {
        QJsonArray result;
        for (qsizetype i = array.size(); i > 0; --i) result.append(array[i - 1]);
        return result;
    };
    auto comparison = plan["unchanged_copy"].toObject();
    comparison["compare_radio_fields"] = reverse(comparison["compare_radio_fields"].toArray());
    comparison["ordered_tables"] = reverse(comparison["ordered_tables"].toArray());
    auto comparedTables = comparison["compare_tables"].toObject();
    for (const auto &name : comparedTables.keys()) comparedTables[name] = reverse(comparedTables[name].toArray());
    comparison["compare_tables"] = comparedTables; plan["unchanged_copy"] = comparison;
    auto validation = plan["validate_output"].toObject();
    validation["compare_radio_fields"] = reverse(validation["compare_radio_fields"].toArray());
    plan["validate_output"] = validation;
    auto limits = plan["validate_target"].toObject();
    for (const QString &name : {QStringLiteral("channels"), QStringLiteral("contacts"), QStringLiteral("zones")}) {
        auto limit = limits[name].toObject(); limit["maximum"] = 8; limits[name] = limit;
    }
    limits["zone_members_maximum"] = 8; plan["validate_target"] = limits;
    plan["radio_identity"] = QJsonObject{{"operation", "write_global_fields"},
        {"writes", QJsonArray{nameWrite("owner", "radio_name"), write("number", "radio_id")}}};
    for (int i = 0; i < 3; ++i) {
        plan["fixed_" + QString::number(i)] = QJsonObject{{"operation", "rebuild_fixed_table"},
            {"records_table", tables[i]}, {"index_table", indexes[i]}, {"count_field", counts[i]},
            {"index", QJsonObject{{"field", "n"}, {"first", 1}, {"step", 1}}},
            {"removed_records", QJsonObject{{"tables", QJsonArray{tables[i], indexes[i]}}, {"fill_hex", "00"}, {"range", "target_count_to_base_count"}}},
            {"unused_records", "preserve"}, {"masked_field_writes", "preserve_unmasked_bits"}, {"preserve_fields", QJsonArray{}},
            {"record_selection", QJsonArray{
                QJsonObject{{"source", "base"}, {"match", QJsonArray{"name"}}, {"choose", "first"}, {"on_missing", "continue"}},
                QJsonObject{{"source", "packaged_template"}, {"match", QJsonArray{}}, {"choose", "first"}, {"on_missing", "reject"}}}},
            {"writes", writes[i]}};
    }
    plan["version"] = 1;
    plan["order"] = QJsonArray{"validate_target", "unchanged_copy", "decode_base", "radio_identity", "identity_only_output",
                                "fixed_1", "fixed_2", "fixed_0", "encode", "validate_output"};
    auto creation = completeDefinition["creation"].toObject();
    creation.remove("channel_defaults"); creation["construction"] = plan;
    creation["template"] = "templates/default.test"; creation["representation"] = "file"; creation["template_bytes"] = image.size();
    creation["template_sha256"] = QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex());
    QJsonObject definition{{"definition_type", "codeplug"}, {"definition_version", 2}, {"id", "fixed-layout-test"},
        {"fields", globals}, {"tables", schemas}, {"layout_checks", QJsonObject{}}, {"creation", creation},
        {"recognition", QJsonObject{{"model_field", "type"}, {"model_value", "TABLE-TEST"}, {"version_field", "version"}}},
        {"representations", QJsonArray{QJsonObject{{"name", "file"}, {"extension", ".test"}, {"decode", QJsonObject{{"operation", "identity"}}}}}}};
    QString error;
    bool ok = ZipArchive::write(path, {{"definition.json", QJsonDocument(definition).toJson()}, {"templates/default.test", image}}, &error); check(ok, error);
    CodeplugDefinition parser;
    ok = CodeplugDefinition::load(path, &parser, &error); check(ok, error);
    ImportedCodeplug target;
    ok = parser.read(image, &target, &error); check(ok, error);
    check(target.channels[0].frequencyAHz == 145250000 && target.channels[0].frequencyBHz == 145250000, "Independent BCD/endian fixture decoded incorrectly");
    target.channels[0].frequencyAHz = 449350000; target.channels[0].frequencyBHz = 444350000;
    target.channels[0].highPower = false; target.channels[0].contactIndex = 2;
    auto added = target.channels[0]; added.name = "SECOND"; added.digital = false;
    added.contactIndex = 0; added.colorCode = 0; added.timeSlot = 0; added.sourceRecord = -1;
    target.channels.append(added); target.contacts.append({"STATE", 3100, true});
    target.zones[0].channelIndexes = {1}; target.zones.append({"SECOND ZONE", {1, 0}});
    QByteArray output;
    ok = parser.write(image, target, "EDITED RADIO", 3109102, &output, &error); check(ok, error);
    ImportedCodeplug checked;
    ok = parser.read(output, &checked, &error); check(ok, error);
    check(checked.channels.size() == 2 && checked.contacts.size() == 2 && checked.zones.size() == 2
          && checked.channels[0].frequencyAHz == 449350000 && checked.channels[0].frequencyBHz == 444350000
          && checked.channels[0].contactIndex == 2 && !checked.channels[0].highPower.value_or(true)
          && !checked.channels[1].digital && checked.zones[1].channelIndexes == QVector<int>{1, 0}
          && checked.contacts[1].id == 3100 && output[3500] == char(0xb7), "Independent fixed-layout export lost data");
    // A second construction shape uses two independent variable tables and
    // two relocation commands. Each command has its own declared context.
    auto variableImage = image;
    put32(variableImage, 60, 3500);
    put32(variableImage, 148, 2048);
    put32(variableImage, 152, 1024);
    auto variableGlobals = globals;
    variableGlobals["tail"] = QJsonObject{{"at", 60}, {"type", "u32le"}};
    variableGlobals["zone_start"] = QJsonObject{{"at", 148}, {"type", "u32le"}};
    variableGlobals["contact_start"] = QJsonObject{{"at", 152}, {"type", "u32le"}};
    auto variableSchemas = schemas;
    auto variablePlan = plan;
    for (int i = 1; i <= 2; ++i) {
        auto schema = variableSchemas[tables[i]].toObject();
        schema.remove("stride_bytes");
        if (i == 1) {
            schema["at"] = QJsonObject{{"field", "contact_start"}};
            schema["record_bytes"] = QJsonObject{{"add", QJsonArray{32, QJsonObject{{"record_field", "kind"}}}}};
        } else {
            schema["at"] = QJsonObject{{"field", "zone_start"}};
            schema["record_bytes"] = QJsonObject{{"add", QJsonArray{20,
                QJsonObject{{"multiply", QJsonArray{2, QJsonObject{{"record_field", "length"}}}}}}}};
        }
        variableSchemas[tables[i]] = schema;
        variablePlan.remove("fixed_" + QString::number(i));
        const QString prefix = "context_" + QString::number(i);
        variablePlan["variable_" + QString::number(i)] = QJsonObject{{"operation", "replace_variable_table"},
            {"table", tables[i]}, {"fields", writes[i]}, {"record_size_from", "tables." + tables[i] + ".record_bytes"},
            {"replace_span", "base_records_only"}, {"preserve_following_bytes", true},
            {"save_context", QJsonObject{{prefix + "_delta", "new_span_bytes_minus_base_span_bytes"}, {prefix + "_end", "base_span_end"}}}};
        variablePlan["relocate_" + QString::number(i)] = QJsonObject{{"operation", "relocate_pointers"},
            {"pointer_directory", QJsonObject{{"first_at", i == 1 ? 148 : 3800}, {"last_at", i == 1 ? 148 : 3800}, {"stride_bytes", 4}, {"type", "u32le"}, {"skip_value", 0}}},
            {"additional_pointer_field", "tail"}, {"read_from", "original_decoded_image"},
            {"delta", i == 1 ? QJsonObject{{"context", prefix + "_delta"}}
                              : QJsonObject{{"add", QJsonArray{QJsonObject{{"context", "context_1_delta"}}, QJsonObject{{"context", "context_2_delta"}}}}}},
            {"constraints", QJsonObject{{"base_minimum", QJsonObject{{"context", prefix + "_end"}}},
                                        {"relocated_minimum", 0}, {"relocated_maximum_exclusive", QJsonObject{{"image_size", true}}},
                                        {"additional_pointer_minimum", 1}}}};
    }
    variablePlan["order"] = QJsonArray{"validate_target", "unchanged_copy", "decode_base", "radio_identity", "identity_only_output",
                                        "variable_1", "relocate_1", "variable_2", "relocate_2", "fixed_0", "encode", "validate_output"};
    auto variableCreation = creation;
    variableCreation["construction"] = variablePlan;
    variableCreation["template_sha256"] = QString::fromLatin1(QCryptographicHash::hash(variableImage, QCryptographicHash::Sha256).toHex());
    auto variableDefinition = definition;
    variableDefinition["fields"] = variableGlobals; variableDefinition["tables"] = variableSchemas;
    variableDefinition["creation"] = variableCreation;
    ok = ZipArchive::write(path, {{"definition.json", QJsonDocument(variableDefinition).toJson()}, {"templates/default.test", variableImage}}, &error); check(ok, error);
    CodeplugDefinition variableParser;
    ok = CodeplugDefinition::load(path, &variableParser, &error); check(ok, error);
    ImportedCodeplug variableTarget;
    ok = variableParser.read(variableImage, &variableTarget, &error); check(ok, error);
    variableTarget.channels.append(added);
    variableTarget.contacts[0].id = 12345; variableTarget.contacts[0].name = "CHANGED";
    variableTarget.contacts[0].groupCall = false; // Raw enum 9 -> 0 shrinks this record by nine bytes.
    variableTarget.zones[0].name = "VARIABLE ZONE"; variableTarget.zones[0].channelIndexes = {1, 0};
    QByteArray variableOutput;
    ok = variableParser.write(variableImage, variableTarget, "VARIABLE RADIO", 3109102, &variableOutput, &error); check(ok, error);
    ImportedCodeplug variableChecked;
    ok = variableParser.read(variableOutput, &variableChecked, &error); check(ok, error);
    check(variableChecked.contacts[0].id == 12345 && variableChecked.zones[0].channelIndexes == QVector<int>{1, 0}
          && !variableChecked.contacts[0].groupCall && variableChecked.channels.size() == 2
          && variableOutput.size() == variableImage.size() - 7 && get32(variableOutput, 148) == 2039
          && get32(variableOutput, 60) == 3493 && variableOutput[3493] == char(0xb7),
          "Repeated variable/relocation operations lost their table bindings or contexts");
    for (const bool duplicate : {true, false}) {
        auto malformedPlan = variablePlan;
        if (duplicate) {
            auto rule = malformedPlan["variable_2"].toObject();
            rule["save_context"] = QJsonObject{{"context_1_delta", "new_span_bytes_minus_base_span_bytes"}};
            malformedPlan["variable_2"] = rule;
        } else {
            auto rule = malformedPlan["relocate_1"].toObject(); rule["delta"] = QJsonObject{{"context", "context_2_delta"}};
            malformedPlan["relocate_1"] = rule;
        }
        auto malformedCreation = variableCreation; malformedCreation["construction"] = malformedPlan;
        auto malformedDefinition = variableDefinition; malformedDefinition["creation"] = malformedCreation;
        ok = ZipArchive::write(path, {{"definition.json", QJsonDocument(malformedDefinition).toJson()}, {"templates/default.test", variableImage}}, &error); check(ok, error);
        CodeplugDefinition malformed;
        ok = CodeplugDefinition::load(path, &malformed, &error); check(ok, error);
        QByteArray untouched("sentinel");
        ok = malformed.write(variableImage, variableChecked, "VARIABLE RADIO", 3109102, &untouched, &error);
        check(!ok && untouched == "sentinel" && error.contains("context", Qt::CaseInsensitive),
              "Duplicate or forward writer context was accepted: " + error);
    }
    qInfo() << "Repeated variable-table replacements and relocations: independent contexts and invalid-context rejection passed";
    {
        auto unwrittenDefinition = variableDefinition;
        auto unwrittenSchemas = variableSchemas;
        auto table = unwrittenSchemas["bank_b"].toObject();
        auto fields = table["fields"].toObject(); fields["unwritten_length"] = field(28, "u8");
        table["fields"] = fields;
        table["record_bytes"] = QJsonObject{{"add", QJsonArray{32, QJsonObject{{"record_field", "unwritten_length"}}}}};
        unwrittenSchemas["bank_b"] = table; unwrittenDefinition["tables"] = unwrittenSchemas;
        ok = ZipArchive::write(path, {{"definition.json", QJsonDocument(unwrittenDefinition).toJson()}, {"templates/default.test", variableImage}}, &error); check(ok, error);
        CodeplugDefinition unwritten;
        ok = CodeplugDefinition::load(path, &unwritten, &error); check(ok, error);
        QByteArray untouched("sentinel");
        ok = unwritten.write(variableImage, variableChecked, "VARIABLE RADIO", 3109102, &untouched, &error);
        check(!ok && untouched == "sentinel" && error.contains("not declared by its writes"),
              "An undeclared record-size target value was guessed: " + error);
    }
    auto invalid = definition;
    auto invalidCreation = creation; auto invalidPlan = plan;
    auto writesTable = invalidPlan["fixed_1"].toObject();
    auto badWrites = writesTable["writes"].toArray();
    auto wrong = badWrites[1].toObject(); wrong["from_target"] = "unknown_meaning"; badWrites[1] = wrong;
    writesTable["writes"] = badWrites; invalidPlan["fixed_1"] = writesTable;
    invalidCreation["construction"] = invalidPlan; invalid["creation"] = invalidCreation;
    ok = ZipArchive::write(path, {{"definition.json", QJsonDocument(invalid).toJson()}, {"templates/default.test", image}}, &error); check(ok, error);
    CodeplugDefinition incomplete;
    ok = CodeplugDefinition::load(path, &incomplete, &error); check(ok, error);
    ImportedCodeplug untouched; untouched.model = "sentinel";
    ok = incomplete.read(image, &untouched, &error);
    check(!ok && untouched.model == "sentinel" && error.contains("contacts"), "Missing target semantics were guessed");
    qInfo() << "Independent layout: repeated fixed-table operations for contacts/zones/channels, BCD/endian writes and missing-semantic rejection passed";
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString root = QStringLiteral(WAFFLE_SOURCE_DIR);
        const QString path = root + "/definitions/cs7000-m17-plus.wcp";
        QString error;
        if (argc == 2 && QByteArray(argv[1]) == "--fixed-layout") {
            QTemporaryDir temporary;
            check(temporary.isValid(), "No temporary directory");
            QMap<QString, QByteArray> entries;
            const bool loaded = ZipArchive::read(path, &entries, &error); check(loaded, error);
            fixedLayoutRoundtrip(QJsonDocument::fromJson(entries["definition.json"]).object(), temporary.filePath("fixed.wcp"));
            return 0;
        }
        CodeplugDefinition definition;
        bool ok = CodeplugDefinition::load(path, &definition, &error); check(ok, error);
        check(CodeplugDefinitions::defaultCodeplug(&error) == definition.defaultCodeplug(), "Catalog default differs from packaged template");
        ImportedCodeplug defaults;
        ok = definition.read(definition.defaultCodeplug(), &defaults, &error); check(ok, error);
        QByteArray source;
        ok = CodeplugWriter::write(definition.defaultCodeplug(), defaults, "TEST RADIO", 3109101, &source, &error); check(ok, error);
        // Unmapped bytes and unmasked channel bits must survive the new writer.
        source[0x600] = char(source[0x600] ^ 0x5a);
        source[0x23768 + 13] = char(source[0x23768 + 13] ^ 0xa5);
        source[0x23768 + 48 + 16] = char(source[0x23768 + 48 + 16] ^ 0x60);
        ImportedCodeplug base;
        ok = definition.read(source, &base, &error); check(ok, error);
        int cases = 0;
        const auto parity = [&](const QByteArray &input, const ImportedCodeplug &target, const QString &name, int id, const QString &label) {
            const auto untouched = input;
            QByteArray expected, actual("sentinel");
            bool success = CodeplugWriter::write(input, target, name, id, &expected, &error);
            check(success, label + ": existing writer: " + error);
            success = definition.write(input, target, name, id, &actual, &error);
            check(success, label + ": definition writer: " + error);
            if (actual != expected) {
                qsizetype first = 0;
                while (first < std::min(actual.size(), expected.size()) && actual[first] == expected[first]) ++first;
                throw std::runtime_error((label + QStringLiteral(": byte mismatch at %1, sizes %2/%3").arg(first).arg(actual.size()).arg(expected.size())).toStdString());
            }
            check(input == untouched, label + ": input changed");
            ImportedCodeplug parsed;
            success = definition.read(actual, &parsed, &error); check(success, label + ": reimport: " + error);
            ++cases; qInfo().noquote() << label << ": byte-identical to existing writer";
            return actual;
        };
        check(parity(source, base, base.radioName, base.radioId, "Unchanged source") == source, "Unchanged bytes differ");
        parity(source, base, "OTHER RADIO", 3109102, "Identity only");
        auto edited = base;
        edited.channels[1].name = "Edited Digital";
        edited.channels[1].frequencyAHz = 449350000; edited.channels[1].frequencyBHz = 444350000;
        edited.channels[1].timeSlot = 2; edited.channels[1].colorCode = 7; edited.channels[1].highPower = false;
        parity(source, edited, base.radioName, base.radioId, "Digital fields and masked settings");
        edited.channels[1].highPower.reset();
        parity(source, edited, base.radioName, base.radioId, "Unspecified power");
        edited = base;
        edited.channels[0].name = "Mode Digital"; edited.channels[0].digital = true;
        edited.channels[0].colorCode = 3; edited.channels[0].timeSlot = 1; edited.channels[0].contactIndex = 1;
        edited.channels[0].useDefaultTemplate = true;
        parity(source, edited, base.radioName, base.radioId, "Analog to digital defaults");
        edited = base;
        edited.channels[1].name = "Mode Analog"; edited.channels[1].digital = false;
        edited.channels[1].colorCode = 0; edited.channels[1].timeSlot = 0; edited.channels[1].contactIndex = 0;
        edited.channels[1].useDefaultTemplate = true;
        parity(source, edited, base.radioName, base.radioId, "Digital to analog defaults");
        edited = base;
        edited.contacts.append({"K1ABC Private", 1234567, false});
        edited.contacts.append({"Five9", 9999, true});
        edited.contacts[0].name = "Local Edited";
        edited.channels.append({"Added TS2", 449350000, 444350000, true, 2, 2, 3, true});
        edited.zones[0].channelIndexes = {2, 0, 1};
        edited.zones.append({"Shared zone", {1, 2}});
        const auto expanded = parity(source, edited, base.radioName, base.radioId, "Add channels contacts and zones; grow zone span");
        ImportedCodeplug expandedPlug;
        ok = definition.read(expanded, &expandedPlug, &error); check(ok, error);
        auto reduced = expandedPlug;
        reduced.channels.resize(2); reduced.contacts.resize(2); reduced.zones.resize(1);
        reduced.zones[0].channelIndexes = {1};
        const auto shrunk = parity(expanded, reduced, base.radioName, base.radioId, "Remove records; shrink zone span");
        Q_UNUSED(shrunk);
        auto reordered = expandedPlug;
        std::swap(reordered.channels[0], reordered.channels[2]);
        for (auto &zone : reordered.zones)
            for (auto &index : zone.channelIndexes) { if (index == 0) index = 2; else if (index == 2) index = 0; }
        parity(expanded, reordered, base.radioName, base.radioId, "Channel storage reordering and lineage");
        auto zonesOnly = expandedPlug;
        std::reverse(zonesOnly.zones[0].channelIndexes.begin(), zonesOnly.zones[0].channelIndexes.end());
        parity(expanded, zonesOnly, base.radioName, base.radioId, "Zone membership ordering");
        bool testedPopulatedEdit = false;
        for (const auto &name : QDir(root + "/Codeplugs").entryList({"*.rdbx"}, QDir::Files, QDir::Name)) {
            const auto input = fileBytes(root + "/Codeplugs/" + name);
            ImportedCodeplug plug;
            ok = definition.read(input, &plug, &error); check(ok, name + ": " + error);
            const bool validId = plug.radioId >= 1000000 && plug.radioId <= 9999999;
            const auto output = parity(input, plug, plug.radioName, validId ? plug.radioId : 3109101, name);
            if (validId) check(output == input, name + ": unchanged export differs");
            if (!testedPopulatedEdit && validId && plug.channels.size() > 100 && plug.contacts.size() > 100) {
                auto changed = plug;
                changed.channels[0].frequencyAHz += 12500;
                changed.zones[0].name = "Definition Test";
                std::reverse(changed.zones[0].channelIndexes.begin(), changed.zones[0].channelIndexes.end());
                changed.zones.append(ImportedZone{"Added Zone", {0}});
                parity(input, changed, plug.radioName, plug.radioId, name + " edited with zone growth");
                testedPopulatedEdit = true;
            }
        }
        // Version text is retained, rather than checked against a whitelist.
        auto newer = source;
        newer[0x1299] = char('9' ^ quint8(source[0x300 + 0x1299 % 256]));
        ImportedCodeplug newerPlug;
        ok = definition.read(newer, &newerPlug, &error); check(ok, error);
        newerPlug.channels[1].name = "Newer Version";
        QByteArray newerOutput;
        ok = definition.write(newer, newerPlug, newerPlug.radioName, newerPlug.radioId, &newerOutput, &error); check(ok, error);
        ok = definition.read(newerOutput, &newerPlug, &error); check(ok && newerPlug.cpsVersion.startsWith("V9."), "Version text changed on export");
        const auto refused = [&](const ImportedCodeplug &bad, const QString &label, const QString &expected) {
            QByteArray output("sentinel");
            bool success = definition.write(source, bad, base.radioName, base.radioId, &output, &error);
            check(!success && output == "sentinel", label + ": invalid target accepted or output changed");
            check(error.contains(expected, Qt::CaseInsensitive), label + ": unexpected error: " + error);
        };
        edited = base; edited.channels[1].sourceRecord = 99999; edited.channels[1].name = "Changed";
        refused(edited, "Invalid lineage", "source record");
        edited = base; edited.channels[1].contactIndex = 99999;
        refused(edited, "Unknown contact", "contact");
        edited = base; edited.zones[0].channelIndexes = {99999};
        refused(edited, "Unknown zone member", "channel");
        edited = base; edited.channels[0].frequencyAHz = 1;
        refused(edited, "Invalid frequency", "Frequency");
        edited = base; edited.contacts[0].name = "Invalid/name";
        refused(edited, "Invalid contact text", "character");
        edited = base; edited.channels.clear();
        refused(edited, "No channels", "channels");
        edited = base; edited.channels.resize(3001);
        refused(edited, "Channel capacity", "channels");
        edited = base; edited.zones.resize(251);
        refused(edited, "Zone capacity", "zones");
        edited = base; edited.contacts.resize(10001);
        refused(edited, "Contact capacity", "contacts");
        edited = base; edited.zones[0].channelIndexes = QVector<int>(17, 0);
        refused(edited, "Zone members", "member");
        edited = base; edited.contacts.clear(); edited.channels[1].contactIndex = 0;
        refused(edited, "Preserved RX group now dangles", "reference");

        QTemporaryDir temporary; check(temporary.isValid(), "No temporary test directory");
        QMap<QString, QByteArray> entries;
        ok = ZipArchive::read(path, &entries, &error); check(ok, error);
        const auto originalJson = QJsonDocument::fromJson(entries.value("definition.json")).object();
        const auto malformed = [&](const std::function<void(QJsonObject &)> &mutate, const QString &expected) {
            auto json = originalJson; mutate(json); auto changed = entries;
            changed["definition.json"] = QJsonDocument(json).toJson();
            const QString file = temporary.filePath("invalid-plan.wcp");
            bool success = ZipArchive::write(file, changed, &error); check(success, error);
            CodeplugDefinition bad;
            success = CodeplugDefinition::load(file, &bad, &error); check(success, error);
            QByteArray output("sentinel");
            success = bad.write(source, base, base.radioName, base.radioId, &output, &error);
            check(!success && output == "sentinel" && error.contains(expected, Qt::CaseInsensitive), "Invalid plan accepted on unchanged path: " + error);
        };
        malformed([](QJsonObject &j) {
            auto c = j["creation"].toObject(), p = c["construction"].toObject();
            auto step = p["contacts_pack"].toObject(); step["operation"] = "unknown";
            p["contacts_pack"] = step; c["construction"] = p; j["creation"] = c;
        }, "Unsupported writer operation");
        malformed([](QJsonObject &j) {
            auto c = j["creation"].toObject(), p = c["construction"].toObject();
            auto step = p["channels"].toObject(); step["unexpected_parameter"] = true;
            p["channels"] = step; c["construction"] = p; j["creation"] = c;
        }, "Unsupported writer parameter");
        malformed([](QJsonObject &j) {
            auto c = j["creation"].toObject(), p = c["construction"].toObject();
            p["version"] = 99; c["construction"] = p; j["creation"] = c;
        }, "version");
        malformed([](QJsonObject &j) {
            auto c = j["creation"].toObject(), p = c["construction"].toObject();
            auto order = p["order"].toArray(); const QJsonValue before = order.at(9);
            order[9] = order.at(10); order[10] = before;
            p["order"] = order; c["construction"] = p; j["creation"] = c;
        }, "operation order");
        malformed([](QJsonObject &j) {
            auto c = j["creation"].toObject(), p = c["construction"].toObject();
            auto step = p["relocate_sections"].toObject(), pointers = step["pointer_directory"].toObject();
            pointers["stride_bytes"] = 0; step["pointer_directory"] = pointers;
            p["relocate_sections"] = step; c["construction"] = p; j["creation"] = c;
        }, "pointer directory bounds");

        // Move a numeric write destination in a temporary definition/template.
        // Neither writer code nor target values are changed to accommodate it.
        auto movedJson = originalJson;
        auto tables = movedJson["tables"].toObject(), channels = tables["channels"].toObject();
        auto fields = channels["fields"].toObject(), frequency = fields["transmit_frequency"].toObject();
        frequency["offset"] = 12; fields["transmit_frequency"] = frequency;
        channels["fields"] = fields; tables["channels"] = channels; movedJson["tables"] = tables;
        auto moved = raw(source);
        for (int i = 0; i < base.channels.size(); ++i) put32(moved, 0x23768 + 48 * i + 12, base.channels[i].frequencyAHz);
        auto creation = movedJson["creation"].toObject();
        creation["representation"] = "decoded_radio_image";
        creation["template_sha256"] = QString::fromLatin1(QCryptographicHash::hash(moved, QCryptographicHash::Sha256).toHex());
        movedJson["creation"] = creation;
        auto movedEntries = entries;
        movedEntries[creation["template"].toString()] = moved;
        movedEntries["definition.json"] = QJsonDocument(movedJson).toJson();
        const QString movedPath = temporary.filePath("moved.wcp");
        ok = ZipArchive::write(movedPath, movedEntries, &error); check(ok, error);
        CodeplugDefinition movedDefinition;
        ok = CodeplugDefinition::load(movedPath, &movedDefinition, &error); check(ok, error);
        ImportedCodeplug movedTarget;
        ok = movedDefinition.read(moved, &movedTarget, &error); check(ok, error);
        movedTarget.channels[1].frequencyAHz = 449350000;
        QByteArray movedOutput;
        ok = movedDefinition.write(moved, movedTarget, movedTarget.radioName, movedTarget.radioId, &movedOutput, &error); check(ok, error);
        check(get32(movedOutput, 0x23768 + 48 + 12) == 449350000, "Writer ignored definition field offset");
        check(get32(movedOutput, 0x23768 + 48) == get32(moved, 0x23768 + 48), "Writer used old hard-coded field offset");
        // Rename every binary table/field and every construction-step label.
        // Keep editor semantics unchanged, and use a different model marker.
        // This detects hidden dependencies on the CS7000 storage vocabulary.
        QMap<QString, QString> tableNames, fieldNames;
        for (const auto &name : originalJson["tables"].toObject().keys()) tableNames[name] = "storage_" + name;
        for (const auto &name : originalJson["fields"].toObject().keys()) fieldNames[name] = "binary_" + name;
        const auto originalTables = originalJson["tables"].toObject();
        for (auto it = originalTables.begin(); it != originalTables.end(); ++it)
            for (const auto &name : it.value().toObject()["fields"].toObject().keys()) fieldNames[name] = "binary_" + name;
        const QSet<QString> fieldProperties{"field", "record_field", "model_field", "cps_version_field", "version_field",
                                           "count_field", "additional_pointer_field", "mode_field", "index_field"};
        const QSet<QString> tableProperties{"table", "index_table", "names_table", "records_table"};
        std::function<QJsonValue(const QJsonValue &, const QString &, const QString &)> renameStorage;
        renameStorage = [&](const QJsonValue &value, const QString &key, const QString &parent) -> QJsonValue {
            if (value.isObject()) {
                QJsonObject result;
                const auto object = value.toObject();
                for (auto it = object.begin(); it != object.end(); ++it) {
                    QString name = it.key();
                    if (key == "tables" && parent.isEmpty()) name = tableNames.value(name, name);
                    if (key == "fields" || key == "globals" || key == "expected_addresses" || key == "required_fields")
                        name = fieldNames.value(name, name);
                    result[name] = renameStorage(it.value(), it.key(), key);
                }
                return result;
            }
            if (value.isArray()) {
                QJsonArray result;
                for (const auto &entry : value.toArray()) {
                    if (entry.isString() && (key == "equal_fields" || parent == "equal_fields" || key == "preserve_fields"))
                        result.append(fieldNames.value(entry.toString(), entry.toString()));
                    else if (entry.isString() && key == "tables" && parent == "removed_records")
                        result.append(tableNames.value(entry.toString(), entry.toString()));
                    else result.append(renameStorage(entry, key, parent));
                }
                return result;
            }
            if (!value.isString()) return value;
            if (fieldProperties.contains(key)) return fieldNames.value(value.toString(), value.toString());
            if (tableProperties.contains(key)) return tableNames.value(value.toString(), value.toString());
            if (key == "record_size_from") {
                const auto components = value.toString().split('.');
                check(components.size() == 3, "Invalid test record-size source");
                return "tables." + tableNames.value(components[1]) + ".record_bytes";
            }
            return value;
        };
        auto renamedJson = renameStorage(originalJson, {}, {}).toObject();
        auto renamedCreation = renamedJson["creation"].toObject();
        auto renamedPlan = renamedCreation["construction"].toObject();
        const auto oldOrder = renamedPlan["order"].toArray();
        QMap<QString, QString> stepNames;
        for (qsizetype i = 0; i < oldOrder.size(); ++i) stepNames[oldOrder[i].toString()] = "step_" + QString::number(i);
        QJsonObject newPlan{{"version", renamedPlan["version"]}, {"default_generation", renamedPlan["default_generation"]}};
        QJsonArray newOrder;
        for (const auto &entry : oldOrder) {
            auto rule = renamedPlan[entry.toString()].toObject();
            if (!rule.contains("operation")) rule["operation"] = entry.toString();
            for (const QString &key : {QStringLiteral("next_step"), QStringLiteral("compare_as"), QStringLiteral("complete_before")})
                if (rule.contains(key)) rule[key] = stepNames.value(rule[key].toString());
            const QString name = stepNames.value(entry.toString());
            newPlan[name] = rule; newOrder.append(name);
        }
        newPlan["order"] = newOrder; renamedCreation["construction"] = newPlan;
        auto renamedImage = raw(source);
        const auto model = originalJson["fields"].toObject()["model"].toObject();
        bool parsedAddress = false;
        const qint64 modelAt = model["at"].toString().toLongLong(&parsedAddress, 16);
        check(parsedAddress, "Invalid fixture model address");
        QByteArray marker(model["bytes"].toInt(), '\0');
        marker.replace(0, 11, "OTHER-RADIO");
        renamedImage.replace(modelAt, model["bytes"].toInt(), marker);
        auto recognition = renamedJson["recognition"].toObject(); recognition["model_value"] = "OTHER-RADIO";
        renamedJson["recognition"] = recognition; renamedJson["id"] = "renamed-schema-fixture";
        renamedJson["representations"] = QJsonArray{QJsonObject{{"name", "fixture"}, {"extension", ".test"},
            {"decode", QJsonObject{{"operation", "identity"}}}}};
        renamedCreation["representation"] = "fixture";
        renamedCreation["template_bytes"] = renamedImage.size();
        renamedCreation["template_sha256"] = QString::fromLatin1(QCryptographicHash::hash(renamedImage, QCryptographicHash::Sha256).toHex());
        renamedJson["creation"] = renamedCreation;
        auto renamedEntries = entries;
        renamedEntries[renamedCreation["template"].toString()] = renamedImage;
        renamedEntries["definition.json"] = QJsonDocument(renamedJson).toJson();
        const QString renamedPath = temporary.filePath("renamed.wcp");
        ok = ZipArchive::write(renamedPath, renamedEntries, &error); check(ok, error);
        CodeplugDefinition renamedDefinition;
        ok = CodeplugDefinition::load(renamedPath, &renamedDefinition, &error); check(ok, error);
        ImportedCodeplug renamedTarget;
        ok = renamedDefinition.read(renamedImage, &renamedTarget, &error); check(ok, error);
        check(renamedTarget.model == "OTHER-RADIO" && renamedTarget.channels.size() == base.channels.size(), "Renamed model/schema was not imported");
        renamedTarget.channels[1].name = "GENERIC";
        renamedTarget.channels[1].frequencyAHz = 449350000;
        renamedTarget.zones[0].name = "GENERIC ZONE";
        renamedTarget.contacts.append({"GENERIC TG", 12345, true});
        QByteArray renamedOutput;
        ok = renamedDefinition.write(renamedImage, renamedTarget, "GENERIC RADIO", 3109102, &renamedOutput, &error); check(ok, error);
        ImportedCodeplug renamedRoundtrip;
        ok = renamedDefinition.read(renamedOutput, &renamedRoundtrip, &error); check(ok, error);
        check(renamedRoundtrip.channels[1].name == "GENERIC" && renamedRoundtrip.channels[1].frequencyAHz == 449350000
              && renamedRoundtrip.zones[0].name == "GENERIC ZONE" && renamedRoundtrip.contacts.last().id == 12345
              && renamedRoundtrip.radioName == "GENERIC RADIO", "Renamed schema export did not preserve editor values");
        qInfo() << "Different model with all binary names and command labels renamed: import/edit/export/reimport passed";
        fixedLayoutRoundtrip(originalJson, temporary.filePath("fixed-layout.wcp"));
        qInfo() << cases << "byte-for-byte comparison cases passed; rejection and schema relocation checks passed";
        return 0;
    } catch (const std::exception &e) { qCritical().noquote() << e.what(); return 1; }
}
