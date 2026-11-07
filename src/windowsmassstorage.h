#pragma once

#include <cstdint>
#include <vector>

// Sends the CDBs declared by a mass-storage .wrd through the Windows storage driver.
class WindowsMassStorage {
public:
    using Bytes = std::vector<uint8_t>;

    WindowsMassStorage(uint16_t vendorId, uint16_t productId);
    ~WindowsMassStorage();
    WindowsMassStorage(const WindowsMassStorage &) = delete;
    WindowsMassStorage &operator=(const WindowsMassStorage &) = delete;

    Bytes command(const Bytes &cdb, const Bytes &payload, uint32_t responseBytes,
                  bool dataIn, uint32_t timeoutMs);

private:
    void *handle_ = nullptr;
};
