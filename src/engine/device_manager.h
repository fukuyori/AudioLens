#pragma once

#include "common/com.h"

#include <audioclient.h>
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

/// Initialises a shared-mode stream, taking the low-latency path where the
/// endpoint offers one.
///
/// `IAudioClient::Initialize` treats its duration as a floor and then rounds up
/// to whatever period the audio engine happens to be running at — asking for
/// 10 ms and being handed 22 ms is routine. Everything downstream is sized from
/// that period, so it sets the floor on the whole pipeline's latency.
///
/// `IAudioClient3` lets the period itself be chosen, down to the smallest the
/// driver supports. It is not always available, and every way it can fail falls
/// back to the original call, so this can only ever do better or the same.
///
/// On AudioLens's own path it currently does neither, and the reason is worth
/// recording rather than rediscovering. Measured through a virtual cable and a
/// USB headset:
///
///   * The capture side is a **loopback** stream, and loopback refuses the
///     low-latency path outright. Asked for exactly the default period — 480
///     frames, inside the reported 128..480 range, with a fundamental of 1 so
///     every value is a legal multiple — it still returned
///     AUDCLNT_E_INVALID_DEVICE_PERIOD. The only thing separating that call
///     from the render one that succeeded is AUDCLNT_STREAMFLAGS_LOOPBACK.
///   * The render side takes the path, and gains nothing by it: that endpoint
///     reports a minimum period equal to its default.
///
/// So it stays for the case where the capture is not a loopback tap, and for
/// what its logging says about an endpoint. It is not a latency fix here.
bool initializeSharedStream(IAudioClient* client, DWORD streamFlags, const WAVEFORMATEX* format,
                            std::uint32_t requestedMs, const char* what, std::string* error);

/// Resolves a user-supplied selector to a device id. The selector is either a
/// full device id, a 1-based index into `enumerateDevices(direction)`, or a
/// case-insensitive substring of a friendly name. Returns an empty string when
/// nothing matches or the selector is ambiguous.
std::wstring resolveDeviceSelector(const std::string& selector, DeviceDirection direction,
                                   std::string* error);

}  // namespace audiolens
