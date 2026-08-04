#pragma once

#include "common/com.h"
#include "engine/device_manager.h"

#include <functional>
#include <mutex>
#include <string>

namespace audiolens {

/// What happened to the audio devices.
enum class DeviceChange {
    DefaultChanged,   ///< The system default endpoint moved to another device.
    Added,            ///< A device appeared.
    Removed,          ///< A device disappeared, or was disabled/unplugged.
    StateChanged,     ///< A device was enabled or disabled.
};

/// Watches the system's audio endpoints and reports changes.
///
/// Required by N-03: the engine must follow devices being plugged and unplugged
/// and the default output moving, rather than falling silent. WASAPI only tells
/// a running client about this indirectly, by failing its next call with
/// AUDCLNT_E_DEVICE_INVALIDATED — which says something broke but not what is
/// available now. This fills that gap.
///
/// The callback arrives on an MMDevice notification thread, not the caller's.
/// It must return quickly and must not call back into WASAPI; treat it as a
/// signal to wake something else up.
class DeviceWatcher {
public:
    using Callback = std::function<void(DeviceChange, const std::wstring& deviceId)>;

    DeviceWatcher() = default;
    ~DeviceWatcher();

    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    /// Begins watching. The callback must outlive the watcher.
    bool start(Callback callback, std::string* error);
    void stop();

    bool running() const;

private:
    class Client;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMNotificationClient> client_;
    mutable std::mutex mutex_;
};

}  // namespace audiolens
