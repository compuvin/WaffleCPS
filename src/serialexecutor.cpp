#include "serialexecutor.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace {
using Bytes = RadioExecutor::Bytes;
using Object = QJsonObject;

bool number(const QJsonValue &value, quint32 *result)
{
    bool ok = false;
    quint64 parsed = 0;
    if (value.isDouble()) {
        const double raw = value.toDouble();
        if (raw < 0 || raw > 4294967295.0 || raw != quint32(raw)) return false;
        parsed = quint32(raw);
    } else if (value.isString()) {
        const QString raw = value.toString();
        parsed = raw.toULongLong(&ok, raw.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
        if (!ok || parsed > 4294967295ULL) return false;
    } else return false;
    *result = quint32(parsed);
    return true;
}

quint32 requiredNumber(const Object &object, const char *key)
{
    quint32 result = 0;
    if (!number(object.value(QLatin1String(key)), &result))
        throw std::runtime_error(QStringLiteral("Invalid serial definition number: %1").arg(QLatin1String(key)).toStdString());
    return result;
}

bool hex(const QJsonValue &value, QByteArray *result)
{
    if (!value.isString()) return false;
    QByteArray raw = value.toString().toLatin1();
    raw.replace(" ", "");
    if (raw.isEmpty() || raw.size() % 2) return false;
    for (char c : raw) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    *result = QByteArray::fromHex(raw);
    return !result->isEmpty();
}

QByteArray requiredHex(const Object &object, const char *key)
{
    QByteArray result;
    if (!hex(object.value(QLatin1String(key)), &result))
        throw std::runtime_error(QStringLiteral("Invalid serial definition bytes: %1").arg(QLatin1String(key)).toStdString());
    return result;
}

bool bad(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

struct Segment { quint32 file, radio, length; };

std::vector<Segment> segments(const Object &method)
{
    std::vector<Segment> result;
    for (const QJsonValue &value : method.value(QStringLiteral("segments")).toArray()) {
        const Object item = value.toObject();
        result.push_back({requiredNumber(item, "image_offset"), requiredNumber(item, "radio_address"),
                          requiredNumber(item, "length")});
    }
    return result;
}

void appendBe(QByteArray *bytes, quint32 value, int width)
{
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) bytes->append(char((value >> shift) & 255));
}

quint32 little(const QByteArray &bytes, int offset, int width)
{
    quint32 result = 0;
    for (int i = 0; i < width; ++i) result |= quint32(quint8(bytes[offset + i])) << (8 * i);
    return result;
}

class Port {
public:
    Port(const Object &transport, const std::atomic_bool *cancelled)
        : m_cancelled(cancelled), m_timeout(int(requiredNumber(transport, "timeout_ms")))
    {
        const quint32 vendor = requiredNumber(transport, "vendor_id");
        const quint32 product = requiredNumber(transport, "product_id");
        QString chosen;
        for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
            if (info.hasVendorIdentifier() && info.hasProductIdentifier()
                && info.vendorIdentifier() == vendor && info.productIdentifier() == product) {
                if (!chosen.isEmpty()) throw std::runtime_error("Multiple matching serial radios are connected");
                chosen = info.portName();
            }
        }
        if (chosen.isEmpty()) throw std::runtime_error("No matching serial radio was found");
        m_port.setPortName(chosen);
        m_port.setBaudRate(int(requiredNumber(transport, "baud_rate")));
        m_port.setDataBits(QSerialPort::Data8);
        m_port.setParity(QSerialPort::NoParity);
        m_port.setStopBits(QSerialPort::OneStop);
        m_port.setFlowControl(QSerialPort::NoFlowControl);
        if (!m_port.open(QIODevice::ReadWrite))
            throw std::runtime_error(QStringLiteral("Cannot open serial radio %1: %2")
                .arg(chosen, m_port.errorString()).toStdString());
    }

    void checkpoint() const { if (m_cancelled && m_cancelled->load()) throw RadioExecutor::Cancelled(); }

    QByteArray exchange(const QByteArray &request, int expectedBytes)
    {
        checkpoint();
        if (request.isEmpty() || expectedBytes < 1) throw std::runtime_error("Invalid serial exchange");
        QElapsedTimer timer;
        timer.start();
        qint64 sent = 0;
        while (sent < request.size()) {
            checkpoint();
            const qint64 count = m_port.write(request.constData() + sent, request.size() - sent);
            if (count < 0) throw std::runtime_error(m_port.errorString().toStdString());
            sent += count;
            if (!m_port.waitForBytesWritten(std::max(1, m_timeout - int(timer.elapsed()))))
                throw std::runtime_error(QStringLiteral("Serial write failed: %1").arg(m_port.errorString()).toStdString());
            if (timer.elapsed() >= m_timeout) throw std::runtime_error("Serial write timed out");
        }
        QByteArray response;
        while (response.size() < expectedBytes) {
            checkpoint();
            response += m_port.read(expectedBytes - response.size());
            if (response.size() == expectedBytes) break;
            if (timer.elapsed() >= m_timeout || !m_port.waitForReadyRead(std::max(1, m_timeout - int(timer.elapsed()))))
                throw std::runtime_error(QStringLiteral("Serial response timed out: %1").arg(m_port.errorString()).toStdString());
        }
        return response;
    }

private:
    QSerialPort m_port;
    const std::atomic_bool *m_cancelled;
    int m_timeout;
};

void commands(Port &port, const QJsonArray &sequence)
{
    for (const QJsonValue &value : sequence) {
        const Object step = value.toObject();
        const QByteArray expected = requiredHex(step, "response_hex");
        if (port.exchange(requiredHex(step, "request_hex"), expected.size()) != expected)
            throw std::runtime_error("Serial radio rejected a setup or finish command");
    }
}

void identify(Port &port, const Object &transport)
{
    const Object rule = transport.value(QStringLiteral("identity")).toObject();
    const QByteArray request = requiredHex(rule, "request_hex");
    const int payloadBytes = int(requiredNumber(rule, "response_length"));
    const QByteArray response = port.exchange(request, payloadBytes + 3);
    const QByteArray prefix = requiredHex(rule, "response_prefix_hex");
    if (!response.startsWith(prefix) || quint8(response[1]) != quint8(payloadBytes >> 8)
        || quint8(response[2]) != quint8(payloadBytes))
        throw std::runtime_error("Serial radio returned invalid identity frame");
    const int offset = int(requiredNumber(rule, "radio_type_offset"));
    const quint32 radioType = little(response, offset + 3, 4);
    for (const QJsonValue &allowed : rule.value(QStringLiteral("allowed_radio_types")).toArray()) {
        quint32 value = 0;
        if (number(allowed, &value) && value == radioType) return;
    }
    throw std::runtime_error(QStringLiteral("Serial radio firmware type %1 is not supported by this definition")
        .arg(radioType).toStdString());
}

QByteArray readBlock(Port &port, const Object &transport, quint32 address)
{
    const int block = int(requiredNumber(transport, "block_bytes"));
    const Object read = transport.value(QStringLiteral("read")).toObject();
    QByteArray request = requiredHex(read, "request_prefix_hex");
    appendBe(&request, address, 4);
    appendBe(&request, quint32(block), 2);
    const QByteArray response = port.exchange(request, block + 3);
    if (!response.startsWith(requiredHex(read, "response_prefix_hex"))
        || quint8(response[1]) != quint8(block >> 8) || quint8(response[2]) != quint8(block))
        throw std::runtime_error("Serial radio returned an invalid flash block");
    return response.mid(3);
}

void writeCommand(Port &port, const QByteArray &request, const QByteArray &expected)
{
    if (port.exchange(request, expected.size()) != expected)
        throw std::runtime_error("Serial radio rejected a flash write command");
}

} // namespace

bool SerialExecutor::validate(const QJsonObject &root, QString *error)
{
    try {
        const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
        const Object methods = root.value(QStringLiteral("methods")).toObject();
        const Object read = methods.value(QStringLiteral("read_codeplug")).toObject();
        const Object write = methods.value(QStringLiteral("write_codeplug")).toObject();
        const Object assembly = read.value(QStringLiteral("assembly")).toObject();
        const quint32 block = requiredNumber(transport, "block_bytes");
        const quint32 sector = requiredNumber(transport, "sector_bytes");
        const quint32 bytes = requiredNumber(assembly, "bytes");
        if (requiredNumber(transport, "vendor_id") > 65535 || requiredNumber(transport, "product_id") > 65535
            || requiredNumber(transport, "baud_rate") < 1200 || requiredNumber(transport, "timeout_ms") < 100
            || block < 1 || block > 1024 || sector < block || sector > 65536 || sector % block
            || bytes < 1 || bytes > 16 * 1024 * 1024
            || assembly.value(QStringLiteral("operation")) != QStringLiteral("replace_regions_in_template")
            || assembly.value(QStringLiteral("representation")).toString().isEmpty()
            || write.value(QStringLiteral("representation")) != assembly.value(QStringLiteral("representation"))
            || write.value(QStringLiteral("codeplug_definition_id")) != root.value(QStringLiteral("id"))
            || write.value(QStringLiteral("preserve_unmapped_sector_bytes")) != true
            || write.value(QStringLiteral("verify_after_write")) != true)
            return bad(error, QStringLiteral("Invalid serial transport or image assembly settings."));
        const Object identity = transport.value(QStringLiteral("identity")).toObject();
        const quint32 identitySize = requiredNumber(identity, "response_length");
        if (requiredHex(identity, "request_hex").size() < 2
            || requiredHex(identity, "response_prefix_hex").size() != 1
            || identitySize < 8 || identitySize > 256
            || requiredNumber(identity, "radio_type_offset") > identitySize - 4
            || identity.value(QStringLiteral("radio_type_type")) != QStringLiteral("u32le")
            || identity.value(QStringLiteral("allowed_radio_types")).toArray().isEmpty())
            return bad(error, QStringLiteral("Invalid serial identity rule."));
        const Object readRule = transport.value(QStringLiteral("read")).toObject();
        const Object writeRule = transport.value(QStringLiteral("write")).toObject();
        if (requiredHex(readRule, "request_prefix_hex").size() != 2
            || requiredHex(readRule, "response_prefix_hex").size() != 1
            || requiredHex(writeRule, "request_prefix_hex").size() != 1
            || requiredNumber(writeRule, "set_sector_command") > 255
            || requiredNumber(writeRule, "write_buffer_command") > 255
            || requiredNumber(writeRule, "commit_sector_command") > 255
            || readRule.value(QStringLiteral("address_type")) != QStringLiteral("u32be")
            || readRule.value(QStringLiteral("length_type")) != QStringLiteral("u16be")
            || writeRule.value(QStringLiteral("address_type")) != QStringLiteral("u32be")
            || writeRule.value(QStringLiteral("length_type")) != QStringLiteral("u16be")
            || writeRule.value(QStringLiteral("sector_type")) != QStringLiteral("u24be"))
            return bad(error, QStringLiteral("Unsupported serial packet layout."));
        auto validCommands = [](const QJsonValue &value) {
            const QJsonArray commands = value.toArray();
            if (commands.isEmpty()) return false;
            for (const QJsonValue &item : commands) {
                const Object step = item.toObject();
                QByteArray request, response;
                if (!hex(step.value(QStringLiteral("request_hex")), &request)
                    || !hex(step.value(QStringLiteral("response_hex")), &response)
                    || request.size() > 256 || response.size() > 256) return false;
            }
            return true;
        };
        if (!validCommands(read.value(QStringLiteral("setup"))) || !validCommands(read.value(QStringLiteral("finish")))
            || !validCommands(write.value(QStringLiteral("setup"))) || !validCommands(write.value(QStringLiteral("finish"))))
            return bad(error, QStringLiteral("Invalid serial setup or finish sequence."));
        const auto readSegments = segments(read), writeSegments = segments(write);
        if (readSegments.empty() || readSegments.size() != writeSegments.size())
            return bad(error, QStringLiteral("Serial read and write segments differ."));
        for (size_t i = 0; i < readSegments.size(); ++i) {
            const Segment a = readSegments[i], b = writeSegments[i];
            if (a.file != b.file || a.radio != b.radio || a.length != b.length
                || !a.length || a.file % block || a.radio % block || a.length % block
                || quint64(a.file) + a.length > bytes || quint64(a.radio) + a.length > 0x100000000ULL)
                return bad(error, QStringLiteral("Serial segment is invalid or read/write mappings differ."));
            for (size_t j = 0; j < i; ++j) {
                const Segment prior = readSegments[j];
                if ((a.file < prior.file + prior.length && prior.file < a.file + a.length)
                    || (a.radio < prior.radio + prior.length && prior.radio < a.radio + a.length))
                    return bad(error, QStringLiteral("Serial segments overlap."));
            }
        }
    } catch (const std::exception &exception) {
        return bad(error, QString::fromUtf8(exception.what()));
    }
    if (error) error->clear();
    return true;
}

RadioExecutor::Bytes SerialExecutor::read(const RadioDefinition &definition,
                                           const RadioExecutor::Progress &progress,
                                           const std::atomic_bool &cancelled)
{
    const Object root = definition.document();
    const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
    const Object method = root.value(QStringLiteral("methods")).toObject().value(QStringLiteral("read_codeplug")).toObject();
    const quint32 block = requiredNumber(transport, "block_bytes");
    Bytes image(requiredNumber(method.value(QStringLiteral("assembly")).toObject(), "bytes"), 0);
    Port port(transport, &cancelled);
    identify(port, transport);
    commands(port, method.value(QStringLiteral("setup")).toArray());
    quint32 total = 0, done = 0;
    const auto mapped = segments(method);
    for (const Segment &item : mapped) total += item.length / block;
    try {
        for (const Segment &item : mapped) {
            for (quint32 at = 0; at < item.length; at += block) {
                port.checkpoint();
                const QByteArray data = readBlock(port, transport, item.radio + at);
                std::copy(data.cbegin(), data.cend(), image.begin() + item.file + at);
                if (progress) progress("flash", ++done, total);
            }
        }
    } catch (...) {
        try { commands(port, method.value(QStringLiteral("finish")).toArray()); } catch (...) {}
        throw;
    }
    commands(port, method.value(QStringLiteral("finish")).toArray());
    return image;
}

void SerialExecutor::write(const RadioDefinition &definition, const Bytes &image,
                           const RadioExecutor::Progress &progress)
{
    const Object root = definition.document();
    const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
    const Object method = definition.writePlan();
    const Object assembly = root.value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("read_codeplug")).toObject().value(QStringLiteral("assembly")).toObject();
    if (image.size() != requiredNumber(assembly, "bytes")) throw std::runtime_error("Serial codeplug image has the wrong size");
    const quint32 block = requiredNumber(transport, "block_bytes");
    const quint32 sector = requiredNumber(transport, "sector_bytes");
    const Object writeRule = transport.value(QStringLiteral("write")).toObject();
    const QByteArray prefix = requiredHex(writeRule, "request_prefix_hex");
    // A sector may contain bytes outside the .wcp image. Read it before changing any byte.
    std::map<quint32, QByteArray> sectors;
    for (const Segment &item : segments(method)) {
        for (quint32 at = 0; at < item.length; at += block) {
            const quint32 address = item.radio + at;
            sectors.emplace(address / sector, QByteArray());
        }
    }
    Port port(transport, nullptr);
    identify(port, transport);
    commands(port, method.value(QStringLiteral("setup")).toArray());
    try {
        quint32 readDone = 0;
        for (auto &[index, content] : sectors) {
            content.reserve(sector);
            for (quint32 at = 0; at < sector; at += block)
                content += readBlock(port, transport, index * sector + at);
            if (progress) progress("preserve flash", ++readDone, quint32(sectors.size()));
        }
        std::map<quint32, QByteArray> original = sectors;
        for (const Segment &item : segments(method)) {
            for (quint32 at = 0; at < item.length; at += block) {
                const quint32 address = item.radio + at;
                sectors.at(address / sector).replace(address % sector, block,
                    reinterpret_cast<const char *>(image.data() + item.file + at), block);
            }
        }
        quint32 writeDone = 0;
        for (const auto &[index, content] : sectors) {
            if (content == original.at(index)) { ++writeDone; continue; }
            QByteArray request = prefix;
            request.append(char(requiredNumber(writeRule, "set_sector_command")));
            appendBe(&request, index, 3);
            writeCommand(port, request, request.left(2));
            for (quint32 at = 0; at < sector; at += block) {
                QByteArray packet = prefix;
                packet.append(char(requiredNumber(writeRule, "write_buffer_command")));
                appendBe(&packet, index * sector + at, 4);
                appendBe(&packet, block, 2);
                packet += content.mid(at, block);
                writeCommand(port, packet, packet.left(2));
            }
            request = prefix;
            request.append(char(requiredNumber(writeRule, "commit_sector_command")));
            writeCommand(port, request, request);
            QByteArray verify;
            verify.reserve(sector);
            for (quint32 at = 0; at < sector; at += block)
                verify += readBlock(port, transport, index * sector + at);
            if (verify != content) throw std::runtime_error("Serial flash verification failed");
            if (progress) progress("flash", ++writeDone, quint32(sectors.size()));
        }
    } catch (...) {
        try { commands(port, method.value(QStringLiteral("finish")).toArray()); } catch (...) {}
        throw;
    }
    commands(port, method.value(QStringLiteral("finish")).toArray());
}
