#include "engine/default_device.h"

#include "common/com.h"
#include "engine/device_manager.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstddef>
#include <cstring>
#include <format>
#include <vector>

namespace audiolens {
namespace {

// The undocumented interface behind the Sound settings applet. Only
// SetDefaultEndpoint is used, but every method above it has to be declared:
// the vtable is matched by position, so a missing or mis-typed entry above
// would silently call the wrong function.
//
// This layout is the one Windows has used since Vista and is what every
// device-switching utility relies on. There is no header for it.
struct DeviceShareMode;

struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// CPolicyConfigClient
const CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

}  // namespace

std::wstring currentDefaultRenderDeviceId() {
    return defaultDevice(DeviceDirection::Render).id;
}

bool setDefaultRenderDevice(const std::wstring& deviceId, std::string* error) {
    auto fail = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };

    if (deviceId.empty()) {
        return fail("デバイス ID が空です。");
    }

    ComPtr<IPolicyConfig> policy;
    const HRESULT hr =
        ::CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy));
    if (FAILED(hr)) {
        // Most likely this Windows version no longer offers the interface.
        return fail(std::format(
            "既定デバイスの変更機能を利用できません (0x{:08X})。"
            "サウンド設定から手動で切り替えてください。",
            static_cast<unsigned>(hr)));
    }

    // Console first: it is the role everything falls back to, so if only one of
    // the two takes effect, that is the one worth having.
    for (const ERole role : {eConsole, eMultimedia}) {
        const HRESULT setHr = policy->SetDefaultEndpoint(deviceId.c_str(), role);
        if (FAILED(setHr)) {
            return fail(std::format("既定デバイスを変更できません (0x{:08X})。",
                                    static_cast<unsigned>(setHr)));
        }
    }
    return true;
}

bool setRenderDeviceSampleRate(const std::wstring& deviceId, std::uint32_t sampleRate,
                               std::uint32_t* previousRate, std::string* error) {
    auto fail = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };

    ComPtr<IPolicyConfig> policy;
    HRESULT hr =
        ::CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy));
    if (FAILED(hr)) {
        return fail(std::format("既定デバイス設定を利用できません (0x{:08X})。",
                                static_cast<unsigned>(hr)));
    }

    WAVEFORMATEX* raw = nullptr;
    hr = policy->GetDeviceFormat(deviceId.c_str(), 0, &raw);
    if (FAILED(hr) || raw == nullptr) {
        return fail(
            std::format("デバイスの形式を取得できません (0x{:08X})。", static_cast<unsigned>(hr)));
    }
    const CoTaskMemPtr<WAVEFORMATEX> current(raw);

    if (previousRate != nullptr) {
        *previousRate = current->nSamplesPerSec;
    }
    // Zero means "tell me what it is and change nothing" — otherwise a caller
    // wanting only to read the rate would set it to zero, which the endpoint
    // rejects with E_INVALIDARG well after the read has already succeeded.
    if (sampleRate == 0 || current->nSamplesPerSec == sampleRate) {
        return true;
    }

    // The format is very likely a WAVEFORMATEXTENSIBLE, so it is copied whole
    // rather than rebuilt: only the rate and the byte rate derived from it
    // change, and the channel mask and subformat carry over untouched.
    const std::size_t bytes = sizeof(WAVEFORMATEX) + current->cbSize;
    std::vector<std::byte> buffer(bytes);
    std::memcpy(buffer.data(), current.get(), bytes);

    auto* next = reinterpret_cast<WAVEFORMATEX*>(buffer.data());
    next->nSamplesPerSec = sampleRate;
    next->nAvgBytesPerSec = sampleRate * next->nBlockAlign;

    hr = policy->SetDeviceFormat(deviceId.c_str(), next, next);
    if (FAILED(hr)) {
        return fail(std::format("デバイスの形式を変更できません (0x{:08X})。"
                                "そのレートに対応していない可能性があります。",
                                static_cast<unsigned>(hr)));
    }
    return true;
}

}  // namespace audiolens
