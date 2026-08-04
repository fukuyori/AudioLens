#pragma once

#include "common/windows_lean.h"

#include <combaseapi.h>
#include <wrl/client.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace audiolens {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

enum class ComThreadingModel { SingleThreaded, MultiThreaded };

/// Initializes COM for the lifetime of the object.
///
/// Audio threads want MTA. A Qt GUI thread must be STA, because Qt calls
/// OleInitialize on it for drag and drop and that fails outright against an
/// MTA apartment.
class ComApartment {
public:
    explicit ComApartment(ComThreadingModel model = ComThreadingModel::MultiThreaded) {
        const DWORD flags = model == ComThreadingModel::SingleThreaded ? COINIT_APARTMENTTHREADED
                                                                      : COINIT_MULTITHREADED;
        hr_ = ::CoInitializeEx(nullptr, flags);
        if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("CoInitializeEx failed");
        }
        // S_FALSE means the apartment already existed and our call merely added
        // a reference, so it still has to be balanced by CoUninitialize.
        owns_ = SUCCEEDED(hr_);
    }
    ~ComApartment() {
        if (owns_) {
            ::CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT hr_ = S_OK;
    bool owns_ = false;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { ::CoTaskMemFree(p); }
};

template <typename T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

/// Owns a Win32 HANDLE, closing it on destruction.
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE h = nullptr) noexcept {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
        }
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

std::string hresultToString(HRESULT hr);
std::string wideToUtf8(const wchar_t* s);
std::wstring utf8ToWide(const std::string& s);

}  // namespace audiolens
