#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace audiolens::app {

/// One command line's worth of instructions (requirement F-36).
///
/// Every field is optional because the command line is a set of adjustments to
/// whatever the app is already doing, not a full description of a state. Asking
/// for a preset must not silently return the volume to some default.
///
/// Parsing is deliberately separate from applying: the same text has to be
/// understood by the process that was launched *and* by the one already running
/// that ends up carrying it out, and the second one only ever sees the argument
/// list. One parser means the two cannot drift apart.
struct ControlRequest {
    // --- what to change ---
    std::optional<QString> preset;   ///< Preset id, or its display name.
    std::optional<int> volume;       ///< Absolute, 0-100.
    std::optional<int> volumeStep;   ///< Relative, added to whatever is set.
    std::optional<int> balance;      ///< Absolute, -50 (left) .. +50 (right).
    std::optional<int> bass;
    std::optional<int> clarity;
    std::optional<int> leveling;

    /// Processing on or off. `powerToggle` is applied first, so `--toggle --on`
    /// is not a contradiction: the explicit one wins.
    std::optional<bool> power;
    bool powerToggle = false;

    /// The A/B comparison, latched rather than held. Never saved: see the note
    /// where it is applied.
    std::optional<bool> bypass;

    // --- what to report ---
    bool status = false;
    bool listPresets = false;

    // --- the window and the process ---
    bool show = false;
    bool hide = false;
    bool quit = false;

    // --- settled by whichever process was launched, never forwarded ---
    bool minimized = false;
    bool verbose = false;
    bool version = false;
    bool help = false;

    /// Empty when the parse succeeded.
    QString error;

    /// Whether there is anything here for a *running* instance to do.
    ///
    /// False for a bare launch, and for one carrying only the startup-time
    /// options, which describe how to come up and mean nothing to a process
    /// that is already up.
    bool actsOnRunningInstance() const;

    /// Whether anything here would change what the app is doing, as opposed to
    /// only asking about it.
    ///
    /// The difference decides what happens when nothing is running: a request
    /// that changes something is worth starting the app for, and a question is
    /// not. Launching a background app in order to report that it is running
    /// answers itself, and leaves behind the very thing the user was asking
    /// about.
    bool changesState() const;
};

/// Understands the arguments after the executable name.
///
/// Never fails outright: an unusable command line comes back with `error` set,
/// so the caller can print the reason together with the usage rather than
/// having to guess at it.
ControlRequest parseControlRequest(const QStringList& arguments);

/// The usage text, in English and ASCII only.
///
/// It is written into a console whose code page is not knowable from here — the
/// user's, not ours — and the one thing worse than untranslated help is help
/// that arrives as question marks.
QString controlUsage();

}  // namespace audiolens::app
