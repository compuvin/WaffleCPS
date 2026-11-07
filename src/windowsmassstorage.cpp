#include "windowsmassstorage.h"

#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <ntddscsi.h>
#include <ntddstor.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string winError(const char *step)
{
    return std::string(step) + ": Windows error " + std::to_string(GetLastError());
}

bool belongsToUsbDevice(DEVINST node, const std::wstring &prefix)
{
    for (int depth = 0; depth < 8; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            throw std::runtime_error("Cannot identify Windows storage device ancestor");
        std::wstring value(id);
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c) { return std::towupper(c); });
        if (value.compare(0, prefix.size(), prefix) == 0
            && (value.size() == prefix.size() || value[prefix.size()] == L'\\'
                || value[prefix.size()] == L'&')) return true;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return false;
}

std::wstring diskPath(uint16_t vendorId, uint16_t productId)
{
    wchar_t id[32] = {};
    std::swprintf(id, sizeof(id) / sizeof(id[0]), L"USB\\VID_%04X&PID_%04X", vendorId, productId);
    const std::wstring prefix(id);
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) throw std::runtime_error(winError("Enumerate Windows disks"));
    struct Guard {
        HDEVINFO devices;
        ~Guard() { SetupDiDestroyDeviceInfoList(devices); }
    } guard{devices};
    std::wstring selected;
    unsigned matches = 0;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &GUID_DEVINTERFACE_DISK, index, &iface)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw std::runtime_error(winError("Enumerate Windows disk interface"));
        }
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &bytes, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
            || bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            throw std::runtime_error(winError("Size Windows disk interface"));
        std::unique_ptr<BYTE[]> storage(new BYTE[bytes]{});
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.get());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, bytes, nullptr, &device))
            throw std::runtime_error(winError("Inspect Windows disk interface"));
        if (!belongsToUsbDevice(device.DevInst, prefix)) continue;
        selected = detail->DevicePath;
        ++matches;
    }
    if (matches == 0) throw std::runtime_error("No Windows storage disk matches radio USB IDs");
    if (matches != 1) throw std::runtime_error("Multiple Windows storage disks match radio USB IDs");
    return selected;
}

} // namespace

WindowsMassStorage::WindowsMassStorage(uint16_t vendorId, uint16_t productId)
{
    const std::wstring path = diskPath(vendorId, productId);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED)
            throw WindowsElevationRequired("Open Windows radio storage device: Windows error 5");
        throw std::runtime_error("Open Windows radio storage device: Windows error "
                                 + std::to_string(error));
    }
    handle_ = handle;
}

WindowsMassStorage::~WindowsMassStorage()
{
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

WindowsMassStorage::Bytes WindowsMassStorage::command(const Bytes &cdb, const Bytes &payload,
                                                      uint32_t responseBytes, bool dataIn,
                                                      uint32_t timeoutMs)
{
    if (cdb.size() != 16 || (dataIn && !payload.empty()) || (!dataIn && responseBytes != 0))
        throw std::runtime_error("Invalid Windows radio SCSI command");
    const size_t length = dataIn ? responseBytes : payload.size();
    if (length == 0 || length > 16384)
        throw std::runtime_error("Windows radio SCSI transfer exceeds supported size");
    constexpr size_t senseBytes = 32;
    const size_t dataAt = (sizeof(SCSI_PASS_THROUGH) + senseBytes + 7) & ~size_t(7);
    Bytes packet(dataAt + length);
    auto *request = reinterpret_cast<SCSI_PASS_THROUGH *>(packet.data());
    request->Length = sizeof(SCSI_PASS_THROUGH);
    request->CdbLength = uint8_t(cdb.size());
    request->SenseInfoLength = senseBytes;
    request->DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
    request->DataTransferLength = DWORD(length);
    request->TimeOutValue = std::max<uint32_t>(1, (timeoutMs + 999) / 1000);
    request->DataBufferOffset = dataAt;
    request->SenseInfoOffset = sizeof(SCSI_PASS_THROUGH);
    std::copy(cdb.begin(), cdb.end(), request->Cdb);
    if (!dataIn) std::copy(payload.begin(), payload.end(), packet.begin() + dataAt);
    DWORD returned = 0;
    if (!DeviceIoControl(static_cast<HANDLE>(handle_), IOCTL_SCSI_PASS_THROUGH,
                         packet.data(), DWORD(packet.size()), packet.data(), DWORD(packet.size()),
                         &returned, nullptr))
        throw std::runtime_error(winError("Windows radio SCSI pass-through"));
    if (request->ScsiStatus != 0)
    {
        std::ostringstream message;
        message << "Windows radio SCSI status " << unsigned(request->ScsiStatus)
                << " for CDB opcode 0x" << std::hex << std::setw(2) << std::setfill('0')
                << unsigned(cdb[0]) << "; sense:";
        const size_t senseLength = std::min<size_t>(request->SenseInfoLength, senseBytes);
        for (size_t i = 0; i < senseLength; ++i)
            message << ' ' << std::setw(2) << unsigned(packet[sizeof(SCSI_PASS_THROUGH) + i]);
        throw std::runtime_error(message.str());
    }
    if (request->DataTransferLength != length)
        throw std::runtime_error("Short Windows radio SCSI transfer: "
                                 + std::to_string(request->DataTransferLength) + " of "
                                 + std::to_string(length) + " bytes");
    return dataIn ? Bytes(packet.begin() + dataAt, packet.end()) : payload;
}
