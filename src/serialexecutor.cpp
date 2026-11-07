#include "serialexecutor.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QThread>
#include <algorithm>
#include <map>
#include <set>
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

using UsbId = std::pair<quint16, quint16>;

std::set<UsbId> serialUsbIds(const Object &transport)
{
    const quint32 vendor = requiredNumber(transport, "vendor_id");
    const quint32 product = requiredNumber(transport, "product_id");
    if (vendor > 65535 || product > 65535)
        throw std::runtime_error("Invalid serial USB ID");
    std::set<UsbId> ids{{quint16(vendor), quint16(product)}};
    const QJsonValue additional = transport.value(QStringLiteral("additional_usb_ids"));
    if (additional.isUndefined()) return ids;
    if (!additional.isArray() || additional.toArray().isEmpty() || additional.toArray().size() > 16)
        throw std::runtime_error("Invalid additional serial USB IDs");
    for (const QJsonValue &value : additional.toArray()) {
        if (!value.isObject()) throw std::runtime_error("Invalid additional serial USB ID");
        const Object entry = value.toObject();
        const quint32 extraVendor = requiredNumber(entry, "vendor_id");
        const quint32 extraProduct = requiredNumber(entry, "product_id");
        if (extraVendor > 65535 || extraProduct > 65535
            || !ids.insert({quint16(extraVendor), quint16(extraProduct)}).second)
            throw std::runtime_error("Invalid or duplicate additional serial USB ID");
    }
    return ids;
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
        : m_cancelled(cancelled), m_timeout(int(requiredNumber(transport, "timeout_ms"))),
          m_paged(transport.value(QStringLiteral("protocol")) == QStringLiteral("paged_addressed_serial_v1")),
          m_closeResetMs(m_paged ? int(requiredNumber(transport, "close_reset_ms")) : 0)
    {
        const auto ids = serialUsbIds(transport);
        QString chosen;
        for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
            if (info.hasVendorIdentifier() && info.hasProductIdentifier()
                && ids.count({info.vendorIdentifier(), info.productIdentifier()})) {
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
        if (m_paged) {
            if (!m_port.setDataTerminalReady(true))
                throw std::runtime_error("Cannot assert serial DTR for radio programming");
            QThread::msleep(requiredNumber(transport, "open_delay_ms"));
            m_port.clear(QSerialPort::Input);
        }
    }

    ~Port()
    {
        if (m_paged && m_port.isOpen()) {
            m_port.setDataTerminalReady(false);
            QThread::msleep(m_closeResetMs);
        }
    }

    void checkpoint() const { if (m_cancelled && m_cancelled->load()) throw RadioExecutor::Cancelled(); }

    QByteArray exchange(const QByteArray &request, int expectedBytes, unsigned preReadDelayMs = 0)
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
        if (preReadDelayMs) QThread::msleep(preReadDelayMs);
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

    QByteArray receive(int expectedBytes, int timeoutMs = -1)
    {
        checkpoint();
        if (expectedBytes < 0 || expectedBytes > 65536)
            throw std::runtime_error("Invalid serial response length");
        if (timeoutMs < 0) timeoutMs = m_timeout;
        QByteArray response;
        QElapsedTimer timer;
        timer.start();
        while (response.size() < expectedBytes) {
            checkpoint();
            response += m_port.read(expectedBytes - response.size());
            if (response.size() == expectedBytes) break;
            if (timer.elapsed() >= timeoutMs || !m_port.waitForReadyRead(std::max(1, timeoutMs - int(timer.elapsed()))))
                throw std::runtime_error(QStringLiteral("Serial response timed out: %1").arg(m_port.errorString()).toStdString());
        }
        return response;
    }

private:
    QSerialPort m_port;
    const std::atomic_bool *m_cancelled;
    int m_timeout;
    bool m_paged;
    int m_closeResetMs;
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

void appendLe(QByteArray *bytes, quint32 value, int width)
{
    for (int i = 0; i < width; ++i) bytes->append(char((value >> (8 * i)) & 255));
}

void pagedSteps(Port &port, const QJsonArray &steps)
{
    for (const QJsonValue &value : steps) {
        const Object step = value.toObject();
        const QByteArray request = requiredHex(step, "request_hex");
        const QByteArray prefix = requiredHex(step, "response_prefix_hex");
        const quint32 length = requiredNumber(step, "response_bytes");
        const QByteArray response = port.exchange(request, int(length), requiredNumber(step, "pre_read_delay_ms"));
        if (response.size() != int(length) || !response.startsWith(prefix))
            throw std::runtime_error("Serial radio rejected a setup command");
        const QJsonArray allowed = step.value(QStringLiteral("allowed_response_text")).toArray();
        if (!allowed.isEmpty()) {
            bool matched = false;
            for (const QJsonValue &text : allowed)
                matched |= !text.toString().isEmpty() && response.contains(text.toString().toLatin1());
            if (!matched) throw std::runtime_error("Serial radio identity does not match this definition");
        }
    }
}

QPair<quint32, quint32> pagedMemoryRange(Port &port, const Object &transport)
{
    const Object rule = transport.value(QStringLiteral("memory_query")).toObject();
    const QByteArray prefix = requiredHex(rule, "response_prefix_hex");
    const QByteArray header = port.exchange(requiredHex(rule, "request_hex"), 3);
    if (prefix.size() != 2 || !header.startsWith(prefix)
        || quint8(header[2]) != requiredNumber(rule, "payload_bytes"))
        throw std::runtime_error("Serial radio returned an invalid memory range header");
    const QByteArray payload = port.receive(quint8(header[2]));
    const int lowerOffset = int(requiredNumber(rule, "lower_offset"));
    const int upperOffset = int(requiredNumber(rule, "upper_offset"));
    if (lowerOffset + 4 > payload.size() || upperOffset + 4 > payload.size())
        throw std::runtime_error("Serial radio memory range is truncated");
    return {little(payload, lowerOffset, 4), little(payload, upperOffset, 4)};
}

void pagedConnect(Port &port, const Object &transport)
{
    pagedSteps(port, transport.value(QStringLiteral("identity_steps")).toArray());
}

QByteArray pagedRead(Port &port, const Object &transport, quint32 address, quint32 length)
{
    const Object rule = transport.value(QStringLiteral("read_packet")).toObject();
    QByteArray request = requiredHex(rule, "request_hex");
    appendLe(&request, address, 3);
    appendLe(&request, length, 2);
    const QByteArray header = port.exchange(request, 6, requiredNumber(rule, "pre_read_delay_ms"));
    if (!header.startsWith(requiredHex(rule, "response_hex"))
        || little(header, 1, 3) != address || little(header, 4, 2) != length)
        throw std::runtime_error("Serial radio returned an invalid page header");
    const QByteArray payload = port.receive(int(length), int(requiredNumber(rule, "payload_timeout_ms")));
    if (payload.size() != int(length)) throw std::runtime_error("Serial radio page is truncated");
    return payload;
}

void pagedWrite(Port &port, const Object &transport, quint32 address, const QByteArray &page)
{
    const Object rule = transport.value(QStringLiteral("write_packet")).toObject();
    QByteArray request = requiredHex(rule, "request_hex");
    appendLe(&request, address, 3);
    appendLe(&request, quint32(page.size()), 2);
    request += page;
    if (port.exchange(request, 1, requiredNumber(rule, "pre_read_delay_ms"))
        != requiredHex(rule, "response_hex"))
        throw std::runtime_error("Serial radio rejected a page write");
}

std::map<quint8, quint32> pagedMap(Port &port, const Object &transport,
                                   const QPair<quint32, quint32> &range,
                                   const RadioExecutor::Progress &progress,
                                   const char *phase)
{
    const Object page = transport.value(QStringLiteral("page_map")).toObject();
    const quint32 bytes = requiredNumber(page, "page_bytes");
    const quint32 tagOffset = requiredNumber(page, "id_offset");
    const quint32 maxPages = requiredNumber(page, "maximum_pages");
    if (!bytes || tagOffset >= bytes || range.first % bytes || range.second < range.first
        || range.second > 0xFFFFFF || (range.second - range.first) / bytes + 1 > maxPages)
        throw std::runtime_error("Serial radio page map has an invalid range");
    const quint32 count = (range.second - range.first) / bytes + 1;
    std::map<quint8, quint32> mapped;
    for (quint32 index = 0; index < count; ++index) {
        port.checkpoint();
        const quint32 address = range.first + index * bytes;
        const QByteArray tag = pagedRead(port, transport, address + tagOffset, 1);
        const quint8 id = quint8(tag[0]);
        if (id != 0 && id != 255 && !mapped.emplace(id, address).second)
            throw std::runtime_error("Serial radio has duplicate logical page IDs");
        if (progress) progress(phase, index + 1, count);
        QThread::msleep(requiredNumber(page, "probe_delay_ms"));
    }
    return mapped;
}

std::map<quint8, QByteArray> imagePages(const Bytes &image, const Object &page)
{
    const quint32 bytes = requiredNumber(page, "page_bytes");
    const quint32 tagOffset = requiredNumber(page, "id_offset");
    if (!bytes || tagOffset >= bytes || image.size() % bytes)
        throw std::runtime_error("Serial codeplug pages have an invalid size");
    std::map<quint8, QByteArray> result;
    for (size_t at = 0; at < image.size(); at += bytes) {
        const quint8 id = image[at + tagOffset];
        if (id == 0 || id == 255) continue;
        if (!result.emplace(id, QByteArray(reinterpret_cast<const char *>(image.data() + at), bytes)).second)
            throw std::runtime_error("Serial codeplug contains duplicate logical page IDs");
    }
    return result;
}

bool validatePaged(const Object &root, QString *error)
{
    try {
        const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
        const Object page = transport.value(QStringLiteral("page_map")).toObject();
        const Object memory = transport.value(QStringLiteral("memory_query")).toObject();
        const Object readPacket = transport.value(QStringLiteral("read_packet")).toObject();
        const Object writePacket = transport.value(QStringLiteral("write_packet")).toObject();
        const Object methods = root.value(QStringLiteral("methods")).toObject();
        const Object read = methods.value(QStringLiteral("read_codeplug")).toObject();
        const Object write = methods.value(QStringLiteral("write_codeplug")).toObject();
        const Object assembly = read.value(QStringLiteral("assembly")).toObject();
        const quint32 bytes = requiredNumber(page, "page_bytes");
        const quint32 imageBytes = requiredNumber(assembly, "bytes");
        serialUsbIds(transport);
        if (requiredNumber(transport, "baud_rate") < 1200
            || requiredNumber(transport, "timeout_ms") < 100
            || requiredNumber(transport, "open_delay_ms") > 10000
            || requiredNumber(transport, "close_reset_ms") > 10000
            || bytes != 4096 || requiredNumber(page, "id_offset") != bytes - 1
            || requiredNumber(page, "maximum_pages") < 1 || requiredNumber(page, "maximum_pages") > 4096
            || requiredNumber(page, "probe_delay_ms") > 1000
            || imageBytes < bytes || imageBytes > 16 * 1024 * 1024 || imageBytes % bytes
            || assembly.value(QStringLiteral("operation")) != QStringLiteral("replace_pages_by_id_in_template")
            || assembly.value(QStringLiteral("representation")).toString().isEmpty()
            || write.value(QStringLiteral("codeplug_definition_id")) != root.value(QStringLiteral("id"))
            || write.value(QStringLiteral("representation")) != assembly.value(QStringLiteral("representation"))
            || write.value(QStringLiteral("verify_after_write")) != true
            || requiredNumber(page, "unallocated_write_address") > 0xFFFFFF
            || requiredNumber(memory, "payload_bytes") != 8
            || requiredNumber(memory, "lower_offset") > 4
            || requiredNumber(memory, "upper_offset") > 4
            || requiredHex(memory, "request_hex").size() != 5
            || requiredHex(memory, "response_prefix_hex").size() != 2
            || requiredHex(readPacket, "request_hex").size() != 1
            || requiredHex(readPacket, "response_hex").size() != 1
            || requiredHex(writePacket, "request_hex").size() != 1
            || requiredHex(writePacket, "response_hex").size() != 1
            || readPacket.value(QStringLiteral("address_type")) != QStringLiteral("u24le")
            || readPacket.value(QStringLiteral("length_type")) != QStringLiteral("u16le")
            || writePacket.value(QStringLiteral("address_type")) != QStringLiteral("u24le")
            || writePacket.value(QStringLiteral("length_type")) != QStringLiteral("u16le")
            || requiredNumber(readPacket, "pre_read_delay_ms") > 1000
            || requiredNumber(readPacket, "payload_timeout_ms") < 1000
            || requiredNumber(writePacket, "pre_read_delay_ms") > 1000
            || requiredNumber(page, "block_delay_ms") > 1000
            || transport.value(QStringLiteral("identity_steps")).toArray().isEmpty()
            || transport.value(QStringLiteral("enter_program_steps")).toArray().isEmpty())
            return bad(error, QStringLiteral("Invalid paged serial transport or image assembly settings."));
        for (const char *key : {"identity_steps", "enter_program_steps"}) {
            for (const QJsonValue &value : transport.value(QLatin1String(key)).toArray()) {
                const Object step = value.toObject();
                const quint32 responseBytes = requiredNumber(step, "response_bytes");
                if (responseBytes < 1 || responseBytes > 256
                    || requiredHex(step, "response_prefix_hex").size() > int(responseBytes)
                    || requiredHex(step, "request_hex").size() > 256
                    || requiredNumber(step, "pre_read_delay_ms") > 1000)
                    return bad(error, QStringLiteral("Invalid paged serial setup step."));
                for (const QJsonValue &text : step.value(QStringLiteral("allowed_response_text")).toArray())
                    if (text.toString().isEmpty() || text.toString().size() > int(responseBytes))
                        return bad(error, QStringLiteral("Invalid paged serial identity text."));
            }
        }
    } catch (const std::exception &exception) {
        return bad(error, QString::fromUtf8(exception.what()));
    }
    if (error) error->clear();
    return true;
}

Bytes readPagedCodeplug(const RadioDefinition &definition,
                        const RadioExecutor::Progress &progress,
                        const std::atomic_bool &cancelled)
{
    const Object transport = definition.document().value(QStringLiteral("serial_transport")).toObject();
    const Object pageRule = transport.value(QStringLiteral("page_map")).toObject();
    const quint32 pageBytes = requiredNumber(pageRule, "page_bytes");
    Bytes image(256 * pageBytes, 0);
    Port port(transport, &cancelled);
    pagedConnect(port, transport);
    const auto range = pagedMemoryRange(port, transport);
    pagedSteps(port, transport.value(QStringLiteral("enter_program_steps")).toArray());
    const auto mapped = pagedMap(port, transport, range, progress, "discover pages");
    quint32 done = 0;
    for (const auto &[id, address] : mapped) {
        port.checkpoint();
        const QByteArray contents = pagedRead(port, transport, address, pageBytes);
        if (quint8(contents[int(pageBytes) - 1]) != id)
            throw std::runtime_error("Serial radio page ID changed during read");
        std::copy(contents.cbegin(), contents.cend(), image.begin() + size_t(id) * pageBytes);
        if (progress) progress("read pages", ++done, quint32(mapped.size()));
        QThread::msleep(requiredNumber(pageRule, "block_delay_ms"));
    }
    return image;
}

void writePagedCodeplug(const RadioDefinition &definition, const Bytes &image,
                        const RadioExecutor::Progress &progress)
{
    const Object root = definition.document();
    const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
    const Object pageRule = transport.value(QStringLiteral("page_map")).toObject();
    const Object assembly = root.value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("read_codeplug")).toObject()
        .value(QStringLiteral("assembly")).toObject();
    if (image.size() != requiredNumber(assembly, "bytes"))
        throw std::runtime_error("Serial codeplug image has the wrong size");
    auto pages = imagePages(image, pageRule);
    for (const QJsonValue &value : definition.writePlan().value(QStringLiteral("exclude_page_ids")).toArray()) {
        quint32 excluded = 0;
        if (!number(value, &excluded) || excluded > 255)
            throw std::runtime_error("Invalid excluded serial page ID");
        pages.erase(quint8(excluded));
    }
    Port port(transport, nullptr);
    pagedConnect(port, transport);
    const auto range = pagedMemoryRange(port, transport);
    pagedSteps(port, transport.value(QStringLiteral("enter_program_steps")).toArray());
    const auto mapped = pagedMap(port, transport, range, progress, "discover pages");
    const quint32 sentinel = requiredNumber(pageRule, "unallocated_write_address");
    quint32 done = 0;
    std::set<quint8> changed;
    for (const auto &[id, contents] : pages) {
        const auto existing = mapped.find(id);
        if (existing != mapped.end()
            && pagedRead(port, transport, existing->second, quint32(contents.size())) == contents) {
            if (progress) progress("write pages", ++done, quint32(pages.size()));
            continue;
        }
        pagedWrite(port, transport, existing == mapped.end() ? sentinel : existing->second, contents);
        changed.insert(id);
        if (progress) progress("write pages", ++done, quint32(pages.size()));
        QThread::msleep(requiredNumber(pageRule, "block_delay_ms"));
    }
    if (!changed.empty()) {
        const auto verified = pagedMap(port, transport, range, progress, "verify page map");
        done = 0;
        for (quint8 id : changed) {
            const auto address = verified.find(id);
            if (address == verified.end()
                || pagedRead(port, transport, address->second, quint32(pages.at(id).size())) != pages.at(id))
                throw std::runtime_error("Serial radio page verification failed");
            if (progress) progress("verify pages", ++done, quint32(changed.size()));
        }
    }
}

} // namespace

bool SerialExecutor::validate(const QJsonObject &root, QString *error)
{
    if (root.value(QStringLiteral("serial_transport")).toObject()
            .value(QStringLiteral("protocol")) == QStringLiteral("paged_addressed_serial_v1"))
        return validatePaged(root, error);
    try {
        const Object transport = root.value(QStringLiteral("serial_transport")).toObject();
        const Object methods = root.value(QStringLiteral("methods")).toObject();
        const Object read = methods.value(QStringLiteral("read_codeplug")).toObject();
        const Object write = methods.value(QStringLiteral("write_codeplug")).toObject();
        const Object assembly = read.value(QStringLiteral("assembly")).toObject();
        const quint32 block = requiredNumber(transport, "block_bytes");
        const quint32 sector = requiredNumber(transport, "sector_bytes");
        const quint32 bytes = requiredNumber(assembly, "bytes");
        serialUsbIds(transport);
        if (requiredNumber(transport, "baud_rate") < 1200 || requiredNumber(transport, "timeout_ms") < 100
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
    if (transport.value(QStringLiteral("protocol")) == QStringLiteral("paged_addressed_serial_v1"))
        return readPagedCodeplug(definition, progress, cancelled);
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
    if (transport.value(QStringLiteral("protocol")) == QStringLiteral("paged_addressed_serial_v1")) {
        writePagedCodeplug(definition, image, progress);
        return;
    }
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
