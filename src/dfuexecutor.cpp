#include "dfuexecutor.h"

#include <libusb.h>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
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

class UsbTransferError : public std::runtime_error {
public:
    explicit UsbTransferError(int result) : std::runtime_error(libusb_error_name(result)), code(result) {}
    const int code;
};

uint32_t number(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double n = value.toDouble();
        require(n >= 0 && n <= 4294967295.0 && n == uint32_t(n), "Invalid DFU definition integer");
        return uint32_t(n);
    }
    const QString text = value.toString();
    bool ok = false;
    const uint32_t result = text.toUInt(&ok, text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
    require(ok, "Invalid DFU definition integer");
    return result;
}

Bytes hex(const QJsonValue &value)
{
    Bytes result;
    for (const QString &pair : value.toString().split(' ', Qt::SkipEmptyParts)) {
        bool ok = false;
        const uint byte = pair.toUInt(&ok, 16);
        require(ok && pair.size() == 2 && byte <= 255, "Invalid DFU command bytes");
        result.push_back(uint8_t(byte));
    }
    return result;
}

const Object transport(const RadioDefinition &definition)
{
    return definition.document().value(QStringLiteral("dfu_control_transport")).toObject();
}

class Usb {
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    bool claimed = false;
    bool detached = false;
    const Object rules;
    const Object requests;
    const std::atomic_bool *cancelled;
    const int interfaceNumber;
    const unsigned timeout;

    void cleanup()
    {
        if (handle) {
            if (claimed) libusb_release_interface(handle, interfaceNumber);
            if (detached) libusb_attach_kernel_driver(handle, interfaceNumber);
            libusb_close(handle);
        }
        if (context) libusb_exit(context);
    }

public:
    Usb(const RadioDefinition &definition, const std::atomic_bool *cancel)
        : rules(transport(definition)), requests(rules.value(QStringLiteral("requests")).toObject()),
          cancelled(cancel),
          interfaceNumber(int(number(definition.document().value(QStringLiteral("usb")).toObject()
                                      .value(QStringLiteral("interface_number"))))),
          timeout(number(rules.value(QStringLiteral("transfer_timeout_ms"))))
    {
        const Object usb = definition.document().value(QStringLiteral("usb")).toObject();
        try {
            int rc = libusb_init(&context);
            require(rc == 0, "Could not initialize DFU USB");
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
                    match = devices[i];
                    ++matches;
                }
            }
            if (matches == 1) rc = libusb_open(match, &handle);
            libusb_free_device_list(devices, 1);
            require(matches == 1, matches ? "Multiple matching DFU devices connected" : "No matching DFU radio connected");
            require(rc == 0, libusb_error_name(rc));
            rc = libusb_kernel_driver_active(handle, interfaceNumber);
            require(rc >= 0 || rc == LIBUSB_ERROR_NOT_SUPPORTED, "Could not inspect DFU interface driver");
#ifdef __APPLE__
            const int claimResult = libusb_claim_interface(handle, interfaceNumber);
            if (claimResult == 0) claimed = true;
            else
#endif
            if (rc == 1) {
                rc = libusb_detach_kernel_driver(handle, interfaceNumber);
                require(rc == 0, "Could not detach DFU interface driver");
                detached = true;
            }
            if (!claimed) {
                rc = libusb_claim_interface(handle, interfaceNumber);
                require(rc == 0, "Could not claim DFU interface");
                claimed = true;
            }
            const int alternate = int(number(usb.value(QStringLiteral("alternate_setting"))));
            if (alternate) {
                rc = libusb_set_interface_alt_setting(handle, interfaceNumber, alternate);
                require(rc == 0, "Could not select DFU alternate setting");
            }
        } catch (...) { cleanup(); throw; }
    }

    ~Usb() { cleanup(); }

    void checkpoint() const
    {
        if (cancelled && cancelled->load()) throw RadioExecutor::Cancelled();
    }

    Bytes transfer(const QString &requestName, uint16_t value, Bytes data, bool input)
    {
        checkpoint();
        require(data.size() <= 65535, "DFU transfer exceeds USB control length");
        const int rc = libusb_control_transfer(handle,
            uint8_t(number(rules.value(input ? QStringLiteral("in_request_type")
                                             : QStringLiteral("out_request_type")))),
            uint8_t(number(requests.value(requestName))), value,
            uint16_t(number(rules.value(QStringLiteral("interface_index")))),
            data.empty() ? nullptr : data.data(), uint16_t(data.size()), timeout);
        if (rc < 0) throw UsbTransferError(rc);
        require(size_t(rc) == data.size(), "Short DFU control transfer");
        return data;
    }

    uint8_t state()
    {
        return transfer(QStringLiteral("get_state"), 0, Bytes(1), true)[0];
    }

    struct Status {
        uint8_t result;
        uint8_t state;
        uint32_t pollMilliseconds;
    };

    Status readStatus()
    {
        const Object rule = rules.value(QStringLiteral("get_status")).toObject();
        const Bytes response = transfer(QStringLiteral("get_status"),
            uint16_t(number(rule.value(QStringLiteral("value")))),
            Bytes(number(rule.value(QStringLiteral("response_bytes")))), true);
        const QJsonArray offsets = rule.value(QStringLiteral("poll_timeout_offsets")).toArray();
        require(offsets.size() == 3, "Invalid DFU poll timeout offsets");
        uint32_t poll = 0;
        for (int i = 0; i < 3; ++i) {
            const uint32_t byte = response[number(offsets.at(i))];
            const int shift = rule.value(QStringLiteral("poll_timeout_type")) == QStringLiteral("u24le")
                ? 8 * i : 8 * (2 - i);
            poll |= byte << shift;
        }
        return {response[number(rule.value(QStringLiteral("status_offset")))],
                response[number(rule.value(QStringLiteral("state_offset")))], poll};
    }

    uint8_t status()
    {
        const Status current = readStatus();
        require(current.result == number(rules.value(QStringLiteral("get_status")).toObject()
                                              .value(QStringLiteral("success_status"))),
                "DFU radio reported an error status");
        return current.state;
    }

    void complete(const Object &rule)
    {
        Status last{0xff, 0xff, 0};
        bool hasStatus = false;
        for (const QJsonValue &step : rule.value(QStringLiteral("after_download")).toArray()) {
            const QString instruction = step.toString();
            if (instruction == QStringLiteral("get_status")) {
                last = readStatus();
                hasStatus = true;
                if (rule.value(QStringLiteral("fail_on_nonzero_status")) == QJsonValue(true))
                    require(last.result == number(rules.value(QStringLiteral("get_status")).toObject()
                                                  .value(QStringLiteral("success_status"))),
                            "DFU radio reported an error status");
            } else if (instruction == QStringLiteral("clear_status")) {
                transfer(QStringLiteral("clear_status"), 0, {}, false);
            } else if (instruction == QStringLiteral("wait_poll_timeout")) {
                require(hasStatus && last.pollMilliseconds <= 60000, "Invalid DFU poll timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(last.pollMilliseconds));
            } else if (instruction.startsWith(QStringLiteral("delay_ms:"))) {
                bool ok = false;
                const uint ms = instruction.mid(9).toUInt(&ok);
                require(ok && ms <= 60000, "Invalid DFU command delay");
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            } else throw std::runtime_error("Unsupported DFU completion step");
        }
        const Object statusRule = rules.value(QStringLiteral("get_status")).toObject();
        const Object states = statusRule.value(QStringLiteral("states")).toObject();
        const QString target = rule.value(QStringLiteral("require_state")).toString();
        require(hasStatus && last.result == number(statusRule.value(QStringLiteral("success_status")))
                    && last.state == number(states.value(target)),
                "DFU command did not reach required state");
        if (rule.value(QStringLiteral("then")) == QStringLiteral("enter_dfu_idle")) enterIdle();
    }

    void enterIdle()
    {
        const Object rule = rules.value(QStringLiteral("enter_dfu_idle")).toObject();
        const Object actions = rule.value(QStringLiteral("state_actions")).toObject();
        for (uint32_t attempt = 0; attempt < number(rule.value(QStringLiteral("maximum_attempts"))); ++attempt) {
            checkpoint();
            const QString action = actions.value(QString::number(state())).toString();
            if (action == QStringLiteral("ready")) return;
            if (action == QStringLiteral("wait"))
                std::this_thread::sleep_for(std::chrono::milliseconds(number(rule.value(QStringLiteral("wait_ms")))));
            else if (action == QStringLiteral("abort") || action == QStringLiteral("clear_status")
                     || action == QStringLiteral("detach")) transfer(action, 0, {}, false);
            else throw std::runtime_error("Unsupported DFU state action");
        }
        throw std::runtime_error("DFU radio did not enter idle state");
    }

    void command(const QString &name, uint32_t address = 0, bool writing = false)
    {
        const Object rule = rules.value(QStringLiteral("commands")).toObject().value(name).toObject();
        require(!rule.isEmpty(), "Unknown DFU command");
        Bytes data;
        if (rule.contains(QStringLiteral("data_hex"))) data = hex(rule.value(QStringLiteral("data_hex")));
        else for (const QJsonValue &entry : rule.value(QStringLiteral("data")).toArray()) {
            const Object field = entry.toObject();
            if (field.contains(QStringLiteral("hex"))) {
                const Bytes bytes = hex(field.value(QStringLiteral("hex")));
                data.insert(data.end(), bytes.begin(), bytes.end());
            } else if (field.value(QStringLiteral("parameter")) == QStringLiteral("address")
                       && field.value(QStringLiteral("type")) == QStringLiteral("u32le")) {
                for (unsigned i = 0; i < 4; ++i) data.push_back(uint8_t(address >> (8 * i)));
            } else throw std::runtime_error("Unsupported DFU command field");
        }
        transfer(rule.value(QStringLiteral("request")).toString(),
                 uint16_t(number(rule.value(QStringLiteral("value")))), std::move(data), false);
        const QString completionName = writing && rules.contains(QStringLiteral("write_command_completion"))
            ? QStringLiteral("write_command_completion") : QStringLiteral("command_completion");
        complete(rules.value(completionName).toObject());
    }

    void exitProgramMode(const Object &method, const Object &step)
    {
        enterIdle();
        const QString name = step.contains(QStringLiteral("command"))
            ? step.value(QStringLiteral("command")).toString()
            : method.value(QStringLiteral("command")).toString();
        const Object command = rules.value(QStringLiteral("commands")).toObject()
            .value(name).toObject();
        require(!command.isEmpty(), "Unknown DFU exit command");
        if (step.value(QStringLiteral("status_after")) == QStringLiteral("allow_disconnect")) {
            try {
                transfer(command.value(QStringLiteral("request")).toString(),
                         uint16_t(number(command.value(QStringLiteral("value")))),
                         hex(command.value(QStringLiteral("data_hex"))), false);
                const Status response = readStatus();
                require(response.result == number(rules.value(QStringLiteral("get_status")).toObject()
                                                  .value(QStringLiteral("success_status"))),
                        "DFU exit reported an error status");
            } catch (const UsbTransferError &error) {
                if (error.code != LIBUSB_ERROR_NO_DEVICE && error.code != LIBUSB_ERROR_IO) throw;
            }
        } else {
            transfer(command.value(QStringLiteral("request")).toString(),
                     uint16_t(number(command.value(QStringLiteral("value")))),
                     hex(command.value(QStringLiteral("data_hex"))), false);
        }
    }

    void waitDownloadIdle()
    {
        const Object completion = rules.value(QStringLiteral("block_completion")).toObject();
        if (completion.contains(QStringLiteral("after_download"))) {
            complete(completion);
            return;
        }
        const QString target = completion.value(QStringLiteral("poll_state")).toString();
        const uint8_t expected = uint8_t(number(rules.value(QStringLiteral("get_status")).toObject()
                                             .value(QStringLiteral("states")).toObject().value(target)));
        for (uint32_t attempt = 0; attempt < number(completion.value(QStringLiteral("maximum_attempts"))); ++attempt) {
            checkpoint();
            const uint8_t actual = status();
            if (actual == expected) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(number(completion.value(QStringLiteral("delay_ms")))));
        }
        throw std::runtime_error("DFU write block did not reach download idle");
    }
};

void runCommand(Usb &usb, const Object &step, bool writing)
{
    const QString name = step.value(QStringLiteral("name")).toString();
    const uint32_t repeat = step.contains(QStringLiteral("repeat"))
        ? number(step.value(QStringLiteral("repeat"))) : 1;
    require(repeat >= 1 && repeat <= 10, "Invalid DFU command repeat");
    for (uint32_t i = 0; i < repeat; ++i)
        usb.command(name, step.contains(QStringLiteral("address"))
                          ? number(step.value(QStringLiteral("address"))) : 0, writing);
}
}

RadioExecutor::Bytes DfuExecutor::writePayload(const RadioDefinition &definition, const Bytes &image)
{
    const Object source = definition.writePlan().value(QStringLiteral("source")).toObject();
    const size_t imageBytes = number(source.value(QStringLiteral("image_bytes")));
    const size_t offset = number(source.value(QStringLiteral("payload_offset")));
    const size_t length = number(source.value(QStringLiteral("payload_bytes")));
    const size_t suffix = number(source.value(QStringLiteral("suffix_bytes")));
    const QByteArray magic = source.value(QStringLiteral("magic_ascii")).toString().toLatin1();
    require(image.size() == imageBytes && offset <= image.size()
                && length <= image.size() - offset && suffix == image.size() - offset - length,
            "DFU write image has the wrong size");
    require(image.size() >= size_t(magic.size())
                && std::equal(magic.begin(), magic.end(), image.begin()),
            "DFU write image has the wrong file signature");
    return Bytes(image.begin() + offset, image.begin() + offset + length);
}

RadioExecutor::Bytes DfuExecutor::read(const RadioDefinition &definition,
                                      const RadioExecutor::Progress &progress,
                                      const std::atomic_bool &cancelled)
{
    const Object plan = definition.document().value(QStringLiteral("methods")).toObject()
        .value(QStringLiteral("read_codeplug")).toObject();
    const size_t expected = number(plan.value(QStringLiteral("assembly")).toObject()
                                       .value(QStringLiteral("bytes")));
    Usb usb(definition, &cancelled);
    Bytes image;
    for (const QJsonValue &entry : plan.value(QStringLiteral("operations")).toArray()) {
        const Object step = entry.toObject();
        const QString operation = step.value(QStringLiteral("operation")).toString();
        if (operation == QStringLiteral("enter_dfu_idle")) usb.enterIdle();
        else if (operation == QStringLiteral("command")) runCommand(usb, step, false);
        else if (operation == QStringLiteral("read_blocks")) {
            const uint32_t first = number(step.value(QStringLiteral("first_block_number")));
            const uint32_t count = number(step.value(QStringLiteral("block_count")));
            const uint32_t bytes = number(step.value(QStringLiteral("block_bytes")));
            require(count && bytes && count <= 65535 - first && uint64_t(count) * bytes == expected,
                    "Invalid DFU read block range");
            image.reserve(expected);
            for (uint32_t i = 0; i < count; ++i) {
                usb.checkpoint();
                const Bytes block = usb.transfer(step.value(QStringLiteral("request")).toString(),
                    uint16_t(first + i), Bytes(bytes), true);
                if (step.value(QStringLiteral("status_after_each_block")) == QJsonValue(true)) usb.status();
                image.insert(image.end(), block.begin(), block.end());
                if (progress) progress("codeplug", i + 1, count);
            }
        } else if (operation == QStringLiteral("exit_program_mode")) {
            require(image.size() == expected, "Incomplete DFU radio image");
            usb.exitProgramMode(definition.document().value(QStringLiteral("methods")).toObject()
                .value(QStringLiteral("exit_program_mode")).toObject(), step);
        } else throw std::runtime_error("Unsupported DFU read operation");
    }
    require(image.size() == expected, "Incomplete DFU radio image");
    usb.checkpoint();
    return image;
}

void DfuExecutor::write(const RadioDefinition &definition, const Bytes &image,
                        const RadioExecutor::Progress &progress)
{
    const Bytes payload = writePayload(definition, image);
    Usb usb(definition, nullptr);
    uint32_t totalBlocks = 0;
    for (const QJsonValue &entry : definition.writePlan().value(QStringLiteral("operations")).toArray())
        if (entry.toObject().value(QStringLiteral("operation")) == QStringLiteral("write_blocks"))
            totalBlocks += number(entry.toObject().value(QStringLiteral("block_count")));
    uint32_t finishedBlocks = 0;
    for (const QJsonValue &entry : definition.writePlan().value(QStringLiteral("operations")).toArray()) {
        const Object step = entry.toObject();
        const QString operation = step.value(QStringLiteral("operation")).toString();
        if (operation == QStringLiteral("validate_codeplug")) continue;
        if (operation == QStringLiteral("enter_dfu_idle")) usb.enterIdle();
        else if (operation == QStringLiteral("command")) runCommand(usb, step, true);
        else if (operation == QStringLiteral("upload")) {
            usb.transfer(QStringLiteral("upload"), uint16_t(number(step.value(QStringLiteral("value")))),
                         Bytes(number(step.value(QStringLiteral("bytes")))), true);
            if (step.value(QStringLiteral("status_after")) == QJsonValue(true)) usb.status();
        } else if (operation == QStringLiteral("delay")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(number(step.value(QStringLiteral("milliseconds")))));
        } else if (operation == QStringLiteral("erase_blocks")) {
            for (const QJsonValue &address : step.value(QStringLiteral("addresses")).toArray())
                usb.command(step.value(QStringLiteral("command")).toString(), number(address), true);
        } else if (operation == QStringLiteral("get_status")) usb.status();
        else if (operation == QStringLiteral("write_blocks")) {
            const uint32_t first = number(step.value(QStringLiteral("first_block_number")));
            const uint32_t count = number(step.value(QStringLiteral("block_count")));
            const uint32_t bytes = number(step.value(QStringLiteral("block_bytes")));
            const size_t offset = step.contains(QStringLiteral("source_offset"))
                ? number(step.value(QStringLiteral("source_offset"))) : 0;
            require(count && bytes && count <= 65535 - first && offset <= payload.size()
                        && uint64_t(count) * bytes <= payload.size() - offset,
                    "Invalid DFU write block range");
            for (uint32_t i = 0; i < count; ++i) {
                try {
                    usb.transfer(step.value(QStringLiteral("request")).toString(), uint16_t(first + i),
                                 Bytes(payload.begin() + offset + size_t(i) * bytes,
                                       payload.begin() + offset + size_t(i + 1) * bytes), false);
                    usb.waitDownloadIdle();
                } catch (const std::exception &error) {
                    throw std::runtime_error("DFU write block " + std::to_string(i + 1) + ": "
                        + error.what() + ". The radio may contain a partial codeplug; retry the complete write.");
                }
                if (progress) progress("codeplug", ++finishedBlocks, totalBlocks);
            }
        } else if (operation == QStringLiteral("exit_program_mode")) {
            usb.exitProgramMode(definition.document().value(QStringLiteral("methods")).toObject()
                .value(QStringLiteral("exit_program_mode")).toObject(), step);
        } else throw std::runtime_error("Unsupported DFU write operation");
    }
}
