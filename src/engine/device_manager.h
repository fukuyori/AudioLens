#pragma once

#include "common/com.h"

#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace audiolens {

enum class DeviceDirection { Render, Capture };

struct DeviceInfo {
    std::wstring id;
    std::string friendlyName;  ///< UTF-8, for display and logging.
    DeviceDirection direction = DeviceDirection::Render;
    bool isDefault = false;
};

/// Enumerates active endpoints in the given direction. Devices that are
/// unplugged or disabled are not returned.
std::vector<DeviceInfo> enumerateDevices(DeviceDirection direction);

/// Resolves the current default endpoint for the eConsole role.
/// Returns an empty id if there is no default device.
DeviceInfo defaultDevice(DeviceDirection direction);

/// Opens an endpoint by its device id.
ComPtr<IMMDevice> openDevice(const std::wstring& id);

/// Resolves a user-supplied selector to a device id. The selector is either a
/// full device id, a 1-based index into `enumerateDevices(direction)`, or a
/// case-insensitive substring of a friendly name. Returns an empty string when
/// nothing matches or the selector is ambiguous.
std::wstring resolveDeviceSelector(const std::string& selector, DeviceDirection direction,
                                   std::string* error);

}  // namespace audiolens
