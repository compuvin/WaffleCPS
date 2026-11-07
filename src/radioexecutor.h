#pragma once

#include "radiodefinition.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace RadioExecutor {
using Bytes = std::vector<uint8_t>;
using Progress = std::function<void(const std::string &, uint32_t, uint32_t)>;
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Radio transfer cancelled") {}
};
class UsbPermissionDenied : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct WriteBlock {
    QString region;
    uint8_t command;
    uint16_t selector;
    uint32_t extent;
    uint32_t index;
    Bytes payload;
};
Bytes makeFrame(const RadioDefinition &definition, uint8_t deviceClass,
                uint8_t command, const Bytes &payload);
std::vector<WriteBlock> planWrite(const RadioDefinition &definition,
                                  const QJsonObject &codeplugDefinition, const Bytes &image);
uint16_t readSelector(const RadioDefinition &definition, const QString &region,
                      uint32_t index, uint32_t blockCount);
Bytes read(const RadioDefinition &definition, const Progress &progress,
           const std::atomic_bool &cancelled);
void write(const RadioDefinition &definition, const QJsonObject &codeplugDefinition,
           const Bytes &image, const Progress &progress);
}
