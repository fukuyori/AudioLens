#include "engine/device_watcher.h"

#include "common/log.h"

#include <atomic>
#include <format>

namespace audiolens {

/// Minimal IMMNotificationClient. Reference counted by hand rather than through
/// a framework: the object is handed to MMDevice, which releases it on its own
/// schedule, so its lifetime cannot be tied to the DeviceWatcher's.
class DeviceWatcher::Client final : public IMMNotificationClient {
public:
    explicit Client(Callback callback) : callback_(std::move(callback)) {}

    // --- IUnknown ---
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // --- IMMNotificationClient ---
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR defaultDeviceId) override {
        // eConsole is the role ordinary playback follows. Reporting all three
        // roles would fire three times for one user action.
        if (flow == eRender && role == eConsole) {
            notify(DeviceChange::DefaultChanged, defaultDeviceId);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override {
        notify(DeviceChange::Added, deviceId);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override {
        notify(DeviceChange::Removed, deviceId);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override {
        // Unplugging a jack shows up here rather than as a removal, so this is
        // the one that matters most for headphones.
        notify(newState == DEVICE_STATE_ACTIVE ? DeviceChange::Added : DeviceChange::Removed,
               deviceId);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        // Fires for volume and name changes among others; nothing here needs it.
        return S_OK;
    }

private:
    ~Client() = default;  // Deleted through Release() only.

    void notify(DeviceChange change, LPCWSTR deviceId) {
        if (callback_) {
            callback_(change, deviceId != nullptr ? std::wstring(deviceId) : std::wstring());
        }
    }

    std::atomic<ULONG> refCount_{1};
    Callback callback_;
};

DeviceWatcher::~DeviceWatcher() { stop(); }

bool DeviceWatcher::start(Callback callback, std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    std::lock_guard<std::mutex> lock(mutex_);
    if (client_) {
        return true;
    }

    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        return fail(std::format("MMDeviceEnumerator の作成に失敗: {}", hresultToString(hr)));
    }

    // Constructed with a reference count of 1, which this ComPtr takes over.
    ComPtr<IMMNotificationClient> client;
    client.Attach(new Client(std::move(callback)));

    hr = enumerator_->RegisterEndpointNotificationCallback(client.Get());
    if (FAILED(hr)) {
        enumerator_.Reset();
        return fail(std::format("デバイス変更通知の登録に失敗: {}", hresultToString(hr)));
    }

    client_ = std::move(client);
    AL_DEBUG("デバイス変更の監視を開始しました");
    return true;
}

void DeviceWatcher::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!client_) {
        return;
    }
    if (enumerator_) {
        // Must happen before dropping the reference: MMDevice can otherwise
        // still be inside a callback on the object we are about to release.
        enumerator_->UnregisterEndpointNotificationCallback(client_.Get());
    }
    client_.Reset();
    enumerator_.Reset();
    AL_DEBUG("デバイス変更の監視を停止しました");
}

bool DeviceWatcher::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(client_);
}

}  // namespace audiolens
