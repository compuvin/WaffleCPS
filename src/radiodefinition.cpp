#include "radiodefinition.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <cstdio>

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

bool hexBytesOrEmpty(const QJsonValue &value)
{
    return value.isString() && (value.toString().isEmpty() || hexBytes(value));
}

bool unsignedValue(const QJsonValue &value, quint32 *result)
{
    bool ok = false;
    quint64 number = 0;
    if (value.isDouble()) {
        const double n = value.toDouble();
        if (n < 0 || n > 4294967295.0 || n != quint32(n)) return false;
        number = quint32(n);
    } else if (value.isString()) {
        const QString text = value.toString();
        number = text.toULongLong(&ok, text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
        if (!ok || number > 4294967295ULL) return false;
    } else return false;
    *result = quint32(number);
    return true;
}

int typeBytes(const QString &type)
{
    if (type == QStringLiteral("u8")) return 1;
    if (type == QStringLiteral("u16le") || type == QStringLiteral("u16be")) return 2;
    if (type == QStringLiteral("u32le") || type == QStringLiteral("u32be")) return 4;
    return 0;
}

bool offsetExpression(const QJsonValue &value)
{
    quint32 offset = 0;
    if (unsignedValue(value, &offset)) return true;
    const QJsonObject expression = value.toObject();
    const QJsonArray add = expression.value(QStringLiteral("add")).toArray();
    if (expression.size() != 1 || add.isEmpty()) return false;
    bool length = false;
    for (const QJsonValue &term : add) {
        if (unsignedValue(term, &offset)) continue;
        const QJsonObject reference = term.toObject();
        if (reference.size() != 1 || reference.value(QStringLiteral("length_of")) != QStringLiteral("payload")
            || length) return false;
        length = true;
    }
    return length;
}

bool fieldReference(const QJsonValue &value)
{
    const QJsonObject object = value.toObject();
    return object.size() == 1 && !object.value(QStringLiteral("field")).toString().isEmpty();
}

bool trueField(const QJsonObject &object, const char *name)
{
    return object.value(QLatin1String(name)) == QJsonValue(true);
}

bool falseField(const QJsonObject &object, const char *name)
{
    return object.value(QLatin1String(name)) == QJsonValue(false);
}

bool validateDfuRules(const QJsonObject &root, QString *error)
{
    if (root.contains(QStringLiteral("bulk_only_transport")))
        return fail(error, QStringLiteral("A radio definition must select one USB transport."));
    const QJsonObject usb = root.value(QStringLiteral("usb")).toObject();
    const QJsonObject transport = root.value(QStringLiteral("dfu_control_transport")).toObject();
    const QJsonObject requests = transport.value(QStringLiteral("requests")).toObject();
    const QJsonObject commands = transport.value(QStringLiteral("commands")).toObject();
    const QJsonObject status = transport.value(QStringLiteral("get_status")).toObject();
    const QJsonObject idle = transport.value(QStringLiteral("enter_dfu_idle")).toObject();
    quint16 vendor = 0, product = 0;
    quint8 direction = 0;
    if (!wordValue(usb.value(QStringLiteral("vendor_id")), &vendor)
        || !wordValue(usb.value(QStringLiteral("product_id")), &product)
        || !byteValue(transport.value(QStringLiteral("out_request_type")), &direction)
        || direction != 0x21
        || !byteValue(transport.value(QStringLiteral("in_request_type")), &direction)
        || direction != 0xA1
        || vendor == 0 || product == 0
        || usb.value(QStringLiteral("interface_number")).toInt(-1) < 0
        || usb.value(QStringLiteral("interface_number")).toInt() > 255
        || usb.value(QStringLiteral("alternate_setting")).toInt(-1) < 0
        || usb.value(QStringLiteral("alternate_setting")).toInt() > 255
        || transport.value(QStringLiteral("interface_index")).toInt(-1) != usb.value(QStringLiteral("interface_number")).toInt(-2)
        || !trueField(usb.value(QStringLiteral("interface_lifecycle")).toObject(), "claim_interface")
        || !trueField(usb.value(QStringLiteral("interface_lifecycle")).toObject(), "release_interface")
        || transport.value(QStringLiteral("transfer_timeout_ms")).toInt() < 1
        || transport.value(QStringLiteral("transfer_timeout_ms")).toInt() > 60000
        || status.value(QStringLiteral("response_bytes")).toInt() != 6
        || status.value(QStringLiteral("success_status")).toInt(-1) != 0
        || status.value(QStringLiteral("state_offset")).toInt(-1) != 4
        || status.value(QStringLiteral("status_offset")).toInt(-1) != 0
        || idle.value(QStringLiteral("maximum_attempts")).toInt() < 1
        || idle.value(QStringLiteral("maximum_attempts")).toInt() > 1000
        || idle.value(QStringLiteral("wait_ms")).toInt(-1) < 0
        || idle.value(QStringLiteral("wait_ms")).toInt() > 60000)
        return fail(error, QStringLiteral("DFU USB interface or status rules are incomplete."));
    const QMap<QString, int> expectedRequests{{"detach", 0}, {"download", 1}, {"upload", 2},
        {"get_status", 3}, {"clear_status", 4}, {"get_state", 5}, {"abort", 6}};
    for (auto it = expectedRequests.cbegin(); it != expectedRequests.cend(); ++it)
        if (requests.value(it.key()).toInt(-1) != it.value())
            return fail(error, QStringLiteral("Unsupported DFU request: %1.").arg(it.key()));
    const QJsonObject actions = idle.value(QStringLiteral("state_actions")).toObject();
    for (int state = 0; state <= 10; ++state) {
        const QString action = actions.value(QString::number(state)).toString();
        if (action != QStringLiteral("ready") && action != QStringLiteral("wait")
            && action != QStringLiteral("abort") && action != QStringLiteral("clear_status")
            && action != QStringLiteral("detach"))
            return fail(error, QStringLiteral("DFU state action is missing: %1.").arg(state));
    }
    if (actions.value(QStringLiteral("2")) != QStringLiteral("ready"))
        return fail(error, QStringLiteral("DFU idle state must be ready."));
    const QJsonArray pollOffsets = status.value(QStringLiteral("poll_timeout_offsets")).toArray();
    QSet<int> usedPollOffsets;
    for (const QJsonValue &offset : pollOffsets) {
        const int index = offset.toInt(-1);
        if (index < 1 || index >= 6 || usedPollOffsets.contains(index))
            return fail(error, QStringLiteral("Invalid DFU poll timeout offsets."));
        usedPollOffsets.insert(index);
    }
    if (pollOffsets.size() != 3
        || (status.value(QStringLiteral("poll_timeout_type")) != QStringLiteral("u24le")
            && status.value(QStringLiteral("poll_timeout_type")) != QStringLiteral("u24be")))
        return fail(error, QStringLiteral("Invalid DFU poll timeout encoding."));
    const QJsonObject completion = transport.value(QStringLiteral("command_completion")).toObject();
    const auto validCompletion = [&status](const QJsonObject &rule) {
        const QJsonArray steps = rule.value(QStringLiteral("after_download")).toArray();
        if (steps.isEmpty() || steps.last() != QStringLiteral("get_status")
            || !status.value(QStringLiteral("states")).toObject()
                    .contains(rule.value(QStringLiteral("require_state")).toString())
            || (rule.contains(QStringLiteral("then"))
                && rule.value(QStringLiteral("then")) != QStringLiteral("enter_dfu_idle"))
            || !rule.value(QStringLiteral("fail_on_nonzero_status")).isBool()) return false;
        bool sawStatus = false;
        for (const QJsonValue &value : steps) {
            const QString step = value.toString();
            if (step == QStringLiteral("get_status")) sawStatus = true;
            else if (step == QStringLiteral("wait_poll_timeout")) {
                if (!sawStatus) return false;
            } else if (step != QStringLiteral("clear_status")) {
                if (!step.startsWith(QStringLiteral("delay_ms:"))) return false;
                bool ok = false;
                const uint delay = step.mid(9).toUInt(&ok);
                if (!ok || delay > 60000) return false;
            }
        }
        return sawStatus;
    };
    if (!validCompletion(completion)
        || completion.value(QStringLiteral("then")) != QStringLiteral("enter_dfu_idle")
        || status.value(QStringLiteral("states")).toObject().value(QStringLiteral("download_idle")).toInt(-1) != 5)
        return fail(error, QStringLiteral("DFU command completion is incomplete."));
    const QJsonObject writeCompletion = transport.value(QStringLiteral("write_command_completion")).toObject();
    if (!writeCompletion.isEmpty() && !validCompletion(writeCompletion))
        return fail(error, QStringLiteral("DFU write command completion is incomplete."));
    const QJsonObject blockCompletion = transport.value(QStringLiteral("block_completion")).toObject();
    if (blockCompletion.contains(QStringLiteral("after_download"))
            ? !validCompletion(blockCompletion)
            : (blockCompletion.value(QStringLiteral("poll_state")) != QStringLiteral("download_idle")
               || blockCompletion.value(QStringLiteral("maximum_attempts")).toInt() < 1
               || blockCompletion.value(QStringLiteral("maximum_attempts")).toInt() > 10000
               || blockCompletion.value(QStringLiteral("delay_ms")).toInt(-1) < 0
               || blockCompletion.value(QStringLiteral("delay_ms")).toInt() > 60000))
        return fail(error, QStringLiteral("DFU write block completion is incomplete."));
    if (commands.isEmpty())
        return fail(error, QStringLiteral("DFU commands are missing."));
    for (auto it = commands.constBegin(); it != commands.constEnd(); ++it) {
        const QJsonObject command = it.value().toObject();
        if (command.value(QStringLiteral("request")) != QStringLiteral("download")
            || command.value(QStringLiteral("value")).toInt(-1) != 0)
            return fail(error, QStringLiteral("Invalid DFU command request: %1.").arg(it.key()));
        if (command.contains(QStringLiteral("data_hex"))) {
            if (command.contains(QStringLiteral("data"))
                || !hexBytesOrEmpty(command.value(QStringLiteral("data_hex"))))
                return fail(error, QStringLiteral("Invalid DFU command bytes: %1.").arg(it.key()));
        } else {
            const QJsonArray fields = command.value(QStringLiteral("data")).toArray();
            if (fields.size() != 2 || !hexBytes(fields[0].toObject().value(QStringLiteral("hex")))
                || fields[1].toObject().value(QStringLiteral("type")) != QStringLiteral("u32le")
                || fields[1].toObject().value(QStringLiteral("parameter")) != QStringLiteral("address"))
                return fail(error, QStringLiteral("Invalid DFU address command: %1.").arg(it.key()));
        }
    }
    const QJsonObject methods = root.value(QStringLiteral("methods")).toObject();
    const QJsonObject read = methods.value(QStringLiteral("read_codeplug")).toObject();
    const QJsonObject assembly = read.value(QStringLiteral("assembly")).toObject();
    const QJsonObject write = methods.value(QStringLiteral("write_codeplug")).toObject();
    const QJsonObject exit = methods.value(QStringLiteral("exit_program_mode")).toObject();
    const QJsonObject source = write.value(QStringLiteral("source")).toObject();
    const QJsonArray readSteps = read.value(QStringLiteral("operations")).toArray();
    const QJsonArray writeSteps = write.value(QStringLiteral("operations")).toArray();
    if (readSteps.isEmpty() || writeSteps.isEmpty()
        || assembly.value(QStringLiteral("operation")) != QStringLiteral("replace_payload_in_template")
        || assembly.value(QStringLiteral("representation")).toString().isEmpty()
        || assembly.value(QStringLiteral("bytes")).toInt() < 1
        || assembly.value(QStringLiteral("wcp_payload_bytes")).toInt() != assembly.value(QStringLiteral("bytes")).toInt()
        || assembly.value(QStringLiteral("wcp_payload_offset")).toInt(-1) < 0
        || assembly.value(QStringLiteral("wcp_suffix_bytes")).toInt(-1) < 0
        || assembly.value(QStringLiteral("wcp_wrapper_bytes")).toInt()
            != assembly.value(QStringLiteral("wcp_payload_offset")).toInt()
               + assembly.value(QStringLiteral("bytes")).toInt()
               + assembly.value(QStringLiteral("wcp_suffix_bytes")).toInt()
        || write.value(QStringLiteral("codeplug_definition_id")) != root.value(QStringLiteral("id"))
        || write.value(QStringLiteral("representation")) != assembly.value(QStringLiteral("representation"))
        || source.value(QStringLiteral("image_bytes")).toInt() != assembly.value(QStringLiteral("wcp_wrapper_bytes")).toInt()
        || source.value(QStringLiteral("payload_offset")).toInt() != assembly.value(QStringLiteral("wcp_payload_offset")).toInt()
        || source.value(QStringLiteral("payload_bytes")).toInt() != assembly.value(QStringLiteral("bytes")).toInt()
        || source.value(QStringLiteral("suffix_bytes")).toInt(-1) != assembly.value(QStringLiteral("wcp_suffix_bytes")).toInt()
        || source.value(QStringLiteral("magic_ascii")).toString().isEmpty())
        return fail(error, QStringLiteral("DFU image assembly or write source is incomplete."));
    const auto validCommandStep = [&commands](const QJsonObject &step) {
        const QString name = step.value(QStringLiteral("name")).toString();
        const int repeat = step.contains(QStringLiteral("repeat"))
            ? step.value(QStringLiteral("repeat")).toInt(-1) : 1;
        return commands.contains(name) && repeat >= 1 && repeat <= 10
            && (commands.value(name).toObject().contains(QStringLiteral("data"))
                || hexBytes(commands.value(name).toObject().value(QStringLiteral("data_hex"))))
            && (!step.contains(QStringLiteral("address"))
                || (step.value(QStringLiteral("address")).isDouble()
                    && commands.value(name).toObject().contains(QStringLiteral("data"))));
    };
    const bool readExits = readSteps.last().toObject().value(QStringLiteral("operation"))
        == QStringLiteral("exit_program_mode");
    const bool writeExits = writeSteps.last().toObject().value(QStringLiteral("operation"))
        == QStringLiteral("exit_program_mode");
    const auto validExit = [&commands, &exit](const QJsonObject &step) {
        const QString name = step.contains(QStringLiteral("command"))
            ? step.value(QStringLiteral("command")).toString()
            : exit.value(QStringLiteral("command")).toString();
        const QJsonObject command = commands.value(name).toObject();
        return command.value(QStringLiteral("request")) == QStringLiteral("download")
            && command.value(QStringLiteral("value")).toInt(-1) == 0
            && hexBytesOrEmpty(command.value(QStringLiteral("data_hex")))
            && (!step.contains(QStringLiteral("status_after"))
                || step.value(QStringLiteral("status_after")) == QStringLiteral("allow_disconnect"));
    };
    if ((readExits && !validExit(readSteps.last().toObject()))
        || (writeExits && !validExit(writeSteps.last().toObject())))
        return fail(error, QStringLiteral("DFU exit command is missing or invalid."));
    bool readsBlocks = false, writesBlocks = false;
    quint64 writtenBytes = 0;
    if (readSteps.first().toObject().value(QStringLiteral("operation")) != QStringLiteral("enter_dfu_idle")
        || readSteps.at(readSteps.size() - 1 - int(readExits)).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("read_blocks")
        || writeSteps.first().toObject().value(QStringLiteral("operation")) != QStringLiteral("validate_codeplug")
        || writeSteps.at(writeSteps.size() - 1 - int(writeExits)).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("write_blocks"))
        return fail(error, QStringLiteral("DFU transfer operation order is invalid."));
    for (int i = 0; i < readSteps.size(); ++i) {
        const QJsonValue value = readSteps.at(i);
        const QJsonObject step = value.toObject();
        const QString operation = step.value(QStringLiteral("operation")).toString();
        if (operation == QStringLiteral("read_blocks")) {
            if (readsBlocks) return fail(error, QStringLiteral("Duplicate DFU read block operation."));
            readsBlocks = true;
            if (step.value(QStringLiteral("first_block_number")).toInt(-1) < 2
                || step.value(QStringLiteral("block_count")).toInt() < 1
                || step.value(QStringLiteral("block_bytes")).toInt() < 1
                || step.value(QStringLiteral("block_bytes")).toInt() > 65535
                || step.value(QStringLiteral("first_block_number")).toInt()
                    + step.value(QStringLiteral("block_count")).toInt() > 65536
                || step.value(QStringLiteral("request")) != QStringLiteral("upload")
                || step.value(QStringLiteral("status_after_each_block")) != QJsonValue(true)
                || qint64(step.value(QStringLiteral("block_count")).toInt()) * step.value(QStringLiteral("block_bytes")).toInt()
                    != assembly.value(QStringLiteral("bytes")).toInt())
                return fail(error, QStringLiteral("Invalid DFU read block range."));
        } else if (operation == QStringLiteral("command")) {
            if (!validCommandStep(step)) return fail(error, QStringLiteral("Invalid DFU read command."));
        } else if (operation == QStringLiteral("exit_program_mode")) {
            if (i != readSteps.size() - 1 || !readExits)
                return fail(error, QStringLiteral("DFU read exit must be last."));
        } else if (operation != QStringLiteral("enter_dfu_idle"))
            return fail(error, QStringLiteral("Unsupported DFU read operation: %1.").arg(operation));
    }
    for (int i = 0; i < writeSteps.size(); ++i) {
        const QJsonValue value = writeSteps.at(i);
        const QJsonObject step = value.toObject();
        const QString operation = step.value(QStringLiteral("operation")).toString();
        if (operation == QStringLiteral("write_blocks")) {
            writesBlocks = true;
            quint32 sourceOffset = 0;
            if (step.contains(QStringLiteral("source_offset"))
                && !unsignedValue(step.value(QStringLiteral("source_offset")), &sourceOffset))
                return fail(error, QStringLiteral("Invalid DFU write payload offset."));
            const quint64 sectionBytes = qint64(step.value(QStringLiteral("block_count")).toInt())
                * step.value(QStringLiteral("block_bytes")).toInt();
            if (step.value(QStringLiteral("first_block_number")).toInt(-1) < 2
                || step.value(QStringLiteral("block_count")).toInt() < 1
                || step.value(QStringLiteral("block_bytes")).toInt() < 1
                || step.value(QStringLiteral("block_bytes")).toInt() > 65535
                || step.value(QStringLiteral("first_block_number")).toInt()
                    + step.value(QStringLiteral("block_count")).toInt() > 65536
                || step.value(QStringLiteral("request")) != QStringLiteral("download")
                || step.value(QStringLiteral("source")) != QStringLiteral("payload")
                || sourceOffset != writtenBytes
                || writtenBytes + sectionBytes > quint64(source.value(QStringLiteral("payload_bytes")).toInt()))
                return fail(error, QStringLiteral("Invalid DFU write block range."));
            writtenBytes += sectionBytes;
        } else if (operation == QStringLiteral("command")) {
            if (!validCommandStep(step)) return fail(error, QStringLiteral("Invalid DFU write command."));
        } else if (operation == QStringLiteral("upload")) {
            if (step.value(QStringLiteral("bytes")).toInt() < 1
                || step.value(QStringLiteral("bytes")).toInt() > 65535
                || step.value(QStringLiteral("value")).toInt(-1) < 0)
                return fail(error, QStringLiteral("Invalid DFU setup upload."));
        } else if (operation == QStringLiteral("delay")) {
            if (step.value(QStringLiteral("milliseconds")).toInt(-1) < 0
                || step.value(QStringLiteral("milliseconds")).toInt() > 60000)
                return fail(error, QStringLiteral("Invalid DFU setup delay."));
        } else if (operation == QStringLiteral("erase_blocks")) {
            if (!commands.contains(step.value(QStringLiteral("command")).toString())
                || !commands.value(step.value(QStringLiteral("command")).toString()).toObject()
                       .contains(QStringLiteral("data"))
                || step.value(QStringLiteral("addresses")).toArray().isEmpty())
                return fail(error, QStringLiteral("DFU erase addresses are missing."));
            for (const QJsonValue &address : step.value(QStringLiteral("addresses")).toArray()) {
                quint32 ignored = 0;
                if (!unsignedValue(address, &ignored))
                    return fail(error, QStringLiteral("Invalid DFU erase address."));
            }
        } else if (operation == QStringLiteral("exit_program_mode")) {
            if (i != writeSteps.size() - 1 || !writeExits)
                return fail(error, QStringLiteral("DFU write exit must be last."));
        } else if (operation != QStringLiteral("enter_dfu_idle")
                   && operation != QStringLiteral("validate_codeplug")
                   && operation != QStringLiteral("get_status"))
            return fail(error, QStringLiteral("Unsupported DFU write operation: %1.").arg(operation));
    }
    if (!readsBlocks || !writesBlocks
        || writtenBytes != quint64(source.value(QStringLiteral("payload_bytes")).toInt()))
        return fail(error, QStringLiteral("DFU read or write block operation is missing."));
    return true;
}

bool validateTransportRules(const QJsonObject &root, QString *error)
{
    const QJsonObject usb = root.value(QStringLiteral("usb")).toObject();
    const QJsonObject lifecycle = usb.value(QStringLiteral("interface_lifecycle")).toObject();
    quint16 vendor = 0, product = 0;
    quint8 in = 0, out = 0;
    if (!wordValue(usb.value(QStringLiteral("vendor_id")), &vendor)
        || !wordValue(usb.value(QStringLiteral("product_id")), &product)
        || !byteValue(usb.value(QStringLiteral("bulk_in_endpoint")), &in)
        || !byteValue(usb.value(QStringLiteral("bulk_out_endpoint")), &out)
        || usb.value(QStringLiteral("interface_number")).toInt(-1) < 0
        || usb.value(QStringLiteral("interface_number")).toInt() > 255
        || usb.value(QStringLiteral("alternate_setting")).toInt(-1) < 0
        || usb.value(QStringLiteral("alternate_setting")).toInt() > 255
        || usb.value(QStringLiteral("maximum_packet_bytes")).toInt() < 1
        || usb.value(QStringLiteral("maximum_packet_bytes")).toInt() > 65535
        || !trueField(lifecycle, "detach_kernel_driver_if_active")
        || !trueField(lifecycle, "claim_interface")
        || !trueField(lifecycle, "release_interface")
        || !trueField(lifecycle, "reattach_if_detached"))
        return fail(error, QStringLiteral("Radio USB interface or lifecycle is incomplete."));
    quint8 interfaceClass = 0, interfaceSubclass = 0, interfaceProtocol = 0;
    if (!byteValue(usb.value(QStringLiteral("interface_class")), &interfaceClass)
        || !byteValue(usb.value(QStringLiteral("interface_subclass")), &interfaceSubclass)
        || !byteValue(usb.value(QStringLiteral("interface_protocol")), &interfaceProtocol)
        || !(in & 0x80) || (out & 0x80) || in == out)
        return fail(error, QStringLiteral("Radio USB interface descriptors are invalid."));
    const QJsonObject transport = root.value(QStringLiteral("bulk_only_transport")).toObject();
    const QJsonObject cbw = transport.value(QStringLiteral("cbw")).toObject();
    const QJsonObject tag = cbw.value(QStringLiteral("tag_rule")).toObject();
    const QJsonObject csw = transport.value(QStringLiteral("csw")).toObject();
    const QJsonObject cswChecks = csw.value(QStringLiteral("checks")).toObject();
    if (cbw.value(QStringLiteral("bytes")).toInt() < 1
        || cbw.value(QStringLiteral("bytes")).toInt() > 4096
        || tag.value(QStringLiteral("initial")).toInt(-1) < 0
        || tag.value(QStringLiteral("increment")).toInt() < 1
        || tag.value(QStringLiteral("wrap_modulus")).toDouble() != 4294967296.0
        || csw.value(QStringLiteral("bytes")).toInt() < 1
        || csw.value(QStringLiteral("bytes")).toInt() > 4096
        || !trueField(cswChecks, "signature_matches")
        || !trueField(cswChecks, "tag_matches_cbw")
        || !trueField(cswChecks, "data_bytes_plus_residue_matches_cbw")
        || cswChecks.value(QStringLiteral("success_status")).toInt(-1) < 0
        || cswChecks.value(QStringLiteral("success_residue")).toInt(-1) < 0)
        return fail(error, QStringLiteral("Radio CBW/CSW validation rules are incomplete."));
    const QJsonArray cbwFields = cbw.value(QStringLiteral("fields")).toArray();
    QSet<QString> cbwParameters;
    int cdbBytes = -1;
    bool signatureFound = false;
    for (const QJsonValue &entry : cbwFields) {
        const QJsonObject field = entry.toObject();
        const int offset = field.value(QStringLiteral("offset")).toInt(-1);
        const QString parameter = field.value(QStringLiteral("parameter")).toString();
        const QString type = field.value(QStringLiteral("type")).toString();
        int width = 0;
        if (offset < 0 || !entry.isObject())
            return fail(error, QStringLiteral("Invalid CBW field offset."));
        if (field.contains(QStringLiteral("hex"))) {
            if (!parameter.isEmpty() || !hexBytes(field.value(QStringLiteral("hex"))))
                return fail(error, QStringLiteral("Invalid CBW signature field."));
            width = field.value(QStringLiteral("hex")).toString().split(' ').size();
            signatureFound = true;
        } else if (parameter == QStringLiteral("cdb")) {
            cdbBytes = field.value(QStringLiteral("bytes")).toInt(-1);
            width = cdbBytes;
            if (typeBytes(type) || cdbBytes < 1 || cdbBytes > 16)
                return fail(error, QStringLiteral("Invalid CBW command descriptor."));
        } else {
            if (!parameter.isEmpty() && parameter != QStringLiteral("tag")
                && parameter != QStringLiteral("data_bytes") && parameter != QStringLiteral("direction"))
                return fail(error, QStringLiteral("Unsupported CBW parameter."));
            width = typeBytes(type);
            quint32 constant = 0;
            if (width == 0 || (parameter.isEmpty()
                && !unsignedValue(field.value(QStringLiteral("value")), &constant)))
                return fail(error, QStringLiteral("Invalid CBW numeric field."));
        }
        if (offset + width > cbw.value(QStringLiteral("bytes")).toInt())
            return fail(error, QStringLiteral("CBW field exceeds wrapper."));
        if (!parameter.isEmpty()) {
            if (cbwParameters.contains(parameter))
                return fail(error, QStringLiteral("Duplicate CBW parameter."));
            cbwParameters.insert(parameter);
        }
    }
    if (!signatureFound || cdbBytes < 1 || cbwParameters != QSet<QString>{QStringLiteral("tag"),
        QStringLiteral("data_bytes"), QStringLiteral("direction"), QStringLiteral("cdb")})
        return fail(error, QStringLiteral("CBW fields are incomplete."));
    const QJsonObject cswSignature = csw.value(QStringLiteral("signature")).toObject();
    const int signatureOffset = cswSignature.value(QStringLiteral("offset")).toInt(-1);
    if (signatureOffset < 0 || !hexBytes(cswSignature.value(QStringLiteral("hex")))
        || signatureOffset + cswSignature.value(QStringLiteral("hex")).toString().split(' ').size()
            > csw.value(QStringLiteral("bytes")).toInt())
        return fail(error, QStringLiteral("CSW signature field is invalid."));
    for (const QString &name : {QStringLiteral("tag"), QStringLiteral("residue"), QStringLiteral("status")}) {
        const QJsonObject field = csw.value(name).toObject();
        const int offset = field.value(QStringLiteral("offset")).toInt(-1);
        const int width = typeBytes(field.value(QStringLiteral("type")).toString());
        if (offset < 0 || width == 0 || offset + width > csw.value(QStringLiteral("bytes")).toInt())
            return fail(error, QStringLiteral("CSW field is invalid: %1").arg(name));
    }
    const QJsonObject commands = transport.value(QStringLiteral("commands")).toObject();
    const QJsonObject send = commands.value(QStringLiteral("send_frame")).toObject();
    const QJsonObject fetch = commands.value(QStringLiteral("fetch_response")).toObject();
    quint8 sendDirection = 0, fetchDirection = 0;
    if (!hexBytes(send.value(QStringLiteral("cdb_hex")))
        || !hexBytes(fetch.value(QStringLiteral("cdb_hex")))
        || send.value(QStringLiteral("cdb_hex")).toString().split(' ').size() != cdbBytes
        || fetch.value(QStringLiteral("cdb_hex")).toString().split(' ').size() != cdbBytes
        || !byteValue(send.value(QStringLiteral("direction")), &sendDirection)
        || !byteValue(fetch.value(QStringLiteral("direction")), &fetchDirection)
        || sendDirection != 0 || fetchDirection != 0x80
        || send.value(QStringLiteral("data")).toObject().value(QStringLiteral("parameter"))
            != QJsonValue(QStringLiteral("request_frame"))
        || send.value(QStringLiteral("data_bytes")).toObject().value(QStringLiteral("length_of"))
            != QJsonValue(QStringLiteral("request_frame"))
        || fetch.value(QStringLiteral("data_bytes")).toObject().value(QStringLiteral("parameter"))
            != QJsonValue(QStringLiteral("fetch_bytes")))
        return fail(error, QStringLiteral("Radio transport commands are incomplete."));
    const QJsonObject frame = root.value(QStringLiteral("frame")).toObject();
    const QJsonArray frameFields = frame.value(QStringLiteral("fields")).toArray();
    const QJsonObject frameChecks = frame.value(QStringLiteral("checks")).toObject();
    const QJsonObject responseChecks = frame.value(QStringLiteral("response_checks")).toObject();
    quint8 responseMask = 0, success = 0;
    if (frameFields.size() != 7
        || !trueField(frameChecks, "delimiters_match_fields")
        || !trueField(frameChecks, "length_matches_fields")
        || !trueField(frameChecks, "checksum_matches_fields")
        || !byteValue(responseChecks.value(QStringLiteral("class_or_mask")), &responseMask)
        || !byteValue(responseChecks.value(QStringLiteral("success_value")), &success)
        || !trueField(responseChecks, "command_matches_request")
        || responseChecks.value(QStringLiteral("minimum_payload_bytes")).toInt() < 1
        || responseChecks.value(QStringLiteral("success_payload_offset")).toInt(-1) < 0
        || responseChecks.value(QStringLiteral("success_payload_offset")).toInt()
            >= responseChecks.value(QStringLiteral("minimum_payload_bytes")).toInt())
        return fail(error, QStringLiteral("Radio frame validation rules are incomplete."));
    const QJsonObject startField = frameFields.at(0).toObject();
    const QJsonObject classField = frameFields.at(1).toObject();
    const QJsonObject commandField = frameFields.at(2).toObject();
    const QJsonObject lengthField = frameFields.at(3).toObject();
    const QJsonObject payloadField = frameFields.at(4).toObject();
    const QJsonObject checksumField = frameFields.at(5).toObject();
    const QJsonObject endField = frameFields.at(6).toObject();
    quint32 startValue = 0, endValue = 0, modulus = 0;
    const int startOffset = startField.value(QStringLiteral("offset")).toInt(-1);
    const int classOffset = classField.value(QStringLiteral("offset")).toInt(-1);
    const int commandOffset = commandField.value(QStringLiteral("offset")).toInt(-1);
    const int lengthOffset = lengthField.value(QStringLiteral("offset")).toInt(-1);
    const int payloadOffset = payloadField.value(QStringLiteral("offset")).toInt(-1);
    const QString lengthType = lengthField.value(QStringLiteral("type")).toString();
    const QString checksumType = checksumField.value(QStringLiteral("type")).toString();
    if (startOffset < 0 || classOffset < 0 || commandOffset < 0 || lengthOffset < 0
        || payloadOffset < 0 || startOffset >= payloadOffset || classOffset >= payloadOffset
        || commandOffset >= payloadOffset || lengthOffset + typeBytes(lengthType) > payloadOffset
        || startField.value(QStringLiteral("type")) != QStringLiteral("u8")
        || classField.value(QStringLiteral("type")) != QStringLiteral("u8")
        || commandField.value(QStringLiteral("type")) != QStringLiteral("u8")
        || typeBytes(lengthType) < 2 || typeBytes(checksumType) < 1
        || endField.value(QStringLiteral("type")) != QStringLiteral("u8")
        || !unsignedValue(startField.value(QStringLiteral("value")), &startValue) || startValue > 255
        || !unsignedValue(endField.value(QStringLiteral("value")), &endValue) || endValue > 255
        || classField.value(QStringLiteral("parameter")) != QStringLiteral("class")
        || commandField.value(QStringLiteral("parameter")) != QStringLiteral("command")
        || lengthField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("length")
        || lengthField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("parameter"))
            != QStringLiteral("payload")
        || payloadField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("copy")
        || payloadField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("parameter"))
            != QStringLiteral("payload")
        || !offsetExpression(checksumField.value(QStringLiteral("offset")))
        || !offsetExpression(endField.value(QStringLiteral("offset")))
        || checksumField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("sum_modulo")
        || checksumField.value(QStringLiteral("derived")).toObject().value(QStringLiteral("parameter"))
            != QStringLiteral("payload")
        || !unsignedValue(checksumField.value(QStringLiteral("derived")).toObject()
                              .value(QStringLiteral("modulus")), &modulus) || modulus == 0)
        return fail(error, QStringLiteral("Radio frame field layout is unsupported."));
    const QJsonObject polling = root.value(QStringLiteral("polling")).toObject();
    const QJsonObject empty = polling.value(QStringLiteral("empty_response")).toObject();
    const QJsonObject nonempty = polling.value(QStringLiteral("nonempty_response")).toObject();
    const QJsonObject timeout = polling.value(QStringLiteral("timeout")).toObject();
    if (polling.value(QStringLiteral("delay_before_each_fetch_ms")).toInt(-1) < 0
        || polling.value(QStringLiteral("bulk_transfer_timeout_ms")).toInt() < 1
        || empty.value(QStringLiteral("all_bytes_equal")).toInt(-1) < 0
        || !trueField(empty, "validate_csw") || !trueField(empty, "continue_polling")
        || !trueField(nonempty, "validate_frame") || !trueField(nonempty, "validate_command_fields")
        || !trueField(nonempty, "validate_csw") || !trueField(timeout, "fail_on_poll_limit")
        || !falseField(timeout, "publish_partial_image"))
        return fail(error, QStringLiteral("Radio polling rules are incomplete."));
    return true;
}

bool validateReadRules(const QJsonObject &methods, QString *error)
{
    const QJsonObject identify = methods.value(QStringLiteral("identify")).toObject();
    const QJsonArray probes = identify.value(QStringLiteral("sequence")).toArray();
    bool expectsModel = false;
    if (probes.isEmpty() || !hexBytes(identify.value(QStringLiteral("model_response")).toObject()
                                                   .value(QStringLiteral("expected_hex"))))
        return fail(error, QStringLiteral("Radio identification is incomplete."));
    const QJsonObject modelResponse = identify.value(QStringLiteral("model_response")).toObject();
    const int modelOffset = modelResponse.value(QStringLiteral("frame_offset")).toInt(-1);
    const int modelBytes = modelResponse.value(QStringLiteral("bytes")).toInt(-1);
    if (modelOffset < 0 || modelBytes < 1
        || modelResponse.value(QStringLiteral("expected_hex")).toString().split(' ').size() != modelBytes)
        return fail(error, QStringLiteral("Radio model response field is invalid."));
    for (const QJsonValue &value : probes) {
        const QJsonObject probe = value.toObject();
        quint8 deviceClass = 0, command = 0;
        const QString expected = probe.value(QStringLiteral("expected")).toString();
        if (!byteValue(probe.value(QStringLiteral("class")), &deviceClass)
            || !byteValue(probe.value(QStringLiteral("command")), &command)
            || probe.value(QStringLiteral("repeat")).toInt() < 1
            || probe.value(QStringLiteral("fetch_bytes")).toInt() < 1
            || probe.value(QStringLiteral("poll_limit_per_repeat")).toInt() < 1
            || !hexBytesOrEmpty(probe.value(QStringLiteral("payload_hex")))
            || (expected != QStringLiteral("empty_polls") && expected != QStringLiteral("model_response")))
            return fail(error, QStringLiteral("Invalid radio identification probe."));
        if (expected == QStringLiteral("model_response")
            && modelOffset + modelBytes > probe.value(QStringLiteral("fetch_bytes")).toInt())
            return fail(error, QStringLiteral("Radio model response exceeds fetch size."));
        expectsModel |= expected == QStringLiteral("model_response");
    }
    if (!expectsModel) return fail(error, QStringLiteral("Radio identification does not check the model."));
    const QJsonObject setup = identify.value(QStringLiteral("setup")).toObject();
    quint8 setupClass = 0, setupCommand = 0;
    if (!byteValue(setup.value(QStringLiteral("class")), &setupClass)
        || !byteValue(setup.value(QStringLiteral("command")), &setupCommand)
        || !hexBytesOrEmpty(setup.value(QStringLiteral("payload_hex")))
        || setup.value(QStringLiteral("fetch_bytes")).toInt() < 1
        || setup.value(QStringLiteral("poll_limit")).toInt() < 1
        || !hexBytes(setup.value(QStringLiteral("expected_response_hex")))
        || setup.value(QStringLiteral("expected_response_hex")).toString().split(' ').size()
            != setup.value(QStringLiteral("fetch_bytes")).toInt())
        return fail(error, QStringLiteral("Radio setup exchange is incomplete."));
    const QJsonObject block = methods.value(QStringLiteral("read_block")).toObject();
    const QJsonObject selector = block.value(QStringLiteral("selector")).toObject();
    const QJsonArray rules = selector.value(QStringLiteral("rules_in_priority_order")).toArray();
    quint16 mask = 0;
    if (rules.isEmpty() || !wordValue(selector.value(QStringLiteral("index_mask")), &mask)
        || selector.value(QStringLiteral("maximum_block_index")).toInt(-1) < 0)
        return fail(error, QStringLiteral("Radio read selector rules are incomplete."));
    bool fallback = false;
    for (const QJsonValue &value : rules) {
        const QJsonObject rule = value.toObject();
        const QJsonObject condition = rule.value(QStringLiteral("when")).toObject();
        if (condition.isEmpty() || fallback)
            return fail(error, QStringLiteral("Invalid radio read selector order."));
        for (auto it = condition.begin(); it != condition.end(); ++it) {
            if (it.key() == QStringLiteral("block_index")) {
                if (!it.value().isDouble() || it.value().toInt(-1) < 0) return fail(error, QStringLiteral("Invalid block index condition."));
            } else if (it.key() == QStringLiteral("region")) {
                if (!block.value(QStringLiteral("region_commands")).toObject().contains(it.value().toString()))
                    return fail(error, QStringLiteral("Unknown read selector region."));
            } else if (it.key() == QStringLiteral("last_block")) {
                if (it.value() != QJsonValue(true)) return fail(error, QStringLiteral("Invalid last-block condition."));
            } else if (it.key() == QStringLiteral("otherwise")) {
                if (condition.size() != 1 || it.value() != QJsonValue(true))
                    return fail(error, QStringLiteral("Invalid fallback selector condition."));
                fallback = true;
            } else return fail(error, QStringLiteral("Unknown radio read selector condition."));
        }
        quint16 valueNumber = 0;
        if (rule.contains(QStringLiteral("value")) == rule.contains(QStringLiteral("or_with_block_index"))
            || !wordValue(rule.value(rule.contains(QStringLiteral("value")) ? QStringLiteral("value")
                                                                          : QStringLiteral("or_with_block_index")), &valueNumber))
            return fail(error, QStringLiteral("Invalid read selector value."));
    }
    if (!fallback) return fail(error, QStringLiteral("Radio read selector has no fallback."));
    const QJsonObject checks = block.value(QStringLiteral("response")).toObject()
        .value(QStringLiteral("checks")).toObject();
    quint8 requestClass = 0;
    const int blockBytes = block.value(QStringLiteral("block_bytes")).toInt();
    const int fetchBytes = block.value(QStringLiteral("fetch_bytes")).toInt();
    const QJsonArray payload = block.value(QStringLiteral("request_payload")).toArray();
    if (!byteValue(block.value(QStringLiteral("request_class")), &requestClass)
        || blockBytes < 1 || blockBytes > 65536 || fetchBytes < 1
        || block.value(QStringLiteral("poll_limit")).toInt() < 1 || payload.isEmpty())
        return fail(error, QStringLiteral("Radio read block request is incomplete."));
    bool hasSelector = false;
    for (const QJsonValue &value : payload) {
        const QJsonObject field = value.toObject();
        const int offset = field.value(QStringLiteral("payload_offset")).toInt(-1);
        const int width = typeBytes(field.value(QStringLiteral("type")).toString());
        const QString parameter = field.value(QStringLiteral("parameter")).toString();
        quint32 constant = 0;
        if (offset < 0 || width < 1 || offset + width > 65535
            || (!parameter.isEmpty() && parameter != QStringLiteral("selector"))
            || (parameter.isEmpty() && !unsignedValue(field.value(QStringLiteral("value")), &constant)))
            return fail(error, QStringLiteral("Unsupported radio read request payload field."));
        hasSelector |= parameter == QStringLiteral("selector");
    }
    if (!hasSelector) return fail(error, QStringLiteral("Radio read request has no selector."));
    const QJsonObject response = block.value(QStringLiteral("response")).toObject();
    const QJsonObject responseFields = response.value(QStringLiteral("fields")).toObject();
    const QJsonObject successField = responseFields.value(QStringLiteral("success")).toObject();
    const QJsonObject indexField = responseFields.value(QStringLiteral("block_index")).toObject();
    const QJsonObject sizeField = responseFields.value(QStringLiteral("region_size")).toObject();
    const QJsonObject dataField = responseFields.value(QStringLiteral("data")).toObject();
    quint32 successValue = 0;
    for (const QJsonObject &field : {successField, indexField, sizeField}) {
        const int offset = field.value(QStringLiteral("frame_offset")).toInt(-1);
        const int width = typeBytes(field.value(QStringLiteral("type")).toString());
        if (offset < 0 || width < 1 || offset + width > fetchBytes)
            return fail(error, QStringLiteral("Radio read response field exceeds fetch size."));
    }
    if (!unsignedValue(successField.value(QStringLiteral("expected")), &successValue)
        || dataField.value(QStringLiteral("frame_offset")).toInt(-1) < 0
        || dataField.value(QStringLiteral("bytes")).toInt(-1) != blockBytes
        || dataField.value(QStringLiteral("frame_offset")).toInt() + blockBytes > fetchBytes
        || response.value(QStringLiteral("payload_bytes")).toInt() < 1)
        return fail(error, QStringLiteral("Radio read response layout is incomplete."));
    if (!trueField(checks, "block_index_matches_selector_mask")
        || checks.value(QStringLiteral("region_size_minimum")).toInt() < 1
        || checks.value(QStringLiteral("region_size_maximum")).toInt() < 1
        || checks.value(QStringLiteral("region_size_minimum")).toInt()
            > checks.value(QStringLiteral("region_size_maximum")).toInt()
        || !trueField(checks, "region_size_matches_previous")
        || !trueField(checks, "block_within_region"))
        return fail(error, QStringLiteral("Radio read response checks are incomplete."));
    const QJsonObject read = methods.value(QStringLiteral("read_codeplug")).toObject();
    const QJsonObject assembly = read.value(QStringLiteral("assembly")).toObject();
    const QJsonArray order = assembly.value(QStringLiteral("region_order")).toArray();
    const QJsonArray operations = read.value(QStringLiteral("operations")).toArray();
    if (order.size() != operations.size() - 1
        || assembly.value(QStringLiteral("block_placement")) != QJsonValue(QStringLiteral("region_base_plus_block_index"))
        || assembly.value(QStringLiteral("valid_bytes_per_block")) != QJsonValue(QStringLiteral("clamp_to_region_end"))
        || !trueField(assembly, "require_every_block_once")
        || assembly.value(QStringLiteral("representation")).toString().isEmpty())
        return fail(error, QStringLiteral("Radio read assembly is incomplete."));
    for (int i = 0; i < order.size(); ++i)
        if (order.at(i) != operations.at(i + 1).toObject().value(QStringLiteral("region")))
            return fail(error, QStringLiteral("Radio read assembly order differs from operations."));
    const QJsonObject cancel = methods.value(QStringLiteral("cancel_read")).toObject();
    if (!trueField(cancel, "finish_active_exchange") || !trueField(cancel, "check_before_next_request")
        || cancel.value(QStringLiteral("cleanup")) != QJsonValue(QStringLiteral("interface_lifecycle"))
        || !falseField(cancel, "publish_partial_image"))
        return fail(error, QStringLiteral("Radio cancellation rules are incomplete."));
    return true;
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
        || payload.isEmpty())
        return fail(error, QStringLiteral("Invalid radio write request format."));
    bool hasImageBlock = false;
    bool hasSelector = false;
    bool hasExtent = false;
    for (const QJsonValue &value : payload) {
        const QJsonObject field = value.toObject();
        const QString parameter = field.value(QStringLiteral("parameter")).toString();
        quint32 constant = 0;
        if (!value.isObject()) return fail(error, QStringLiteral("Invalid write payload field."));
        if (field.contains(QStringLiteral("hex"))) {
            if (field.size() != 1 || !hexBytes(field.value(QStringLiteral("hex"))))
                return fail(error, QStringLiteral("Invalid write payload constant bytes."));
        } else if (parameter == QStringLiteral("image_block")) {
            if (field.value(QStringLiteral("bytes")).toInt() != blockBytes)
                return fail(error, QStringLiteral("Write image block size differs from plan."));
            hasImageBlock = true;
        } else if (parameter == QStringLiteral("selector") || parameter == QStringLiteral("extent")) {
            if (!typeBytes(field.value(QStringLiteral("type")).toString()))
                return fail(error, QStringLiteral("Unsupported write payload integer type."));
            hasSelector |= parameter == QStringLiteral("selector");
            hasExtent |= parameter == QStringLiteral("extent");
        } else if (field.contains(QStringLiteral("value"))) {
            if (!typeBytes(field.value(QStringLiteral("type")).toString())
                || !unsignedValue(field.value(QStringLiteral("value")), &constant))
                return fail(error, QStringLiteral("Invalid write payload integer constant."));
        } else return fail(error, QStringLiteral("Unknown write payload parameter."));
    }
    if (!hasImageBlock || !hasSelector || !hasExtent)
        return fail(error, QStringLiteral("Write payload lacks image block, selector, or extent."));
    const QJsonObject ack = plan.value(QStringLiteral("acknowledgement")).toObject();
    quint16 mask = 0;
    if (ack.value(QStringLiteral("fetch_bytes")).toInt() < 1
        || ack.value(QStringLiteral("poll_limit")).toInt() < 1
        || ack.value(QStringLiteral("payload_bytes")).toInt() < 1
        || ack.value(QStringLiteral("index_offset")).toInt(-1) < 0
        || typeBytes(ack.value(QStringLiteral("index_type")).toString()) < 1
        || ack.value(QStringLiteral("index_offset")).toInt(-1)
            + typeBytes(ack.value(QStringLiteral("index_type")).toString())
            > ack.value(QStringLiteral("fetch_bytes")).toInt()
        || !wordValue(ack.value(QStringLiteral("index_mask")), &mask)
        || ack.value(QStringLiteral("duplicate")) != QJsonValue(QStringLiteral("skip_previous")))
        return fail(error, QStringLiteral("Invalid radio write acknowledgement format."));
    return true;
}
}

bool RadioDefinition::load(const QString &path, RadioDefinition *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stdout, "[radio definitions] ERROR opening FILE: %s\n", qUtf8Printable(path));
        std::fflush(stdout);
        return fail(error, QStringLiteral("Cannot read radio definition: %1").arg(path));
    }
    const QByteArray json = file.readAll();
    const QString absolutePath = QFileInfo(file).absoluteFilePath();
    const QByteArray hash = QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex();
    std::fprintf(stdout, "[radio definitions] Reading FILE: %s\n[radio definitions] JSON SHA-256: %s\n",
                 qUtf8Printable(absolutePath), hash.constData());
    std::fflush(stdout);
    if (!fromJson(json, out, error)) {
        std::fprintf(stdout, "[radio definitions] ERROR loading FILE: %s: %s\n",
                     qUtf8Printable(absolutePath), error ? qUtf8Printable(*error) : "invalid definition");
        std::fflush(stdout);
        return false;
    }
    out->m_sourcePath = absolutePath;
    std::fprintf(stdout, "[radio definitions] Loaded ID: %s\n", qUtf8Printable(out->m_document.value(QStringLiteral("id")).toString()));
    std::fflush(stdout);
    return true;
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
    if (root.contains(QStringLiteral("dfu_control_transport"))) {
        if (!validateDfuRules(root, error)) return false;
        out->m_readOperations.clear();
        out->m_writePlan = methods.value(QStringLiteral("write_codeplug")).toObject();
        out->m_document = root;
        out->m_sourcePath.clear();
        out->m_dfu = true;
        if (error) error->clear();
        return true;
    }
    if (!validateTransportRules(root, error) || !validateReadRules(methods, error)) return false;
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
    out->m_document = root;
    out->m_sourcePath.clear();
    out->m_dfu = false;
    if (error) error->clear();
    return true;
}

bool RadioDefinition::compatibleWithCodeplug(const QJsonObject &codeplugDocument, QString *error) const
{
    if (m_document.value(QStringLiteral("id")) != codeplugDocument.value(QStringLiteral("id")))
        return fail(error, QStringLiteral("Radio and codeplug definitions have different IDs."));
    const QJsonObject methods = m_document.value(QStringLiteral("methods")).toObject();
    const QString readName = methods.value(QStringLiteral("read_codeplug")).toObject()
        .value(QStringLiteral("assembly")).toObject().value(QStringLiteral("representation")).toString();
    const QString writeName = m_writePlan.value(QStringLiteral("representation")).toString();
    bool readFound = false, writeFound = writeName.isEmpty();
    for (const QJsonValue &value : codeplugDocument.value(QStringLiteral("representations")).toArray()) {
        const QJsonObject representation = value.toObject();
        const QString name = representation.value(QStringLiteral("name")).toString();
        if (name != readName && name != writeName) continue;
        if (representation.value(QStringLiteral("decode")).toObject().value(QStringLiteral("operation"))
            != QStringLiteral("identity"))
            return fail(error, QStringLiteral("Radio transfer representation is not a decoded image: %1").arg(name));
        readFound |= name == readName;
        writeFound |= name == writeName;
    }
    if (!readFound || !writeFound)
        return fail(error, QStringLiteral("Radio transfer representation is absent from the codeplug definition."));
    if (error) error->clear();
    return true;
}
