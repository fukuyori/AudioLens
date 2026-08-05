#pragma once

#include <QString>

namespace audiolens::app {

/// Borrows the system's default playback device and guarantees it goes back
/// (requirement N-04).
///
/// AudioLens only hears anything because the virtual cable is the default
/// output. That arrangement has a nasty property: the moment AudioLens is not
/// running, every sound on the machine goes into a cable nobody is listening
/// to. The user gets silence and no explanation.
///
/// A run can end three ways, and each needs its own mechanism:
///
///   1. **A clean stop or quit.** `release()` puts the device back.
///   2. **A crash.** The process-wide exception filter installed by
///      `installCrashHandler()` puts it back on the way down.
///   3. **A kill, a power cut, a blue screen.** Nothing of ours runs at all, so
///      recovery has to happen on the *next* launch. That is why the caller
///      writes the displaced device into the settings file *before* the switch
///      and clears it *after* the restore: a non-empty value at startup means
///      the last run never gave the device back.
///
/// Mechanism 3 is the only one that always works, so it is the one the design
/// leans on. The other two exist to keep the silent window short.
class DefaultDeviceGuard {
public:
    /// The current default playback device, for the caller to persist before
    /// calling `acquire()`. Empty if there is none.
    static QString currentDefault();

    /// Makes `deviceId` the default.
    ///
    /// `restoreDeviceId` is where to leave the default afterwards, and it is the
    /// app's *output* device rather than whatever was displaced. That is a
    /// deliberate choice and not merely a convenience: the output device is by
    /// definition the one the user has been listening on for the whole run, and
    /// unlike the displaced device it is known to exist and to be audible right
    /// now. Putting the displaced device back is the more polite behaviour, but
    /// politeness is worth nothing if the machine ends up silent — and it does
    /// end up silent whenever the displaced device was itself the cable, or has
    /// been unplugged since, or never worked in the first place.
    ///
    /// `fallbackDeviceId` is the displaced device, used only when the restore
    /// target cannot be set. The caller must already have written it somewhere
    /// durable.
    static bool acquire(const QString& deviceId, const QString& restoreDeviceId,
                        const QString& fallbackDeviceId, QString* error);

    /// Points the default at the restore device, or at the fallback if that
    /// fails. Does nothing if nothing is held, and is safe to call repeatedly
    /// and from any thread.
    static void release();

    static bool held();

    /// Installs the last-gasp restore for an unhandled exception. Call once, at
    /// startup, before anything can crash.
    static void installCrashHandler();
};

}  // namespace audiolens::app
