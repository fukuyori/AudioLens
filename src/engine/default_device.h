#pragma once

#include <cstdint>
#include <string>

namespace audiolens {

/// Changing the system's default playback device (requirement N-04).
///
/// AudioLens only sees audio because the virtual cable is the default output.
/// Asking the user to set that by hand in the Sound settings — and to set it
/// back every time the app stops — is both tedious and dangerous: forget the
/// second half and the machine is silent, with no clue as to why.
///
/// **Windows exposes no supported API for this.** The Sound settings applet
/// drives an undocumented COM interface, `IPolicyConfig`, and every tool that
/// switches audio devices uses the same one. It has been stable since Windows 7
/// and works on Windows 11, but it is not contractual: a future release could
/// change or remove it.
///
/// That risk is handled by never depending on success. Every function here
/// reports failure instead of throwing, and the caller falls back to telling
/// the user to change the device themselves. The app still works, it is just
/// less convenient.
///
/// Callers must have COM initialised on the calling thread.

/// The current default playback endpoint for the console role, or an empty
/// string if there is none.
std::wstring currentDefaultRenderDeviceId();

/// Makes `deviceId` the default playback endpoint.
///
/// Only the console and multimedia roles are changed. The communications role
/// is deliberately left alone: if AudioLens is not running, a voice call should
/// still reach a real headset rather than a cable that goes nowhere.
bool setDefaultRenderDevice(const std::wstring& deviceId, std::string* error);

/// Changes an endpoint's shared-mode sample rate, reporting what it was.
/// Pass 0 for `sampleRate` to read the current rate without changing it.
///
/// This is the same change the Sound control panel's "Default Format" makes,
/// and Windows reacts to it by invalidating every stream on that endpoint.
/// Requirement N-03 names sample rate changes as one of the things the engine
/// must survive, and this is how that is provoked on demand — no administrator
/// rights, no unplugging anything.
bool setRenderDeviceSampleRate(const std::wstring& deviceId, std::uint32_t sampleRate,
                               std::uint32_t* previousRate, std::string* error);

}  // namespace audiolens
