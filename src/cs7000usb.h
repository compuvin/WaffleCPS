#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Cs7000Usb {
using Bytes = std::vector<uint8_t>;
using Progress = std::function<void(const std::string &region,
                                    uint32_t completedBlocks, uint32_t totalBlocks)>;

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Radio read cancelled") {}
};

// Complete decoded image: main region followed by contact region, without padding.
// Cancellation takes effect between completed request/response exchanges; never programs the radio.
Bytes read(const Progress &progress, const std::atomic_bool &cancelled);

struct WriteBlock {
    uint8_t command;
    uint16_t selector;
    uint32_t extent;
    Bytes request;
};
// Pure planning and injectable transport allow protocol tests without USB.
std::vector<WriteBlock> planWrite(const Bytes &image);
using Command = std::function<Bytes(uint8_t, const Bytes &, uint32_t)>;
void executeWrite(const Bytes &image, const Command &command, const Progress &progress);
void write(const Bytes &image, const Progress &progress);

// Standalone capture verification and diagnostic block-file reader.
int runCommandLine(int argc, char **argv);
} // namespace Cs7000Usb
