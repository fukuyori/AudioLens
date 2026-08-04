// INITGUID must precede the device headers so that CLSID_MMDeviceEnumerator and
// the PKEY_* property keys are emitted into this translation unit.
#include <initguid.h>

#include "engine/device_manager.h"

#include "common/log.h"

#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace audiolens {
namespace {

ComPtr<IMMDeviceEnumerator> createEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        AL_ERROR("MMDeviceEnumerator の作成に失敗: {}", hresultToString(hr));
        return nullptr;
    }
    return enumerator;
}

EDataFlow toDataFlow(DeviceDirection direction) {
    return direction == DeviceDirection::Render ? eRender : eCapture;
}

std::string readFriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
        return {};
    }
    PROPVARIANT value;
    ::PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8(value.pwszVal);
    }
    ::PropVariantClear(&value);
    return name;
}

std::wstring readDeviceId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || raw == nullptr) {
        return {};
    }
    CoTaskMemPtr<WCHAR> owned(raw);
    return std::wstring(owned.get());
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::vector<DeviceInfo> enumerateDevices(DeviceDirection direction) {
    std::vector<DeviceInfo> result;

    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    if (!enumerator) {
        return result;
    }

    const DeviceInfo defaultInfo = defaultDevice(direction);

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(toDataFlow(direction), DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        AL_ERROR("エンドポイントの列挙に失敗: {}", hresultToString(hr));
        return result;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return result;
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        DeviceInfo info;
        info.id = readDeviceId(device.Get());
        info.friendlyName = readFriendlyName(device.Get());
        info.direction = direction;
        info.isDefault = !info.id.empty() && info.id == defaultInfo.id;
        result.push_back(std::move(info));
    }

    return result;
}

DeviceInfo defaultDevice(DeviceDirection direction) {
    DeviceInfo info;
    info.direction = direction;

    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    if (!enumerator) {
        return info;
    }

    ComPtr<IMMDevice> device;
    const HRESULT hr = enumerator->GetDefaultAudioEndpoint(toDataFlow(direction), eConsole, &device);
    if (FAILED(hr)) {
        // No default device is a normal state on a machine with no audio hardware.
        return info;
    }

    info.id = readDeviceId(device.Get());
    info.friendlyName = readFriendlyName(device.Get());
    info.isDefault = true;
    return info;
}

ComPtr<IMMDevice> openDevice(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    if (!enumerator) {
        return nullptr;
    }
    ComPtr<IMMDevice> device;
    const HRESULT hr = enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(hr)) {
        AL_ERROR("デバイスを開けません: {}", hresultToString(hr));
        return nullptr;
    }
    return device;
}

std::wstring resolveDeviceSelector(const std::string& selector, DeviceDirection direction,
                                   std::string* error) {
    const auto fail = [error](std::string message) -> std::wstring {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return {};
    };

    if (selector.empty()) {
        return fail("デバイス指定が空です");
    }

    if (selector == "default") {
        const DeviceInfo info = defaultDevice(direction);
        if (info.id.empty()) {
            return fail("既定デバイスが見つかりません");
        }
        return info.id;
    }

    const std::vector<DeviceInfo> devices = enumerateDevices(direction);

    // A full device id wins outright.
    const std::wstring wide = utf8ToWide(selector);
    for (const DeviceInfo& info : devices) {
        if (info.id == wide) {
            return info.id;
        }
    }

    // 1-based index into the listing shown by --list.
    int index = 0;
    const auto [ptr, ec] = std::from_chars(selector.data(), selector.data() + selector.size(), index);
    if (ec == std::errc{} && ptr == selector.data() + selector.size()) {
        if (index < 1 || static_cast<std::size_t>(index) > devices.size()) {
            return fail(std::format("番号 {} は範囲外です(1〜{})", index, devices.size()));
        }
        return devices[static_cast<std::size_t>(index) - 1].id;
    }

    // Substring of a friendly name, which must match exactly one device.
    const std::string needle = toLower(selector);
    const DeviceInfo* match = nullptr;
    int matches = 0;
    for (const DeviceInfo& info : devices) {
        if (toLower(info.friendlyName).find(needle) != std::string::npos) {
            match = &info;
            ++matches;
        }
    }
    if (matches == 1) {
        return match->id;
    }
    if (matches > 1) {
        return fail(std::format("'{}' に一致するデバイスが {} 個あります。番号で指定してください",
                                selector, matches));
    }
    return fail(std::format("'{}' に一致するデバイスがありません", selector));
}

}  // namespace audiolens
