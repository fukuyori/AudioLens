#include "app/control_request.h"

namespace audiolens::app {
namespace {

/// Reads the value that follows a flag, as an integer within `lo`..`hi`.
///
/// `index` is advanced past the value on success. Rejecting an out-of-range
/// number here rather than clamping it is on purpose: `--volume 700` is a typo,
/// and silently setting the volume to 100 would hide it.
bool takeInt(const QStringList& arguments, int& index, const QString& flag, int lo, int hi,
             std::optional<int>* out, QString* error) {
    if (index + 1 >= arguments.size()) {
        *error = QStringLiteral("%1 needs a value.").arg(flag);
        return false;
    }
    const QString text = arguments[++index];
    bool valid = false;
    const int value = text.toInt(&valid);
    if (!valid) {
        *error = QStringLiteral("%1: '%2' is not a number.").arg(flag, text);
        return false;
    }
    if (value < lo || value > hi) {
        *error =
            QStringLiteral("%1: %2 is outside %3..%4.").arg(flag).arg(value).arg(lo).arg(hi);
        return false;
    }
    *out = value;
    return true;
}

bool takeText(const QStringList& arguments, int& index, const QString& flag,
              std::optional<QString>* out, QString* error) {
    if (index + 1 >= arguments.size()) {
        *error = QStringLiteral("%1 needs a value.").arg(flag);
        return false;
    }
    *out = arguments[++index];
    return true;
}

bool takeOnOff(const QStringList& arguments, int& index, const QString& flag,
               std::optional<bool>* out, QString* error) {
    if (index + 1 >= arguments.size()) {
        *error = QStringLiteral("%1 needs on or off.").arg(flag);
        return false;
    }
    const QString text = arguments[++index].toLower();
    if (text == QStringLiteral("on")) {
        *out = true;
        return true;
    }
    if (text == QStringLiteral("off")) {
        *out = false;
        return true;
    }
    *error = QStringLiteral("%1: expected on or off, got '%2'.").arg(flag, text);
    return false;
}

}  // namespace

bool ControlRequest::changesState() const {
    return preset.has_value() || volume.has_value() || volumeStep.has_value() ||
           balance.has_value() || bass.has_value() || clarity.has_value() ||
           leveling.has_value() || power.has_value() || powerToggle || bypass.has_value() ||
           show || hide || quit;
}

bool ControlRequest::actsOnRunningInstance() const {
    return changesState() || status || listPresets;
}

ControlRequest parseControlRequest(const QStringList& arguments) {
    ControlRequest request;

    for (int i = 0; i < arguments.size(); ++i) {
        const QString flag = arguments[i];
        bool fine = true;

        if (flag == QStringLiteral("--preset")) {
            fine = takeText(arguments, i, flag, &request.preset, &request.error);
        } else if (flag == QStringLiteral("--volume")) {
            fine = takeInt(arguments, i, flag, 0, 100, &request.volume, &request.error);
        } else if (flag == QStringLiteral("--volume-step")) {
            // Separate from --volume rather than folded into it as a signed
            // value. Balance runs from -50 to +50, so a leading minus already
            // means "an absolute position on the left"; if it meant "relative"
            // on one control and "absolute" on the other, every hotkey anyone
            // wrote would be a coin toss.
            fine = takeInt(arguments, i, flag, -100, 100, &request.volumeStep, &request.error);
        } else if (flag == QStringLiteral("--balance")) {
            fine = takeInt(arguments, i, flag, -50, 50, &request.balance, &request.error);
        } else if (flag == QStringLiteral("--bass")) {
            fine = takeInt(arguments, i, flag, 0, 100, &request.bass, &request.error);
        } else if (flag == QStringLiteral("--clarity")) {
            fine = takeInt(arguments, i, flag, 0, 100, &request.clarity, &request.error);
        } else if (flag == QStringLiteral("--leveling")) {
            fine = takeInt(arguments, i, flag, 0, 100, &request.leveling, &request.error);
        } else if (flag == QStringLiteral("--bypass")) {
            fine = takeOnOff(arguments, i, flag, &request.bypass, &request.error);
        } else if (flag == QStringLiteral("--on")) {
            request.power = true;
        } else if (flag == QStringLiteral("--off")) {
            request.power = false;
        } else if (flag == QStringLiteral("--toggle")) {
            request.powerToggle = true;
        } else if (flag == QStringLiteral("--status")) {
            request.status = true;
        } else if (flag == QStringLiteral("--list-presets")) {
            request.listPresets = true;
        } else if (flag == QStringLiteral("--show")) {
            request.show = true;
        } else if (flag == QStringLiteral("--hide")) {
            request.hide = true;
        } else if (flag == QStringLiteral("--quit")) {
            request.quit = true;
        } else if (flag == QStringLiteral("--minimized")) {
            request.minimized = true;
        } else if (flag == QStringLiteral("--verbose")) {
            request.verbose = true;
        } else if (flag == QStringLiteral("--version")) {
            request.version = true;
        } else if (flag == QStringLiteral("--help") || flag == QStringLiteral("-h") ||
                   flag == QStringLiteral("/?")) {
            request.help = true;
        } else {
            // Refused rather than ignored. An unknown flag is nearly always a
            // misspelt known one, and carrying on would apply every *other*
            // flag on the line while quietly dropping the one that was meant.
            request.error = QStringLiteral("Unknown option '%1'.").arg(flag);
            fine = false;
        }

        if (!fine) {
            break;
        }
    }

    return request;
}

QString controlUsage() {
    return QStringLiteral(
        "AudioLens " AUDIOLENS_VERSION "\n"
        "\n"
        "  AudioLens.exe [options]\n"
        "\n"
        "Options are handed to the copy of AudioLens that is already running, and\n"
        "take effect at once. If none is running, AudioLens starts with them\n"
        "applied. Launching with no options at all brings the window to the front.\n"
        "\n"
        "Sound\n"
        "  --preset <id>         Switch preset. The name shown on screen works\n"
        "                        too. --list-presets shows both.\n"
        "  --volume <0-100>      Master output level. 100 is unity gain.\n"
        "  --volume-step <+-n>   Move the volume by n, for a hotkey.\n"
        "  --balance <-50..50>   Left/right trim. Negative is left, 0 centred.\n"
        "  --bass <0-100>        The three amounts, as on screen. Applied after\n"
        "  --clarity <0-100>     --preset however the flags are ordered, because\n"
        "  --leveling <0-100>    choosing a preset resets all three.\n"
        "\n"
        "Processing\n"
        "  --on, --off, --toggle Start or stop processing.\n"
        "  --bypass on|off       Hear the sound before processing. The path and\n"
        "                        the latency do not change. Not saved.\n"
        "\n"
        "Reporting\n"
        "  --status              Print what is running, and how well.\n"
        "  --list-presets        Print the preset ids and names.\n"
        "\n"
        "Window and process\n"
        "  --show, --hide        Show or hide the window. The tray icon stays.\n"
        "  --quit                Exit, giving the default output device back.\n"
        "  --minimized           Start without showing the window.\n"
        "  --verbose             Log at debug level.\n"
        "  --version, --help\n"
        "\n"
        "Printed output goes to the console that launched AudioLens. Because this\n"
        "is a windowed program the shell does not wait for it, so the text lands\n"
        "after the prompt returns. To read it in order:\n"
        "  Start-Process -Wait -NoNewWindow AudioLens.exe -ArgumentList '--status'\n");
}

}  // namespace audiolens::app
