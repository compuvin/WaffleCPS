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

// Standalone capture verification and diagnostic block-file reader.
int runCommandLine(int argc, char **argv);
} // namespace Cs7000Usb
