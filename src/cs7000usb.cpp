#include "cs7000usb.h"

#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
void checkInterrupt() { require(!interrupted, "Interrupted; partial output retained"); }
uint16_t be16(const Bytes &b, size_t p)
{
    require(p + 2 <= b.size(), "Truncated big-endian field");
    return uint16_t((b[p] << 8) | b[p + 1]);
}
uint32_t be32(const Bytes &b, size_t p)
{
    return (uint32_t(be16(b, p)) << 16) | be16(b, p + 2);
}
uint16_t le16(const Bytes &b, size_t p)
{
    require(p + 2 <= b.size(), "Truncated little-endian field");
    return uint16_t(b[p] | (b[p + 1] << 8));
}
uint32_t le32(const Bytes &b, size_t p)
{
    return uint32_t(le16(b, p)) | (uint32_t(le16(b, p + 2)) << 16);
}
void put32(Bytes &b, size_t p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b.at(p + i) = uint8_t(value >> (8 * i));
}
std::string hex(const Bytes &b)
{
    std::ostringstream out;
    for (uint8_t byte : b)
        out << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return out.str();
}
bool zero(const Bytes &b)
{
    return std::all_of(b.begin(), b.end(), [](uint8_t v) { return v == 0; });
}
Bytes makeRequest(uint8_t deviceClass, uint8_t command, const Bytes &payload)
{
    const bool identification = command == 0x06 &&
        (deviceClass == 0x02 || deviceClass == 0x03 || deviceClass == 0x05);
    const bool setup = deviceClass == 0x05 && command == 0x15;
    const bool read = deviceClass == 0x05 && (command == 0x10 || command == 0x2c);
    require(identification || setup || read, "Command outside captured read sequence");
    require((read && payload.size() == 4) || (!read && payload.empty()),
            "Unexpected request payload");
    uint16_t sum = 0;
    for (uint8_t v : payload) sum = uint16_t(sum + v);
    Bytes b{0xa1, deviceClass, command, uint8_t(payload.size() >> 8), uint8_t(payload.size())};
    b.insert(b.end(), payload.begin(), payload.end());
    b.insert(b.end(), {uint8_t(sum >> 8), uint8_t(sum), 0xa4});
    return b;
}
void validateFrame(const Bytes &b)
{
    require(b.size() >= 8 && b.front() == 0xa1 && b.back() == 0xa4,
            "Invalid frame delimiters");
    const size_t length = be16(b, 3);
    require(b.size() == length + 8, "Frame payload length mismatch");
    uint16_t sum = 0;
    for (size_t i = 5; i < 5 + length; ++i) sum = uint16_t(sum + b[i]);
    require(sum == be16(b, 5 + length), "Frame checksum mismatch");
}
Bytes vendorCdb(uint8_t opcode)
{
    require(opcode == 0x28 || opcode == 0x2a, "Invalid vendor transport opcode");
    Bytes cdb(16, 0);
    cdb[0] = 0xff; cdb[1] = opcode; cdb[14] = 0x47; cdb[15] = 0x50;
    return cdb;
}
struct Transaction {
    uint32_t packet = 0, tag = 0, length = 0;
    uint8_t flags = 0;
    Bytes cdb, data;
    bool vendor() const { return cdb.size() == 16 && cdb[0] == 0xff; }
};
void validateCsw(const Bytes &csw, const Transaction &t, bool successRequired)
{
    require(csw.size() == 13 && le32(csw, 0) == 0x53425355, "Invalid CSW signature/length");
    require(le32(csw, 4) == t.tag, "CSW tag mismatch");
    require(uint64_t(t.data.size()) + le32(csw, 8) == t.length, "CSW residue mismatch");
    require(csw[12] <= 2, "Invalid CSW status");
    if (successRequired) require(csw[12] == 0 && le32(csw, 8) == 0,
                                 "Vendor transaction failed or transferred short data");
}
struct Exchange {
    uint32_t packet = 0;
    Bytes request, response;
    std::vector<uint32_t> polls;
};
struct Plan {
    std::vector<Exchange> exchanges;
    uint32_t mainSize = 0, contactSize = 0;
    size_t transactions = 0, frames = 0, blocks = 0;
    bool direct = false;
};
std::vector<Exchange> remainingRegionRequests(uint8_t command, uint32_t regionSize)
{
    require(command == 0x10 || command == 0x2c, "Unsupported region command");
    require(regionSize > 0 && regionSize <= 0x4000u * 2048,
            "Region exceeds block-selector range");
    const uint32_t count = (regionSize + 2047) / 2048;
    std::vector<Exchange> requests;
    // Block 0 has already been read to discover regionSize.
    for (uint32_t index = 1; index < count; ++index) {
        const bool finalContact = command == 0x2c && index == count - 1;
        const uint16_t selector = uint16_t(index | (finalContact ? 0x4000 : 0xc000));
        Exchange exchange;
        exchange.request = makeRequest(0x05, command, {
            uint8_t(selector >> 8), uint8_t(selector), 0x08, 0x00
        });
        exchange.polls = {2063};
        requests.push_back(std::move(exchange));
    }
    return requests;
}
Exchange firstRegionRequest(uint8_t command)
{
    return {0, makeRequest(0x05, command, {0, 0, 8, 0}), {}, {2063}};
}
Plan directPlan()
{
    Plan plan;
    plan.direct = true;
    for (uint8_t deviceClass : {2, 2, 3, 3, 5}) {
        plan.exchanges.push_back({0, makeRequest(deviceClass, 0x06, {}), {},
                                  std::vector<uint32_t>(deviceClass == 5 ? 1 : 10, 265)});
    }
    plan.exchanges.push_back({0, makeRequest(0x05, 0x15, {}), {}, {10}});
    plan.exchanges.push_back(firstRegionRequest(0x10));
    return plan;
}
void validateBlock(const Bytes &request, const Bytes &response, uint32_t size)
{
    require(response.size() == 2063 && be16(response, 3) == 2055,
            "Unexpected block response size");
    require(be16(request, 7) == 2048, "Unexpected requested block size");
    require(be16(response, 6) == (be16(request, 5) & 0x3fff), "Returned block index mismatch");
    require(be32(response, 8) == size, "Region size differs from read plan");
    require(uint64_t(be16(response, 6)) * 2048 < size, "Block outside region");
}
void discoverRegion(Plan &plan, const Exchange &exchange, const Bytes &response)
{
    const bool main = exchange.request[2] == 0x10;
    uint32_t &size = main ? plan.mainSize : plan.contactSize;
    if (!plan.direct || size != 0) return;
    require(be16(exchange.request, 5) == 0, "Region discovery requires block 0");
    const uint32_t discovered = be32(response, 8);
    auto remaining = remainingRegionRequests(exchange.request[2], discovered);
    validateBlock(exchange.request, response, discovered);
    if (!main) require(le32(response, 16) == discovered, "Contact header size mismatch");
    size = discovered;
    plan.blocks += remaining.size() + 1;
    plan.exchanges.insert(plan.exchanges.end(),
                          std::make_move_iterator(remaining.begin()),
                          std::make_move_iterator(remaining.end()));
    if (main) plan.exchanges.push_back(firstRegionRequest(0x2c));
}
void validateResponse(const Bytes &request, const Bytes &response)
{
    validateFrame(response);
    require(response[1] == uint8_t(request[1] | 0x80) && response[2] == request[2],
            "Response class/command mismatch");
    require(be16(response, 3) >= 1 && response[5] == 1, "Radio returned unsuccessful response");
    if (request[2] == 0x06) {
        const std::string model = "CS7000-M17 PLUS";
        require(response.size() == 265 &&
                std::equal(model.begin(), model.end(), response.begin() + 6) &&
                response[6 + model.size()] == 0, "Unexpected radio model/identification format");
    } else if (request[2] == 0x15) {
        require(response == Bytes({0xa1, 0x85, 0x15, 0, 2, 1, 0, 0, 1, 0xa4}),
                "Unexpected setup response");
    }
}
Plan loadPlan(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot open capture: " + path);
    Bytes bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(bytes.size() >= 28 && le32(bytes, 0) == 0x0a0d0d0a &&
            le32(bytes, 8) == 0x1a2b3c4d, "Expected little-endian pcapng");
    std::vector<uint16_t> links;
    std::vector<Transaction> transactions;
    Transaction active;
    bool pending = false;
    int radioBus = -1, radioDevice = -1;
    uint32_t packet = 0;
    size_t offset = 0;
    while (offset < bytes.size()) {
        require(bytes.size() - offset >= 12, "Truncated pcapng block");
        uint32_t type = le32(bytes, offset), length = le32(bytes, offset + 4);
        require(length >= 12 && length % 4 == 0 && length <= bytes.size() - offset &&
                le32(bytes, offset + length - 4) == length, "Malformed pcapng block");
        if (type == 0x0a0d0d0a) {
            require(offset == 0, "Multiple pcapng sections are unsupported");
        } else if (type == 1) {
            require(length >= 20, "Truncated interface descriptor");
            links.push_back(le16(bytes, offset + 8));
        } else if (type == 6) {
            ++packet;
            require(length >= 32, "Truncated enhanced packet block");
            uint32_t interface = le32(bytes, offset + 8), cap = le32(bytes, offset + 20);
            require(interface < links.size() && cap <= length - 32, "Invalid captured packet");
            if (links[interface] == 249) {
                Bytes p(bytes.begin() + offset + 28, bytes.begin() + offset + 28 + cap);
                require(p.size() >= 27, "Truncated USBPcap header");
                uint16_t header = le16(p, 0), bus = le16(p, 17), device = le16(p, 19);
                require(header >= 27 && header <= p.size(), "Invalid USBPcap header length");
                uint32_t dataSize = le32(p, 23);
                require(dataSize == p.size() - header, "USBPcap payload truncated");
                const uint8_t ep = p[21], stage = p[16] & 1;
                if (p[22] == 3 && ((ep == 1 && stage == 0) || (ep == 0x81 && stage == 1)) && dataSize) {
                    Bytes data(p.begin() + header, p.end());
                    const bool cbw = data.size() == 31 && le32(data, 0) == 0x43425355;
                    if (cbw && data[15] == 0xff && radioDevice == -1) {
                        radioBus = bus; radioDevice = device;
                    }
                    if (bus == radioBus && device == radioDevice) {
                        require(le32(p, 10) == 0, "USB error in captured transaction");
                        if (cbw) {
                            require(!pending && ep == 1, "Overlapping captured transactions");
                            require(data[14] >= 1 && data[14] <= 16 && data[13] == 0 &&
                                    (data[12] == 0 || data[12] == 0x80), "Unsupported CBW fields");
                            active = {packet, le32(data, 4), le32(data, 8), data[12],
                                      Bytes(data.begin() + 15, data.begin() + 15 + data[14]), {}};
                            pending = true;
                        } else if (data.size() == 13 && le32(data, 0) == 0x53425355) {
                            require(pending && ep == 0x81, "CSW without matching CBW");
                            validateCsw(data, active, active.vendor());
                            if (active.vendor()) transactions.push_back(active);
                            pending = false;
                        } else {
                            require(pending && ep == (active.flags ? 0x81 : 1), "Unexpected bulk data");
                            active.data.insert(active.data.end(), data.begin(), data.end());
                            require(active.data.size() <= active.length, "Excess transaction data");
                        }
                    }
                }
            }
        }
        offset += length;
    }
    require(!pending && !transactions.empty(), "Incomplete capture or no vendor transactions");
    Plan plan;
    plan.transactions = transactions.size();
    for (const auto &t : transactions) {
        require(t.cdb == vendorCdb(t.flags ? 0x28 : 0x2a), "Unrecognized vendor CDB");
        if (!t.flags) {
            validateFrame(t.data);
            Bytes payload(t.data.begin() + 5, t.data.end() - 3);
            require(t.data == makeRequest(t.data[1], t.data[2], payload), "Request not allowed");
            plan.exchanges.push_back({t.packet, t.data, {}, {}});
            ++plan.frames;
        } else {
            require(!plan.exchanges.empty(), "Response without request");
            auto &e = plan.exchanges.back();
            require(e.response.empty(), "Poll after completed response");
            require(t.length > 0 && t.length <= 2063, "Poll length outside supported transport");
            e.polls.push_back(t.length);
            if (!zero(t.data)) {
                validateResponse(e.request, t.data);
                e.response = t.data;
                ++plan.frames;
                if (e.request[2] == 0x10 || e.request[2] == 0x2c) {
                    uint32_t &region = e.request[2] == 0x10 ? plan.mainSize : plan.contactSize;
                    if (!region) region = be32(t.data, 8);
                    validateBlock(e.request, e.response, region);
                    ++plan.blocks;
                }
            }
        }
    }
    require(plan.exchanges.size() >= 7, "Read plan too short");
    const std::array<uint8_t, 6> classes{2, 2, 3, 3, 5, 5};
    for (size_t i = 0; i < classes.size(); ++i) {
        const auto &e = plan.exchanges[i];
        require(e.request == makeRequest(classes[i], i == 5 ? 0x15 : 0x06, {}),
                "Unexpected identification/setup sequence");
        require(i < 4 ? e.response.empty() : !e.response.empty(), "Unexpected probe result");
        require(!e.polls.empty(), "Identification/setup request has no polling plan");
    }
    uint32_t mainIndex = 0;
    bool contacts = false;
    std::vector<uint16_t> contactIndexes;
    for (size_t i = 6; i < plan.exchanges.size(); ++i) {
        const auto &e = plan.exchanges[i];
        require(!e.response.empty() && !e.polls.empty(), "Missing block response");
        require(e.request[2] == 0x10 || e.request[2] == 0x2c, "Non-read command after setup");
        uint16_t index = be16(e.response, 6);
        if (e.request[2] == 0x10) {
            require(!contacts && index == mainIndex++, "Main blocks are not sequential");
        } else {
            contacts = true;
            require(std::find(contactIndexes.begin(), contactIndexes.end(), index) == contactIndexes.end(),
                    "Duplicate contact block");
            contactIndexes.push_back(index);
        }
    }
    require(plan.mainSize && plan.contactSize &&
            mainIndex == (uint64_t(plan.mainSize) + 2047) / 2048 &&
            !contactIndexes.empty() && contactIndexes.front() == 0, "Incomplete main/contact read plan");
    return plan;
}

class Usb {
    libusb_context *context = nullptr;
    libusb_device_handle *handle = nullptr;
    bool detached = false, claimed = false;
    std::ostream &log;
    const std::atomic_bool *cancelled;
    void checkpoint()
    {
        checkInterrupt();
        if (cancelled && cancelled->load()) throw Cs7000Usb::Cancelled();
    }
    uint32_t nextTag = 1;
    void cleanup() noexcept
    {
        if (handle) {
            if (claimed) {
                const int r = libusb_release_interface(handle, 0);
                if (r < 0) std::cerr << "Release interface: " << libusb_error_name(r) << '\n';
            }
            if (detached) {
                const int r = libusb_attach_kernel_driver(handle, 0);
                if (r < 0) std::cerr << "Restore usb-storage: " << libusb_error_name(r) << '\n';
            }
            libusb_close(handle);
        }
        if (context) libusb_exit(context);
    }
    void transfer(uint8_t endpoint, Bytes &data, bool exact)
    {
        int actual = 0;
        const int r = libusb_bulk_transfer(handle, endpoint, data.data(), int(data.size()), &actual, 3000);
        require(r == 0, "USB bulk transfer: " + std::string(libusb_error_name(r)));
        require(!exact || size_t(actual) == data.size(), "Short USB transfer");
        data.resize(size_t(actual));
    }
public:
    explicit Usb(std::ostream &output, const std::atomic_bool *stopRequested = nullptr)
        : log(output), cancelled(stopRequested)
    {
        try {
            int r = libusb_init(&context);
            require(r == 0, "libusb initialization failed");
            libusb_device **list = nullptr;
            const ssize_t count = libusb_get_device_list(context, &list);
            require(count >= 0, "USB enumeration failed");
            libusb_device *selected = nullptr;
            size_t matches = 0;
            for (ssize_t i = 0; i < count; ++i) {
                libusb_device_descriptor descriptor{};
                if (libusb_get_device_descriptor(list[i], &descriptor) == 0 &&
                    descriptor.idVendor == 0x0483 && descriptor.idProduct == 0x5720) {
                    selected = list[i]; ++matches;
                }
            }
            if (matches == 1) r = libusb_open(selected, &handle);
            libusb_free_device_list(list, 1);
            require(matches != 0,
                    "No supported radio found. Check that the radio is powered on "
                    "and its programming cable is connected.");
            require(matches == 1,
                    "Multiple matching USB devices found. Connect only the radio "
                    "you want to program.");
            require(r != LIBUSB_ERROR_ACCESS,
                    "USB access denied. Install the Linux USB permission rule described in "
                    "docs/RadioReading.md, then reconnect the radio.");
            require(r == 0, "Open USB device: " + std::string(libusb_error_name(r)));
            libusb_config_descriptor *config = nullptr;
            r = libusb_get_active_config_descriptor(libusb_get_device(handle), &config);
            require(r == 0, "Cannot read active USB configuration");
            bool compatible = false;
            for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
                for (int a = 0; a < config->interface[i].num_altsetting; ++a) {
                    const auto &iface = config->interface[i].altsetting[a];
                    if (iface.bInterfaceNumber != 0 || iface.bAlternateSetting != 0 ||
                        iface.bInterfaceClass != 8 || iface.bInterfaceSubClass != 6 ||
                        iface.bInterfaceProtocol != 0x50) continue;
                    bool in = false, out = false;
                    for (uint8_t j = 0; j < iface.bNumEndpoints; ++j) {
                        const auto &ep = iface.endpoint[j];
                        if ((ep.bmAttributes & 3) == 2 && ep.wMaxPacketSize == 64) {
                            in |= ep.bEndpointAddress == 0x81;
                            out |= ep.bEndpointAddress == 0x01;
                        }
                    }
                    compatible = in && out;
                }
            }
            libusb_free_config_descriptor(config);
            require(compatible, "USB interface does not match captured mass-storage transport");
            r = libusb_kernel_driver_active(handle, 0);
            require(r >= 0, "Cannot inspect kernel driver");
            if (r == 1) {
                r = libusb_detach_kernel_driver(handle, 0);
                require(r == 0, "Detach usb-storage: " + std::string(libusb_error_name(r)));
                detached = true;
            }
            r = libusb_claim_interface(handle, 0);
            require(r == 0, "Claim interface: " + std::string(libusb_error_name(r)));
            claimed = true;
        } catch (...) { cleanup(); throw; }
    }
    ~Usb() { cleanup(); }
    Usb(const Usb &) = delete;
    Usb &operator=(const Usb &) = delete;
    void checkCancellation() { checkpoint(); }

    Bytes command(uint8_t opcode, const Bytes &request, uint32_t length)
    {
        require(length > 0 && length <= (opcode == 0x2a ? 2064u : 2063u), "Transport length outside supported plan");
        Transaction t{0, nextTag++, length, uint8_t(opcode == 0x28 ? 0x80 : 0), vendorCdb(opcode), {}};
        Bytes cbw(31, 0);
        put32(cbw, 0, 0x43425355); put32(cbw, 4, t.tag); put32(cbw, 8, length);
        cbw[12] = t.flags; cbw[14] = 16;
        std::copy(t.cdb.begin(), t.cdb.end(), cbw.begin() + 15);
        log << "CBW tag=" << t.tag << " opcode=" << unsigned(opcode) << " length=" << length << '\n';
        log.flush();
        transfer(1, cbw, true);
        t.data = t.flags ? Bytes(length) : request;
        require(t.data.size() == length, "Outbound payload length mismatch");
        transfer(t.flags ? 0x81 : 1, t.data, !t.flags);
        log << "DATA " << hex(t.data) << '\n'; log.flush();
        Bytes csw(13);
        transfer(0x81, csw, true);
        log << "CSW " << hex(csw) << '\n'; log.flush();
        validateCsw(csw, t, true);
        return t.data;
    }
};

struct ReadSummary {
    uint32_t mainSize, contactSize;
    size_t blocks;
};
using BlockSink = std::function<void(bool main, uint16_t index, uint16_t selector, uint32_t size,
                                      uint32_t mainSize, const Bytes &response)>;
ReadSummary executePlan(Plan plan, Usb &usb, std::ostream &log,
                        const BlockSink &sink, const Cs7000Usb::Progress &progress)
{
    size_t saved = 0;
    for (size_t step = 0; step < plan.exchanges.size(); ++step) {
        // Discovery appends requests, so keep a copy across vector reallocations.
        const Exchange e = plan.exchanges[step];
        usb.checkCancellation();
        log << "REQUEST capture_cbw_packet=" << e.packet << " frame=" << hex(e.request) << '\n';
        usb.command(0x2a, e.request, uint32_t(e.request.size()));
        Bytes response;
        const bool expectResponse = e.request[1] == 0x05;
        const size_t limit = e.polls.size() + (expectResponse ? 20 : 0);
        for (size_t poll = 0; poll < limit; ++poll) {
            require(!e.polls.empty(), "Request has no polling plan");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Bytes data = usb.command(0x28, {}, e.polls[std::min(poll, e.polls.size() - 1)]);
            if (!zero(data)) {
                require(expectResponse, "Unexpected response to unsuccessful probe");
                validateResponse(e.request, data);
                response = std::move(data);
                break;
            }
        }
        require(expectResponse == !response.empty(), "Timed out waiting for framed response");
        usb.checkCancellation();
        if (e.request[2] != 0x10 && e.request[2] != 0x2c) continue;
        const bool main = e.request[2] == 0x10;
        discoverRegion(plan, e, response);
        const uint32_t size = main ? plan.mainSize : plan.contactSize;
        validateBlock(e.request, response, size);
        const uint16_t index = be16(response, 6);
        sink(main, index, be16(e.request, 5), size, plan.mainSize, response);
        ++saved;
        if (progress) progress(main ? "main" : "contacts", uint32_t(index) + 1, (size + 2047) / 2048);
    }
    require(saved == plan.blocks, "Read ended before all planned blocks were saved");
    return {plan.mainSize, plan.contactSize, saved};
}

void readRadio(const Plan &plan, const std::filesystem::path &output)
{
    require(std::filesystem::create_directory(output), "Output directory already exists");
    std::ofstream log(output / "transactions.log");
    std::ofstream manifest(output / "coverage.jsonl");
    require(bool(log) && bool(manifest), "Cannot create log/manifest");
    log.exceptions(std::ios::badbit | std::ios::failbit);
    manifest.exceptions(std::ios::badbit | std::ios::failbit);
    manifest << "{\"type\":\"start\",\"main_size\":" << plan.mainSize
             << ",\"contact_size\":" << plan.contactSize
             << ",\"format\":\"decoded-blocks\",\"plan\":\""
             << (plan.direct ? "direct-full" : "capture-driven") << "\"}\n";
    manifest.flush();
    try {
        Usb usb(log);
        std::array<bool, 2> discovered{};
        const auto sink = [&](bool main, uint16_t index, uint16_t selector, uint32_t size, uint32_t mainSize,
                              const Bytes &response) {
            const std::string region = main ? "main" : "contacts";
            if (plan.direct && !discovered[main ? 0 : 1]) {
                discovered[main ? 0 : 1] = true;
                manifest << "{\"type\":\"region\",\"region\":\"" << region
                         << "\",\"size\":" << size << ",\"offset\":" << (main ? 0 : mainSize) << "}\n";
                std::cout << "Discovered " << region << ": " << size << " bytes, "
                          << (size + 2047) / 2048 << " blocks\n";
            }
            const uint32_t valid = std::min(uint32_t(2048), size - uint32_t(index) * 2048);
            const std::string name = region + "-" + std::to_string(index) + ".bin";
            std::ofstream block(output / name, std::ios::binary);
            block.exceptions(std::ios::badbit | std::ios::failbit);
            block.write(reinterpret_cast<const char *>(response.data() + 12), 2048);
            block.close();
            manifest << "{\"type\":\"block\",\"region\":\"" << region
                     << "\",\"index\":" << index << ",\"selector\":" << selector
                     << ",\"offset\":" << uint64_t(index) * 2048 + (main ? 0 : mainSize)
                     << ",\"valid_bytes\":" << valid << ",\"file\":\"" << name << "\"}\n";
            manifest.flush();
        };
        const auto progress = [](const std::string &region, uint32_t completed, uint32_t total) {
            std::cout << "Read " << region << " block " << completed - 1
                      << " (" << completed << '/' << total << " in region)\n";
        };
        const auto summary = executePlan(plan, usb, log, sink, progress);
        manifest << "{\"type\":\"complete\",\"blocks\":" << summary.blocks
                 << ",\"main_size\":" << summary.mainSize
                 << ",\"contact_size\":" << summary.contactSize << "}\n";
        manifest.flush();
    } catch (...) {
        try { log << "FAILED; output is partial\n"; log.flush(); } catch (...) {}
        throw;
    }
}

class NullBuffer : public std::streambuf {
    int overflow(int value) override { return traits_type::not_eof(value); }
    std::streamsize xsputn(const char *, std::streamsize count) override { return count; }
};
} // namespace

int Cs7000Usb::runCommandLine(int argc, char **argv)
{
    try {
        std::string capture, output;
        bool read = false, direct = false;
        const std::string usage = "Usage: cs7000-read (--capture FILE | --direct) "
                                  "[--read --output NEW_DIRECTORY]";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--read") read = true;
            else if (arg == "--direct") direct = true;
            else if (arg == "--capture" && i + 1 < argc) capture = argv[++i];
            else if (arg == "--output" && i + 1 < argc) output = argv[++i];
            else throw std::runtime_error(usage);
        }
        require(direct != !capture.empty() && (read ? !output.empty() : output.empty()), usage);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        const Plan plan = direct ? directPlan() : loadPlan(capture);
        if (direct) std::cout << "Direct full read: region sizes will be discovered from radio responses.\n";
        else std::cout << "Verified " << plan.transactions << " vendor transactions, " << plan.frames
                  << " frames, " << plan.blocks << " blocks. Region sizes: " << plan.mainSize
                  << " + " << plan.contactSize << " bytes.\n";
        if (read) readRadio(plan, output);
        else std::cout << "Offline verification only; USB was not opened.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}

Cs7000Usb::Bytes Cs7000Usb::read(const Progress &progress, const std::atomic_bool &cancelled)
{
    if (cancelled.load()) throw Cancelled();
    NullBuffer buffer;
    std::ostream log(&buffer);
    Usb usb(log, &cancelled);
    Bytes image;
    const auto sink = [&](bool main, uint16_t index, uint16_t, uint32_t size, uint32_t mainSize,
                          const Bytes &response) {
        const size_t base = main ? 0 : mainSize;
        const size_t offset = base + size_t(index) * 2048;
        const size_t valid = std::min(size_t(2048), size_t(size) - size_t(index) * 2048);
        if (index == 0) image.resize(base + size);
        require(offset + valid <= image.size(), "Downloaded block exceeds image");
        std::copy_n(response.begin() + 12, valid, image.begin() + offset);
    };
    const auto summary = executePlan(directPlan(), usb, log, sink, progress);
    require(image.size() == size_t(summary.mainSize) + summary.contactSize,
            "Incomplete radio image");
    if (cancelled.load()) throw Cancelled();
    return image;
}

namespace {
Bytes writeFrame(uint8_t command, const Bytes &payload)
{
    require((command == 0x14 && payload.empty()) ||
            ((command == 0x11 || command == 0x2d) && payload.size() == 2056),
            "Command outside captured write sequence");
    uint16_t sum = 0;
    for (auto value : payload) sum = uint16_t(sum + value);
    Bytes frame{0xa1, 5, command, uint8_t(payload.size() >> 8), uint8_t(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.insert(frame.end(), {uint8_t(sum >> 8), uint8_t(sum), 0xa4});
    return frame;
}
}

std::vector<Cs7000Usb::WriteBlock> Cs7000Usb::planWrite(const Bytes &image)
{
    require(image.size() >= 0x12a4 && image.size() <= 64u * 1024 * 1024, "Invalid image size");
    const std::string model("CS7000-M17 PLUS\0", 16);
    require(std::equal(model.begin(), model.end(), image.begin() + 0x1200), "Unsupported codeplug model");
    const std::string version(image.begin() + 0x1298, image.begin() + 0x12a4);
    require(version == std::string("V1.02.20.00\0", 12) || version == std::string("V1.02.21.00\0", 12),
            "Unsupported codeplug CPS version");
    const uint32_t main = le32(image, 0x1264);
    require(main >= 0x12a4 && main <= 0x4000u * 2048 && main == le32(image, 0x240), "Invalid main extent");
    const uint32_t count = le32(image, main), size = le32(image, size_t(main) + 4);
    require(size >= 8 && size <= 0x4000u * 2048 && uint64_t(main) + size == image.size(), "Invalid contact extent");
    require(count <= 10000, "Contact count exceeds radio capacity");
    const uint32_t directoryEnd = 8 + count * 4;
    require(directoryEnd <= size && directoryEnd <= 0x3f7a03, "Contact directory exceeds region");
    for (uint32_t at = 8; at < directoryEnd; at += 4) {
        const auto ref = le32(image, size_t(main) + at);
        require(ref >= 1 && ref <= count, "Invalid contact reference");
    }
    const uint32_t recordsEnd = count ? 0x3f7a03 + (count - 1) * 16 + 21 : 0;
    const uint32_t extent = std::max(directoryEnd, recordsEnd);
    require(extent <= size, "Contact records exceed region");
    std::set<uint32_t> contacts;
    for (uint32_t i = 0; i < (directoryEnd + 2047) / 2048; ++i) contacts.insert(i);
    if (count) for (uint32_t i = 0x3f7a03 / 2048; i < (recordsEnd + 2047) / 2048; ++i) contacts.insert(i);
    std::vector<WriteBlock> blocks;
    auto append = [&](uint8_t command, uint32_t index, uint32_t base, uint32_t length, bool final) {
        const uint16_t selector = uint16_t(final ? index | 0x4000 : index ? index | 0xc000 : 0);
        const size_t offset = size_t(base) + size_t(index) * 2048;
        require(offset <= image.size() && image.size() - offset >= 2048, "Incomplete source block");
        Bytes payload{uint8_t(selector >> 8), uint8_t(selector), uint8_t(length >> 24),
                      uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length), 8, 0};
        payload.insert(payload.end(), image.begin() + offset, image.begin() + offset + 2048);
        blocks.push_back({command, selector, length, writeFrame(command, payload)});
    };
    for (uint32_t i = 0; i < (main + 2047) / 2048; ++i) append(0x11, i, 0, main, false);
    for (auto i : contacts) append(0x2d, i, main, extent, i == *contacts.rbegin());
    return blocks;
}

void Cs7000Usb::executeWrite(const Bytes &image, const Command &command, const Progress &progress)
{
    const auto blocks = planWrite(image); // Validate everything before the first USB command.
    Bytes previousAck;
    auto exchange = [&](const Bytes &request, uint32_t responseSize, bool expectResponse) {
        command(0x2a, request, uint32_t(request.size()));
        for (unsigned poll = 0; poll < (expectResponse ? 30u : 10u); ++poll) {
            const auto response = command(0x28, {}, responseSize);
            if (zero(response)) continue;
            require(expectResponse, "Unexpected identification response");
            // The vendor capture contains one repeated acknowledgement.
            if (!previousAck.empty() && response == previousAck) continue;
            validateResponse(request, response);
            return response;
        }
        require(!expectResponse, "Timed out waiting for radio acknowledgement");
        return Bytes{};
    };
    for (uint8_t deviceClass : {2, 2, 3, 3, 5})
        exchange(makeRequest(deviceClass, 6, {}), 265, deviceClass == 5);
    const auto setup = exchange(writeFrame(0x14, {}), 10, true);
    require(setup == Bytes({0xa1, 0x85, 0x14, 0, 2, 1, 0, 0, 1, 0xa4}), "Unexpected write setup acknowledgement");
    uint32_t mainTotal = 0, contactTotal = 0, mainDone = 0, contactDone = 0;
    for (const auto &block : blocks) (block.command == 0x11 ? mainTotal : contactTotal)++;
    for (const auto &block : blocks) {
        try {
            const auto ack = exchange(block.request, 11, true);
            require(ack.size() == 11 && be16(ack, 3) == 3 &&
                    be16(ack, 6) == (block.selector & 0x3fff), "Write acknowledgement index mismatch");
            previousAck = ack;
        } catch (const std::exception &error) {
            throw std::runtime_error(std::string(block.command == 0x11 ? "Settings" : "Contacts") +
                " block " + std::to_string(block.selector & 0x3fff) + ": " + error.what() +
                ". The radio may contain a partial codeplug. Retry the complete write before using it.");
        }
        const bool main = block.command == 0x11;
        if (progress) progress(main ? "main" : "contacts", main ? ++mainDone : ++contactDone,
                               main ? mainTotal : contactTotal);
    }
}

void Cs7000Usb::write(const Bytes &image, const Progress &progress)
{
    planWrite(image);
    NullBuffer buffer;
    std::ostream log(&buffer);
    Usb usb(log);
    executeWrite(image, [&](uint8_t opcode, const Bytes &request, uint32_t length) {
        if (opcode == 0x28) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return usb.command(opcode, request, length);
    }, progress);
}
