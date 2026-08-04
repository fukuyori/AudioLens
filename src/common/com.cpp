#include "common/com.h"

#include <format>
#include <vector>

namespace audiolens {

std::string hresultToString(HRESULT hr) {
    // Well-known WASAPI codes first: FormatMessage does not know them and would
    // render them as a bare hex value, which is the least useful thing in a log.
    switch (static_cast<unsigned long>(hr)) {
        case 0x88890001: return "AUDCLNT_E_NOT_INITIALIZED (0x88890001)";
        case 0x88890002: return "AUDCLNT_E_ALREADY_INITIALIZED (0x88890002)";
        case 0x88890003: return "AUDCLNT_E_WRONG_ENDPOINT_TYPE (0x88890003)";
        case 0x88890004: return "AUDCLNT_E_DEVICE_INVALIDATED (0x88890004)";
        case 0x88890005: return "AUDCLNT_E_NOT_STOPPED (0x88890005)";
        case 0x88890006: return "AUDCLNT_E_BUFFER_TOO_LARGE (0x88890006)";
        case 0x88890007: return "AUDCLNT_E_OUT_OF_ORDER (0x88890007)";
        case 0x88890008: return "AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008)";
        case 0x8889000a: return "AUDCLNT_E_DEVICE_IN_USE (0x8889000a)";
        case 0x8889000e: return "AUDCLNT_E_CPUUSAGE_EXCEEDED (0x8889000e)";
        case 0x8889000f: return "AUDCLNT_E_BUFFER_ERROR (0x8889000f)";
        case 0x88890010: return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED (0x88890010)";
        case 0x88890019: return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED (0x88890019)";
        case 0x8889001a: return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY (0x8889001a)";
        case 0x8889001d: return "AUDCLNT_E_EVENTHANDLE_NOT_SET (0x8889001d)";
        case 0x8889001e: return "AUDCLNT_E_INCORRECT_BUFFER_SIZE (0x8889001e)";
        case 0x8889001f: return "AUDCLNT_E_BUFFER_SIZE_ERROR (0x8889001f)";
        case 0x88890021: return "AUDCLNT_E_INVALID_DEVICE_PERIOD (0x88890021)";
        default: break;
    }

    LPWSTR buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::string message;
    if (len != 0 && buffer != nullptr) {
        message = wideToUtf8(buffer);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }

    if (message.empty()) {
        return std::format("0x{:08x}", static_cast<unsigned long>(hr));
    }
    return std::format("{} (0x{:08x})", message, static_cast<unsigned long>(hr));
}

std::string wideToUtf8(const wchar_t* s) {
    if (s == nullptr || *s == L'\0') {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

}  // namespace audiolens
