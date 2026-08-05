#include "app/default_device_guard.h"

#include "common/com.h"
#include "engine/default_device.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace audiolens::app {
namespace {

/// Namespace-scope rather than members so the crash filter can reach them
/// without touching any object that may be in the middle of being destroyed.
std::mutex g_mutex;
/// Where to leave the default: the app's output device.
std::wstring g_restoreDeviceId;
/// Where to leave it if that one cannot be set: the device we displaced.
std::wstring g_fallbackDeviceId;
std::atomic<bool> g_held{false};

LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

/// Restores without locking. The crash filter cannot take a mutex — the thread
/// that owns it may be the one that just died — so it calls this instead and
/// accepts the small chance of racing a normal release. Setting the same device
/// twice is harmless; leaving it unset is not.
void restoreUnlocked() {
    if (!g_held.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::wstring target = g_restoreDeviceId;
    const std::wstring fallback = g_fallbackDeviceId;
    g_restoreDeviceId.clear();
    g_fallbackDeviceId.clear();

    // The crashing thread has no apartment of its own if the fault happened on
    // a worker, so claim one. This is best effort by definition.
    const ComApartment com;
    std::string error;
    if (!target.empty() && setDefaultRenderDevice(target, &error)) {
        return;
    }
    // The output device is gone or refused. The displaced device is the second
    // chance; if it is also the cable this changes nothing, but there is
    // nothing better left to try.
    if (!fallback.empty() && fallback != target) {
        setDefaultRenderDevice(fallback, &error);
    }
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    restoreUnlocked();
    return g_previousFilter != nullptr ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

QString DefaultDeviceGuard::currentDefault() {
    const std::wstring id = currentDefaultRenderDeviceId();
    return QString::fromWCharArray(id.c_str(), static_cast<int>(id.size()));
}

bool DefaultDeviceGuard::acquire(const QString& deviceId, const QString& restoreDeviceId,
                                 const QString& fallbackDeviceId, QString* error) {
    if (deviceId.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("取り込み元のデバイスが指定されていません。");
        return false;
    }
    // No early exit when the cable is already the default. It used to return
    // here — nothing to displace, nothing to give back — but that is precisely
    // the case where the machine goes silent on exit, because nobody put the
    // default there on this run and so nobody takes it away either. Holding it
    // anyway costs one redundant call to the policy interface and buys the
    // restore on the way out.

    const std::lock_guard<std::mutex> lock(g_mutex);

    std::string setError;
    if (!setDefaultRenderDevice(deviceId.toStdWString(), &setError)) {
        if (error != nullptr) *error = QString::fromStdString(setError);
        return false;
    }

    // Recorded only after the switch succeeded, so a failure can never leave a
    // restore pointing somewhere the system was not.
    g_restoreDeviceId = restoreDeviceId.toStdWString();
    g_fallbackDeviceId = fallbackDeviceId.toStdWString();
    g_held.store(true, std::memory_order_release);
    return true;
}

void DefaultDeviceGuard::release() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    restoreUnlocked();
}

bool DefaultDeviceGuard::held() { return g_held.load(std::memory_order_acquire); }

void DefaultDeviceGuard::installCrashHandler() {
    g_previousFilter = ::SetUnhandledExceptionFilter(crashFilter);
}

}  // namespace audiolens::app
