#include "radioexecutor.h"

#include <libusb.h>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Bytes = RadioExecutor::Bytes;
using Object = QJsonObject;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

uint32_t number(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double n = value.toDouble();
        require(n >= 0 && n <= 4294967295.0 && n == uint32_t(n), "Invalid radio definition integer");
        return uint32_t(n);
    }
    const QString s = value.toString();
    bool ok = false;
    const uint n = s.toUInt(&ok, s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
    require(ok, "Invalid radio definition integer");
    return n;
}

Bytes hex(const QJsonValue &value)
{
    const QString s = value.toString();
    if (s.isEmpty()) return {};
    const auto pairs = s.split(' ', Qt::SkipEmptyParts);
    Bytes out;
    for (const QString &pair : pairs) {
        bool ok = false;
        const uint n = pair.toUInt(&ok, 16);
        require(ok && pair.size() == 2 && n <= 255, "Invalid radio definition hex bytes");
        out.push_back(uint8_t(n));
    }
    return out;
}

uint32_t integer(const Bytes &data, size_t at, const QString &type)
{
    const size_t count = type == QStringLiteral("u8") ? 1 :
                         type == QStringLiteral("u16le") || type == QStringLiteral("u16be") ? 2 : 4;
    require(at <= data.size() && count <= data.size() - at, "Truncated radio field");
    uint32_t value = 0;
    if (type.endsWith(QStringLiteral("le")))
        for (size_t i = 0; i < count; ++i) value |= uint32_t(data[at + i]) << (8 * i);
    else
        for (size_t i = 0; i < count; ++i) value = (value << 8) | data[at + i];
    return value;
}

void put(Bytes &data, size_t at, uint32_t value, const QString &type)
{
    const size_t count = type == QStringLiteral("u8") ? 1 :
                         type == QStringLiteral("u16le") || type == QStringLiteral("u16be") ? 2 : 4;
    require(at <= data.size() && count <= data.size() - at, "Radio field exceeds buffer");
    for (size_t i = 0; i < count; ++i)
        data[at + i] = uint8_t(value >> (8 * (type.endsWith(QStringLiteral("le")) ? i : count - 1 - i)));
}

size_t frameOffset(const QJsonValue &value, size_t payloadBytes)
{
    if (!value.isObject()) return number(value);
    const QJsonArray terms = value.toObject().value(QStringLiteral("add")).toArray();
    require(!terms.isEmpty(), "Unsupported frame offset expression");
    size_t result = 0;
    for (const QJsonValue &term : terms) {
        if (term.isObject()) {
            require(term.toObject().value(QStringLiteral("length_of")) == QStringLiteral("payload"),
                    "Unsupported frame length parameter");
            result += payloadBytes;
        } else result += number(term);
    }
    return result;
}

Bytes frame(const Object &rules, uint8_t deviceClass, uint8_t command, const Bytes &payload)
{
    const QJsonArray fields = rules.value(QStringLiteral("fields")).toArray();
    const uint8_t start = uint8_t(number(fields.at(0).toObject().value(QStringLiteral("value"))));
    const uint8_t end = uint8_t(number(fields.at(6).toObject().value(QStringLiteral("value"))));
    const size_t header = frameOffset(fields.at(4).toObject().value(QStringLiteral("offset")), payload.size());
    const size_t checksumAt = frameOffset(fields.at(5).toObject().value(QStringLiteral("offset")), payload.size());
    const size_t endAt = frameOffset(fields.at(6).toObject().value(QStringLiteral("offset")), payload.size());
    const QString checksumType = fields.at(5).toObject().value(QStringLiteral("type")).toString();
    const size_t checksumWidth = checksumType == QStringLiteral("u8") ? 1 :
        checksumType == QStringLiteral("u16be") || checksumType == QStringLiteral("u16le") ? 2 : 4;
    require(header + payload.size() <= checksumAt && checksumAt + checksumWidth <= endAt,
            "Overlapping radio frame fields");
    Bytes result(endAt + 1);
    put(result, frameOffset(fields.at(0).toObject().value(QStringLiteral("offset")), payload.size()),
        start, fields.at(0).toObject().value(QStringLiteral("type")).toString());
    put(result, frameOffset(fields.at(1).toObject().value(QStringLiteral("offset")), payload.size()),
        deviceClass, fields.at(1).toObject().value(QStringLiteral("type")).toString());
    put(result, frameOffset(fields.at(2).toObject().value(QStringLiteral("offset")), payload.size()),
        command, fields.at(2).toObject().value(QStringLiteral("type")).toString());
    put(result, frameOffset(fields.at(3).toObject().value(QStringLiteral("offset")), payload.size()),
        uint32_t(payload.size()), fields.at(3).toObject().value(QStringLiteral("type")).toString());
    std::copy(payload.begin(), payload.end(), result.begin() + header);
    uint32_t checksum = 0;
    for (uint8_t byte : payload) checksum += byte;
    put(result, checksumAt, checksum % number(fields.at(5).toObject()
        .value(QStringLiteral("derived")).toObject().value(QStringLiteral("modulus"))),
        fields.at(5).toObject().value(QStringLiteral("type")).toString());
    put(result, endAt, end, fields.at(6).toObject().value(QStringLiteral("type")).toString());
    return result;
}

void checkFrame(const Object &rules, const Bytes &data)
{
    const QJsonArray fields = rules.value(QStringLiteral("fields")).toArray();
    const Object lengthField = fields.at(3).toObject();
    const size_t length = integer(data, frameOffset(lengthField.value(QStringLiteral("offset")), 0),
                                  lengthField.value(QStringLiteral("type")).toString());
    const Object startField = fields.at(0).toObject();
    const Object endField = fields.at(6).toObject();
    require(data.size() == frameOffset(endField.value(QStringLiteral("offset")), length) + 1,
            "Invalid radio frame length");
    require(integer(data, frameOffset(startField.value(QStringLiteral("offset")), length),
                    startField.value(QStringLiteral("type")).toString()) == number(startField.value(QStringLiteral("value")))
            && integer(data, frameOffset(endField.value(QStringLiteral("offset")), length),
                       endField.value(QStringLiteral("type")).toString()) == number(endField.value(QStringLiteral("value"))),
            "Invalid radio frame delimiters");
    const size_t payloadAt = frameOffset(fields.at(4).toObject().value(QStringLiteral("offset")), length);
    require(payloadAt + length <= data.size(), "Radio frame payload exceeds response");
    uint32_t sum = 0;
    for (size_t i = payloadAt; i < payloadAt + length; ++i) sum += data[i];
    const Object checksumField = fields.at(5).toObject();
    require(integer(data, frameOffset(checksumField.value(QStringLiteral("offset")), length),
                    checksumField.value(QStringLiteral("type")).toString()) ==
            sum % number(fields.at(5).toObject().value(QStringLiteral("derived")).toObject()
                         .value(QStringLiteral("modulus"))), "Invalid radio frame checksum");
}

class Usb {
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    bool detached = false, claimed = false;
    Object root, usb, transport;
    uint32_t tag;
    const std::atomic_bool *cancelled;
    int interfaceNumber;

    void cleanup() noexcept
    {
        if (handle) {
            if (claimed) libusb_release_interface(handle, interfaceNumber);
            if (detached) libusb_attach_kernel_driver(handle, interfaceNumber);
            libusb_close(handle);
        }
        if (context) libusb_exit(context);
    }

    void transfer(uint8_t endpoint, Bytes &data, bool exact)
    {
        int actual = 0;
        const int timeout = int(number(root.value(QStringLiteral("polling")).toObject()
                                       .value(QStringLiteral("bulk_transfer_timeout_ms"))));
        const int result = libusb_bulk_transfer(handle, endpoint, data.data(), int(data.size()), &actual, timeout);
        require(result == 0, libusb_error_name(result));
        require(!exact || size_t(actual) == data.size(), "Short radio USB transfer");
        data.resize(size_t(actual));
    }

public:
    Usb(const RadioDefinition &definition, const std::atomic_bool *cancel)
        : root(definition.document()), usb(root.value(QStringLiteral("usb")).toObject()),
          transport(root.value(QStringLiteral("bulk_only_transport")).toObject()),
          tag(number(transport.value(QStringLiteral("cbw")).toObject()
                         .value(QStringLiteral("tag_rule")).toObject().value(QStringLiteral("initial")))),
          cancelled(cancel), interfaceNumber(int(number(usb.value(QStringLiteral("interface_number")))))
    {
        try {
            int result = libusb_init(&context);
            require(result == 0, "Could not initialize USB");
            libusb_device **devices = nullptr;
            const ssize_t count = libusb_get_device_list(context, &devices);
            require(count >= 0, "Could not enumerate USB devices");
            libusb_device *match = nullptr;
            int matches = 0;
            for (ssize_t i = 0; i < count; ++i) {
                libusb_device_descriptor descriptor{};
                if (libusb_get_device_descriptor(devices[i], &descriptor) == 0
                    && descriptor.idVendor == number(usb.value(QStringLiteral("vendor_id")))
                    && descriptor.idProduct == number(usb.value(QStringLiteral("product_id")))) {
                    match = devices[i]; ++matches;
                }
            }
            if (matches == 1) result = libusb_open(match, &handle);
            libusb_free_device_list(devices, 1);
            require(matches == 1, matches ? "Multiple matching radios connected" : "No matching radio connected");
            require(result == 0, libusb_error_name(result));
            libusb_config_descriptor *config = nullptr;
            result = libusb_get_active_config_descriptor(libusb_get_device(handle), &config);
            require(result == 0, "Cannot inspect radio USB interface");
            bool compatible = false;
            for (uint8_t i = 0; i < config->bNumInterfaces; ++i)
                for (int a = 0; a < config->interface[i].num_altsetting; ++a) {
                    const auto &iface = config->interface[i].altsetting[a];
                    if (iface.bInterfaceNumber != interfaceNumber
                        || iface.bAlternateSetting != number(usb.value(QStringLiteral("alternate_setting")))
                        || iface.bInterfaceClass != number(usb.value(QStringLiteral("interface_class")))
                        || iface.bInterfaceSubClass != number(usb.value(QStringLiteral("interface_subclass")))
                        || iface.bInterfaceProtocol != number(usb.value(QStringLiteral("interface_protocol")))) continue;
                    bool in = false, out = false;
                    for (uint8_t j = 0; j < iface.bNumEndpoints; ++j) {
                        const auto &ep = iface.endpoint[j];
                        if ((ep.bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK
                            || ep.wMaxPacketSize != number(usb.value(QStringLiteral("maximum_packet_bytes")))) continue;
                        in |= ep.bEndpointAddress == number(usb.value(QStringLiteral("bulk_in_endpoint")));
                        out |= ep.bEndpointAddress == number(usb.value(QStringLiteral("bulk_out_endpoint")));
                    }
                    compatible |= in && out;
                }
            libusb_free_config_descriptor(config);
            require(compatible, "Radio USB interface does not match definition");
            result = libusb_kernel_driver_active(handle, interfaceNumber);
            require(result >= 0 || result == LIBUSB_ERROR_NOT_SUPPORTED,
                    "Could not inspect radio USB driver");
#ifdef __APPLE__
            int claimResult = libusb_claim_interface(handle, interfaceNumber);
            if (claimResult == 0) {
                claimed = true;
            } else
#endif
            if (result == 1) {
                result = libusb_detach_kernel_driver(handle, interfaceNumber);
                require(result == 0, "Could not detach radio USB driver");
                detached = true;
            }
            if (!claimed) {
                result = libusb_claim_interface(handle, interfaceNumber);
                require(result == 0, "Could not claim radio USB interface");
                claimed = true;
            }
            const int alternate = int(number(usb.value(QStringLiteral("alternate_setting"))));
            if (alternate != 0) {
                result = libusb_set_interface_alt_setting(handle, interfaceNumber, alternate);
                require(result == 0, "Could not select radio USB alternate setting");
            }
        } catch (...) { cleanup(); throw; }
    }
    ~Usb() { cleanup(); }
    void checkpoint() const
    {
        if (cancelled && cancelled->load()) throw RadioExecutor::Cancelled();
    }
    Bytes command(const QString &name, const Bytes &payload, uint32_t fetchBytes)
    {
        const Object commandRule = transport.value(QStringLiteral("commands")).toObject().value(name).toObject();
        const uint8_t direction = uint8_t(number(commandRule.value(QStringLiteral("direction"))));
        const Bytes cdb = hex(commandRule.value(QStringLiteral("cdb_hex")));
        const Object cbwRule = transport.value(QStringLiteral("cbw")).toObject();
        const Object cswRule = transport.value(QStringLiteral("csw")).toObject();
        const uint32_t length = direction ? fetchBytes : uint32_t(payload.size());
        Bytes cbw(size_t(number(cbwRule.value(QStringLiteral("bytes")))), 0);
        const QJsonArray fields = cbwRule.value(QStringLiteral("fields")).toArray();
        for (const QJsonValue &entry : fields) {
            const Object field = entry.toObject();
            const size_t offset = number(field.value(QStringLiteral("offset")));
            const QString parameter = field.value(QStringLiteral("parameter")).toString();
            if (field.contains(QStringLiteral("hex"))) {
                const Bytes constant = hex(field.value(QStringLiteral("hex")));
                require(offset + constant.size() <= cbw.size(), "CBW field exceeds wrapper");
                std::copy(constant.begin(), constant.end(), cbw.begin() + offset);
            } else if (parameter == QStringLiteral("cdb")) {
                require(offset + cdb.size() <= cbw.size(), "CBW CDB exceeds wrapper");
                std::copy(cdb.begin(), cdb.end(), cbw.begin() + offset);
            } else {
                const uint32_t value = parameter == QStringLiteral("tag") ? tag :
                                       parameter == QStringLiteral("data_bytes") ? length :
                                       parameter == QStringLiteral("direction") ? direction :
                                       number(field.value(QStringLiteral("value")));
                put(cbw, offset, value, field.value(QStringLiteral("type")).toString());
            }
        }
        const uint32_t currentTag = tag;
        tag += number(cbwRule.value(QStringLiteral("tag_rule")).toObject()
                      .value(QStringLiteral("increment")));
        transfer(uint8_t(number(usb.value(QStringLiteral("bulk_out_endpoint")))), cbw, true);
        Bytes data = direction ? Bytes(fetchBytes) : payload;
        transfer(uint8_t(number(usb.value(direction ? QStringLiteral("bulk_in_endpoint")
                                             : QStringLiteral("bulk_out_endpoint")))), data, !direction);
        Bytes csw(size_t(number(cswRule.value(QStringLiteral("bytes")))));
        transfer(uint8_t(number(usb.value(QStringLiteral("bulk_in_endpoint")))), csw, true);
        const Object signature = cswRule.value(QStringLiteral("signature")).toObject();
        const Bytes expected = hex(signature.value(QStringLiteral("hex")));
        const size_t offset = number(signature.value(QStringLiteral("offset")));
        require(offset + expected.size() <= csw.size()
                && std::equal(expected.begin(), expected.end(), csw.begin() + offset), "Radio CSW signature mismatch");
        require(integer(csw, number(cswRule.value(QStringLiteral("tag")).toObject().value(QStringLiteral("offset"))),
                        cswRule.value(QStringLiteral("tag")).toObject().value(QStringLiteral("type")).toString()) == currentTag,
                "Radio CSW tag mismatch");
        const uint32_t residue = integer(csw, number(cswRule.value(QStringLiteral("residue")).toObject()
                                                     .value(QStringLiteral("offset"))),
                                         cswRule.value(QStringLiteral("residue")).toObject()
                                             .value(QStringLiteral("type")).toString());
        const Object checks = cswRule.value(QStringLiteral("checks")).toObject();
        require(uint64_t(data.size()) + residue == length, "Radio CSW residue mismatch");
        require(integer(csw, number(cswRule.value(QStringLiteral("status")).toObject().value(QStringLiteral("offset"))),
                        cswRule.value(QStringLiteral("status")).toObject().value(QStringLiteral("type")).toString())
                    == number(checks.value(QStringLiteral("success_status")))
                && residue == number(checks.value(QStringLiteral("success_residue"))), "Radio USB transaction failed");
        return data;
    }
};

Bytes exchange(Usb &usb, const Object &root, uint8_t deviceClass, uint8_t command,
               const Bytes &payload, uint32_t fetchBytes, uint32_t pollLimit, bool expected,
               const Bytes &expectedBytes = {}, const Bytes &previous = {})
{
    usb.checkpoint();
    const Bytes request = frame(root.value(QStringLiteral("frame")).toObject(), deviceClass, command, payload);
    usb.command(QStringLiteral("send_frame"), request, 0);
    const Object polling = root.value(QStringLiteral("polling")).toObject();
    const Object frameRules = root.value(QStringLiteral("frame")).toObject();
    const QJsonArray frameFields = frameRules.value(QStringLiteral("fields")).toArray();
    const Object responseRules = frameRules
        .value(QStringLiteral("response_checks")).toObject();
    for (uint32_t i = 0; i < pollLimit; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(number(polling.value(QStringLiteral("delay_before_each_fetch_ms")))));
        Bytes response = usb.command(QStringLiteral("fetch_response"), {}, fetchBytes);
        const uint8_t empty = uint8_t(number(polling.value(QStringLiteral("empty_response")).toObject()
                                            .value(QStringLiteral("all_bytes_equal"))));
        if (std::all_of(response.begin(), response.end(), [empty](uint8_t b) { return b == empty; })) continue;
        if (!previous.empty() && response == previous) continue;
        require(expected, "Unexpected radio response");
        checkFrame(frameRules, response);
        const Object classField = frameFields.at(1).toObject();
        const Object commandField = frameFields.at(2).toObject();
        require(integer(response, frameOffset(classField.value(QStringLiteral("offset")), 0),
                        classField.value(QStringLiteral("type")).toString())
                    == uint8_t(deviceClass | number(responseRules.value(QStringLiteral("class_or_mask"))))
                    && integer(response, frameOffset(commandField.value(QStringLiteral("offset")), 0),
                               commandField.value(QStringLiteral("type")).toString()) == command,
                "Radio response class or command mismatch");
        const size_t successOffset = frameOffset(frameFields.at(4).toObject().value(QStringLiteral("offset")), 0)
            + number(responseRules.value(QStringLiteral("success_payload_offset")));
        require(response.size() >= successOffset + 1
                && response[successOffset] == number(responseRules.value(QStringLiteral("success_value"))),
                "Radio rejected command");
        if (!expectedBytes.empty()) require(response == expectedBytes, "Unexpected radio acknowledgement");
        return response;
    }
    require(!expected, "Radio response timed out");
    return {};
}

void identify(Usb &usb, const Object &root, bool setup)
{
    const Object method = root.value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("identify")).toObject();
    for (const QJsonValue &entry : method.value(QStringLiteral("sequence")).toArray()) {
        const Object probe = entry.toObject();
        const bool model = probe.value(QStringLiteral("expected")) == QStringLiteral("model_response");
        for (uint32_t repeat = 0; repeat < number(probe.value(QStringLiteral("repeat"))); ++repeat) {
            const Bytes response = exchange(usb, root, uint8_t(number(probe.value(QStringLiteral("class")))),
                uint8_t(number(probe.value(QStringLiteral("command")))), hex(probe.value(QStringLiteral("payload_hex"))),
                number(probe.value(QStringLiteral("fetch_bytes"))),
                number(probe.value(QStringLiteral("poll_limit_per_repeat"))), model);
            if (model) {
                const Object identity = method.value(QStringLiteral("model_response")).toObject();
                const Bytes expected = hex(identity.value(QStringLiteral("expected_hex")));
                const size_t at = number(identity.value(QStringLiteral("frame_offset")));
                require(at + expected.size() <= response.size()
                        && std::equal(expected.begin(), expected.end(), response.begin() + at),
                        "Connected radio model does not match definition");
            }
        }
    }
    if (setup) {
        const Object operation = method.value(QStringLiteral("setup")).toObject();
        exchange(usb, root, uint8_t(number(operation.value(QStringLiteral("class")))),
                 uint8_t(number(operation.value(QStringLiteral("command")))),
                 hex(operation.value(QStringLiteral("payload_hex"))),
                 number(operation.value(QStringLiteral("fetch_bytes"))),
                 number(operation.value(QStringLiteral("poll_limit"))), true,
                 hex(operation.value(QStringLiteral("expected_response_hex"))));
    }
}

class ImageFields {
    const Object definition;
    const Bytes &image;
    std::set<QString> active;
public:
    ImageFields(const Object &document, const Bytes &bytes) : definition(document), image(bytes) {}
    uint32_t expression(const QJsonValue &value)
    {
        if (!value.isObject()) return number(value);
        const Object expression = value.toObject();
        if (expression.contains(QStringLiteral("field")))
            return field(expression.value(QStringLiteral("field")).toString());
        const QJsonArray add = expression.value(QStringLiteral("add")).toArray();
        require(!add.isEmpty(), "Unsupported codeplug field expression");
        uint64_t result = 0;
        for (const QJsonValue &term : add) result += expressionValue(term);
        require(result <= UINT32_MAX, "Codeplug field expression overflow");
        return uint32_t(result);
    }
    uint32_t expressionValue(const QJsonValue &value) { return expression(value); }
    uint32_t field(const QString &name)
    {
        require(!active.count(name), "Recursive codeplug field");
        const Object definitionField = definition.value(QStringLiteral("fields")).toObject().value(name).toObject();
        require(!definitionField.isEmpty(), "Missing codeplug field");
        active.insert(name);
        uint32_t result = 0;
        if (definitionField.contains(QStringLiteral("value"))) result = expression(definitionField.value(QStringLiteral("value")));
        else result = integer(image, expression(definitionField.value(QStringLiteral("at"))),
                              definitionField.value(QStringLiteral("type")).toString());
        active.erase(name);
        return result;
    }
};

std::vector<RadioExecutor::WriteBlock> buildWriteBlocks(const Object &root, const Object &wcp, const Bytes &image)
{
    const Object write = root.value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("write_codeplug")).toObject();
    require(write.value(QStringLiteral("codeplug_definition_id")) == wcp.value(QStringLiteral("id")),
            "Radio and codeplug definitions do not match");
    ImageFields fields(wcp, image);
    const uint32_t blockBytes = number(write.value(QStringLiteral("block_bytes")));
    const Object request = write.value(QStringLiteral("request")).toObject();
    const Object regions = write.value(QStringLiteral("regions")).toObject();
    std::vector<RadioExecutor::WriteBlock> result;
    for (const QJsonValue &operationValue : write.value(QStringLiteral("operations")).toArray()) {
        const Object operation = operationValue.toObject();
        if (operation.value(QStringLiteral("operation")) != QStringLiteral("write_region")) continue;
        const QString regionName = operation.value(QStringLiteral("region")).toString();
        const Object region = regions.value(regionName).toObject();
        const uint32_t start = fields.expression(region.value(QStringLiteral("start")));
        const uint32_t size = fields.expression(region.value(QStringLiteral("size")));
        require(size > 0 && uint64_t(start) + size <= image.size(), "Write region outside decoded image");
        const uint32_t count = (size + blockBytes - 1) / blockBytes;
        std::set<uint32_t> selected;
        uint32_t extent = size;
        if (region.value(QStringLiteral("selection")) == QStringLiteral("all_blocks")) {
            for (uint32_t index = 0; index < count; ++index) selected.insert(index);
        } else {
            const Object choice = region.value(QStringLiteral("selection")).toObject();
            require(choice.value(QStringLiteral("operation")) == QStringLiteral("table_blocks"),
                    "Unsupported write block selection");
            uint64_t end = number(choice.value(QStringLiteral("header_bytes")));
            for (const QJsonValue &nameValue : choice.value(QStringLiteral("tables")).toArray()) {
                const Object table = wcp.value(QStringLiteral("tables")).toObject()
                    .value(nameValue.toString()).toObject();
                const uint32_t at = fields.expression(table.value(QStringLiteral("at")));
                const uint32_t rows = fields.expression(table.value(QStringLiteral("count")));
                const uint32_t stride = number(table.value(QStringLiteral("stride_bytes")));
                const uint32_t span = table.contains(QStringLiteral("logical_span_bytes"))
                    ? number(table.value(QStringLiteral("logical_span_bytes"))) : stride;
                if (!rows) continue;
                const uint64_t tableEnd = uint64_t(at) + uint64_t(rows - 1) * stride + span;
                require(at >= start && tableEnd <= uint64_t(start) + size, "Write table exceeds region");
                end = std::max(end, tableEnd - start);
                for (uint64_t block = (at - start) / blockBytes;
                     block <= (tableEnd - 1 - start) / blockBytes; ++block) selected.insert(uint32_t(block));
            }
            for (uint64_t index = 0; index < (number(choice.value(QStringLiteral("header_bytes"))) + blockBytes - 1) / blockBytes; ++index)
                selected.insert(uint32_t(index));
            require(end <= UINT32_MAX, "Write extent overflow");
            extent = uint32_t(end);
        }
        require(!selected.empty(), "No blocks selected for write region");
        const uint32_t finalIndex = *selected.rbegin();
        for (uint32_t index : selected) {
            require(index < count && index <= UINT16_MAX, "Write block index exceeds region");
            const size_t at = size_t(start) + size_t(index) * blockBytes;
            require(at + blockBytes <= image.size(), "Incomplete source write block");
            uint32_t selector = index == 0 ? number(request.value(QStringLiteral("first_selector")))
                : index | number(request.value(QStringLiteral("other_selector_or")));
            const Object final = request.value(QStringLiteral("final_region_selector_or")).toObject();
            if (regionName == final.value(QStringLiteral("region")).toString() && index == finalIndex)
                selector = index | number(final.value(QStringLiteral("value")));
            Bytes payload;
            for (const QJsonValue &fieldValue : request.value(QStringLiteral("payload")).toArray()) {
                const Object field = fieldValue.toObject();
                if (field.contains(QStringLiteral("hex"))) {
                    const Bytes bytes = hex(field.value(QStringLiteral("hex")));
                    payload.insert(payload.end(), bytes.begin(), bytes.end());
                } else if (field.value(QStringLiteral("parameter")) == QStringLiteral("image_block")) {
                    payload.insert(payload.end(), image.begin() + at, image.begin() + at + blockBytes);
                } else {
                    const QString parameter = field.value(QStringLiteral("parameter")).toString();
                    require(parameter == QStringLiteral("selector") || parameter == QStringLiteral("extent")
                            || field.contains(QStringLiteral("value")),
                            "Unsupported write payload parameter");
                    const uint32_t value = parameter == QStringLiteral("selector") ? selector :
                        parameter == QStringLiteral("extent") ? extent : number(field.value(QStringLiteral("value")));
                    const QString type = field.value(QStringLiteral("type")).toString();
                    const size_t width = type == QStringLiteral("u8") ? 1 :
                        type == QStringLiteral("u16be") || type == QStringLiteral("u16le") ? 2 : 4;
                    const size_t offset = payload.size();
                    payload.resize(offset + width);
                    put(payload, offset, value, type);
                }
            }
            const QString frameLengthType = root.value(QStringLiteral("frame")).toObject()
                .value(QStringLiteral("fields")).toArray().at(3).toObject()
                .value(QStringLiteral("type")).toString();
            require(payload.size() <= (frameLengthType.startsWith(QStringLiteral("u16")) ? 65535u : UINT32_MAX),
                    "Write payload exceeds radio frame length field");
            result.push_back({regionName, uint8_t(number(region.value(QStringLiteral("command")))),
                              uint16_t(selector), extent, index, std::move(payload)});
        }
    }
    return result;
}

uint16_t selectReadBlock(const Object &block, const QString &region, uint32_t index, uint32_t count)
{
    require(count > 0 && index < count, "Invalid radio read block index");
    const Object selector = block.value(QStringLiteral("selector")).toObject();
    require(index <= number(selector.value(QStringLiteral("maximum_block_index"))),
            "Radio region exceeds selector capacity");
    for (const QJsonValue &entry : selector.value(QStringLiteral("rules_in_priority_order")).toArray()) {
        const Object rule = entry.toObject();
        const Object condition = rule.value(QStringLiteral("when")).toObject();
        if (condition.contains(QStringLiteral("block_index"))
            && index != number(condition.value(QStringLiteral("block_index")))) continue;
        if (condition.contains(QStringLiteral("region"))
            && region != condition.value(QStringLiteral("region")).toString()) continue;
        if (condition.contains(QStringLiteral("last_block")) && index != count - 1) continue;
        return uint16_t(rule.contains(QStringLiteral("value"))
            ? number(rule.value(QStringLiteral("value")))
            : index | number(rule.value(QStringLiteral("or_with_block_index"))));
    }
    throw std::runtime_error("No selector rule matched radio read block");
}
}

std::vector<RadioExecutor::WriteBlock> RadioExecutor::planWrite(
    const RadioDefinition &definition, const QJsonObject &codeplugDefinition, const RadioExecutor::Bytes &image)
{
    require(definition.canWrite(), "Radio definition has no write operation");
    return buildWriteBlocks(definition.document(), codeplugDefinition, image);
}

RadioExecutor::Bytes RadioExecutor::makeFrame(const RadioDefinition &definition,
                                             uint8_t deviceClass, uint8_t command,
                                             const RadioExecutor::Bytes &payload)
{
    return frame(definition.document().value(QStringLiteral("frame")).toObject(),
                 deviceClass, command, payload);
}

uint16_t RadioExecutor::readSelector(const RadioDefinition &definition, const QString &region,
                                    uint32_t index, uint32_t blockCount)
{
    require(definition.canRead(), "Radio definition has no read operation");
    const Object block = definition.document().value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("read_block")).toObject();
    require(block.value(QStringLiteral("region_commands")).toObject().contains(region),
            "Unknown radio read region");
    return selectReadBlock(block, region, index, blockCount);
}

RadioExecutor::Bytes RadioExecutor::read(const RadioDefinition &definition,
                                     const RadioExecutor::Progress &progress,
                                     const std::atomic_bool &cancelled)
{
    require(definition.canRead(), "Radio definition has no read operation");
    if (cancelled.load()) throw RadioExecutor::Cancelled();
    const Object root = definition.document();
    const Object methods = root.value(QStringLiteral("methods")).toObject();
    const Object block = methods.value(QStringLiteral("read_block")).toObject();
    const Object responseRule = block.value(QStringLiteral("response")).toObject();
    const Object fields = responseRule.value(QStringLiteral("fields")).toObject();
    const Object checks = responseRule.value(QStringLiteral("checks")).toObject();
    const Object selector = block.value(QStringLiteral("selector")).toObject();
    const uint32_t blockBytes = number(block.value(QStringLiteral("block_bytes")));
    const uint32_t maxSize = number(checks.value(QStringLiteral("region_size_maximum")));
    const uint32_t mask = number(selector.value(QStringLiteral("index_mask")));
    Usb usb(definition, &cancelled);
    Bytes image;
    for (const RadioReadOperation &operation : definition.readOperations()) {
        if (operation.kind == RadioReadOperation::Kind::Identify) {
            identify(usb, root, true);
            continue;
        }
        const size_t base = image.size();
        uint32_t regionSize = 0;
        uint32_t count = 1;
        for (uint32_t index = 0; index < count; ++index) {
            usb.checkpoint();
            const uint32_t selected = selectReadBlock(block, operation.region, index, count);
            const QJsonArray requestFields = block.value(QStringLiteral("request_payload")).toArray();
            size_t payloadSize = 0;
            for (const QJsonValue &entry : requestFields) {
                const Object field = entry.toObject();
                const QString type = field.value(QStringLiteral("type")).toString();
                const size_t width = type == QStringLiteral("u8") ? 1 :
                    type == QStringLiteral("u16be") || type == QStringLiteral("u16le") ? 2 : 4;
                payloadSize = std::max(payloadSize,
                    size_t(number(field.value(QStringLiteral("payload_offset")))) + width);
            }
            Bytes payload(payloadSize);
            for (const QJsonValue &entry : requestFields) {
                const Object field = entry.toObject();
                put(payload, number(field.value(QStringLiteral("payload_offset"))),
                    field.contains(QStringLiteral("parameter")) ? selected : number(field.value(QStringLiteral("value"))),
                    field.value(QStringLiteral("type")).toString());
            }
            const Bytes response = exchange(usb, root, uint8_t(number(block.value(QStringLiteral("request_class")))),
                operation.command, payload, number(block.value(QStringLiteral("fetch_bytes"))),
                number(block.value(QStringLiteral("poll_limit"))), true);
            const Object lengthField = root.value(QStringLiteral("frame")).toObject()
                .value(QStringLiteral("fields")).toArray().at(3).toObject();
            require(response.size() == number(block.value(QStringLiteral("fetch_bytes")))
                    && integer(response, frameOffset(lengthField.value(QStringLiteral("offset")), 0),
                               lengthField.value(QStringLiteral("type")).toString())
                        == number(responseRule.value(QStringLiteral("payload_bytes"))),
                    "Unexpected radio block response size");
            const Object indexField = fields.value(QStringLiteral("block_index")).toObject();
            const Object sizeField = fields.value(QStringLiteral("region_size")).toObject();
            const Object successField = fields.value(QStringLiteral("success")).toObject();
            require(integer(response, number(successField.value(QStringLiteral("frame_offset"))),
                            successField.value(QStringLiteral("type")).toString())
                        == number(successField.value(QStringLiteral("expected"))),
                    "Radio block response indicates failure");
            const uint32_t returnedIndex = integer(response, number(indexField.value(QStringLiteral("frame_offset"))),
                                                   indexField.value(QStringLiteral("type")).toString());
            const uint32_t returnedSize = integer(response, number(sizeField.value(QStringLiteral("frame_offset"))),
                                                  sizeField.value(QStringLiteral("type")).toString());
            require(returnedIndex == (selected & mask), "Radio returned wrong block index");
            require(returnedSize >= number(checks.value(QStringLiteral("region_size_minimum")))
                    && returnedSize <= maxSize && uint64_t(index) * blockBytes < returnedSize,
                    "Radio returned invalid region size");
            if (index == 0) {
                regionSize = returnedSize;
                count = (regionSize + blockBytes - 1) / blockBytes;
                image.resize(base + regionSize);
            } else require(returnedSize == regionSize, "Radio region size changed during read");
            const Object data = fields.value(QStringLiteral("data")).toObject();
            const size_t at = number(data.value(QStringLiteral("frame_offset")));
            const size_t valid = std::min<size_t>(blockBytes, regionSize - uint64_t(index) * blockBytes);
            require(at + valid <= response.size(), "Radio block data is truncated");
            if (index == 0 && operation.checkFirstBlockSize) {
                const uint32_t declared = integer(response, at + operation.sizeCheckOffset, operation.sizeCheckType);
                require(declared == regionSize, "Radio region header size mismatch");
            }
            std::copy_n(response.begin() + at, valid, image.begin() + base + size_t(index) * blockBytes);
            if (progress) progress(operation.region.toStdString(), index + 1, count);
        }
    }
    usb.checkpoint();
    return image;
}

void RadioExecutor::write(const RadioDefinition &definition, const QJsonObject &codeplugDefinition,
                          const RadioExecutor::Bytes &image, const RadioExecutor::Progress &progress)
{
    require(definition.canWrite(), "Radio definition has no write operation");
    const Object root = definition.document();
    const Object write = definition.writePlan();
    // Build and bounds-check every block before opening the radio.
    const auto blocks = RadioExecutor::planWrite(definition, codeplugDefinition, image);
    require(!blocks.empty(), "Radio write plan selected no blocks");
    Usb usb(definition, nullptr);
    const QJsonArray operations = write.value(QStringLiteral("operations")).toArray();
    Bytes previousAck;
    std::map<QString, uint32_t> totals, completed;
    for (const auto &block : blocks) ++totals[block.region];
    for (const QJsonValue &value : operations) {
        const Object operation = value.toObject();
        const QString kind = operation.value(QStringLiteral("operation")).toString();
        if (kind == QStringLiteral("validate_codeplug")) continue;
        if (kind == QStringLiteral("identify")) {
            identify(usb, root, operation.value(QStringLiteral("include_read_setup")).toBool());
        } else if (kind == QStringLiteral("write_setup")) {
            exchange(usb, root, uint8_t(number(operation.value(QStringLiteral("class")))),
                uint8_t(number(operation.value(QStringLiteral("command")))), {},
                number(operation.value(QStringLiteral("fetch_bytes"))),
                number(operation.value(QStringLiteral("poll_limit"))), true,
                hex(operation.value(QStringLiteral("expected_response_hex"))));
        } else if (kind == QStringLiteral("write_region")) {
            const QString region = operation.value(QStringLiteral("region")).toString();
            const Object ack = write.value(QStringLiteral("acknowledgement")).toObject();
            for (const auto &block : blocks) {
                if (block.region != region) continue;
                try {
                    const Bytes response = exchange(usb, root, uint8_t(number(write.value(QStringLiteral("request"))
                        .toObject().value(QStringLiteral("class")))), block.command, block.payload,
                        number(ack.value(QStringLiteral("fetch_bytes"))),
                        number(ack.value(QStringLiteral("poll_limit"))), true, {}, previousAck);
                    const Object lengthField = root.value(QStringLiteral("frame")).toObject()
                        .value(QStringLiteral("fields")).toArray().at(3).toObject();
                    require(integer(response, frameOffset(lengthField.value(QStringLiteral("offset")), 0),
                                    lengthField.value(QStringLiteral("type")).toString())
                                == number(ack.value(QStringLiteral("payload_bytes"))),
                            "Radio write acknowledgement payload size mismatch");
                    const uint32_t returned = integer(response, number(ack.value(QStringLiteral("index_offset"))),
                                                      ack.value(QStringLiteral("index_type")).toString());
                    require(returned == (block.selector & number(ack.value(QStringLiteral("index_mask")))),
                            "Radio write acknowledgement block index mismatch");
                    previousAck = response;
                } catch (const std::exception &error) {
                    throw std::runtime_error("Write " + region.toStdString() + " block "
                        + std::to_string(block.index) + ": " + error.what()
                        + ". The radio may contain a partial codeplug; retry the complete write.");
                }
                if (progress) progress(region.toStdString(), ++completed[region], totals[region]);
            }
        } else throw std::runtime_error("Unknown radio write operation");
    }
}
