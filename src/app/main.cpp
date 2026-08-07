#include "app/control_channel.h"
#include "app/control_request.h"
#include "app/default_device_guard.h"
#include "app/i18n.h"
#include "app/main_window.h"
#include "app/settings_store.h"
#include "common/com.h"
#include "common/log.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>

#include <optional>
#include <string>

#include <windows.h>

namespace {

/// Where printed text should go, or null if there is nowhere for it to go.
///
/// A WIN32-subsystem binary has no output of its own. The setting that stops a
/// console window flashing on every ordinary launch is the same one that throws
/// away everything the process prints, so `--status` has to be given somewhere
/// to answer.
///
/// Worked out once and remembered, because the attach cannot be repeated: a
/// second AttachConsole fails with ERROR_ACCESS_DENIED precisely because the
/// first one worked.
HANDLE consoleOutput() {
    static const HANDLE handle = []() -> HANDLE {
        // A handle the launcher handed us, because it redirected our output
        // into a file or a pipe. That one wins: it is what whoever ran us is
        // reading, and a script capturing `--status` is the case that matters
        // most here.
        const HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
        if (inherited != nullptr && inherited != INVALID_HANDLE_VALUE) {
            return inherited;
        }

        // Otherwise borrow the console of whoever launched us.
        if (AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
            return nullptr;
        }
        // Opened rather than fetched with GetStdHandle: attaching gives the
        // process a console but does not fill in its standard handles, which
        // were empty a moment ago and still are.
        const HANDLE console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
        return console == INVALID_HANDLE_VALUE ? nullptr : console;
    }();
    return handle;
}

/// Written straight to the handle rather than through the C runtime, whose
/// stdout is not wired to anything in a windowed process.
void writeConsole(const QString& text) {
    const HANDLE handle = consoleOutput();
    if (text.isEmpty() || handle == nullptr) {
        return;
    }

    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(handle, &mode)) {
        // A real console takes UTF-16 straight, bypassing the code page. Going
        // through it instead is what turns a preset named in Japanese into a
        // row of question marks on a console that is not set to a Japanese one.
        const std::wstring wide = text.toStdWString();
        WriteConsoleW(handle, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }

    // Redirected to a file or a pipe. UTF-8, which is what anything reading it
    // back will assume, and what a console would have had to convert to anyway.
    const QByteArray utf8 = text.toUtf8();
    WriteFile(handle, utf8.constData(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

/// Says a short thing where the user can see it, console or not.
///
/// Launched from Explorer there is no console and nothing redirected, and a
/// `--version` that printed into nothing would look like a crash.
void announce(const QString& text) {
    if (consoleOutput() != nullptr) {
        writeConsole(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    } else {
        QMessageBox::information(nullptr, QStringLiteral("AudioLens"), text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Installed before anything else can fail. While AudioLens holds the
    // system default output, dying without giving it back leaves the whole
    // machine silent (requirement N-04).
    audiolens::app::DefaultDeviceGuard::installCrashHandler();

    // Qt calls OleInitialize on this thread, which needs an STA. Claiming the
    // apartment here makes that explicit and keeps the device enumeration
    // (which is COM) valid for the lifetime of the app.
    audiolens::ComApartment com(audiolens::ComThreadingModel::SingleThreaded);

    QApplication app(argc, argv);
    // The organisation name is left unset on purpose: it becomes a path
    // component of QStandardPaths::AppDataLocation, and setting it to
    // "AudioLens" too would bury everything in %APPDATA%\AudioLens\AudioLens.
    QApplication::setApplicationName(QStringLiteral("AudioLens"));
    QApplication::setApplicationDisplayName(QStringLiteral("AudioLens"));
    QApplication::setApplicationVersion(QStringLiteral(AUDIOLENS_VERSION));

    // The window is only one way to reach the app; closing it leaves the tray
    // icon behind, so the last window closing must not end the process.
    QApplication::setQuitOnLastWindowClosed(false);

    const QStringList arguments = QApplication::arguments().mid(1);
    const audiolens::app::ControlRequest request =
        audiolens::app::parseControlRequest(arguments);

    // Settled here rather than forwarded. These three answer for the executable
    // that was invoked -- its version, its options, its typo -- and asking the
    // running instance would answer for a different binary after an upgrade.
    if (!request.error.isEmpty()) {
        announce(request.error + QLatin1Char('\n') + audiolens::app::controlUsage());
        return 2;
    }
    if (request.help) {
        announce(audiolens::app::controlUsage());
        return 0;
    }
    if (request.version) {
        announce(QStringLiteral("AudioLens " AUDIOLENS_VERSION));
        return 0;
    }

    // Everything else goes to the instance that is already running, if there is
    // one, and this process is finished (requirement F-36).
    //
    // This is also what makes AudioLens single-instance, which it was not
    // before. Two copies would each redirect the system default output to their
    // own capture device and hand it back on exit, and whichever exited second
    // would restore a device the other had already replaced.
    //
    // Returns a value once the request has been dealt with, and nothing when
    // there is no instance to deal with it.
    const auto deliver = [&arguments]() -> std::optional<int> {
        QString reply;
        bool accepted = false;
        switch (audiolens::app::sendToRunningInstance(arguments, &reply, &accepted)) {
            case audiolens::app::Delivery::Answered:
                writeConsole(reply);
                return accepted ? 0 : 1;
            case audiolens::app::Delivery::Unreachable:
                // Deliberately not falling through to starting one. A second
                // AudioLens would redirect the system default output to its own
                // capture device, and the two would then hand it back to each
                // other in the wrong order (requirement N-04).
                announce(reply);
                return 1;
            case audiolens::app::Delivery::NoInstance:
                break;
        }
        return std::nullopt;
    };
    if (const std::optional<int> code = deliver()) {
        return *code;
    }

    // Nobody was listening. A command line that only asks questions has nothing
    // left to ask, and starting the app in order to answer would both answer
    // itself and leave behind the very thing that was being asked about.
    //
    // A command line that changes something is different, and falls through: it
    // says what the sound should be like, and starting up that way is a
    // reasonable reading of it.
    if (request.quit) {
        announce(QStringLiteral("AudioLens is not running."));
        return 0;
    }
    if (request.actsOnRunningInstance() && !request.changesState()) {
        announce(QStringLiteral("AudioLens is not running."));
        return 1;
    }

    // Claiming the channel is what makes this process *the* instance, and it has
    // to be done here rather than inside the window.
    //
    // Asking first and starting second cannot settle a race, because there is a
    // gap between another launch's CreateProcess and the moment it starts
    // listening. A connect inside that gap finds nothing, concludes there is no
    // AudioLens, and starts a second one -- the exact outcome this channel
    // exists to prevent. Claiming the name does settle it: the pipe is created
    // with FILE_FLAG_FIRST_PIPE_INSTANCE, so exactly one process can win it.
    //
    // Declared before the window so that it is destroyed after it.
    audiolens::app::MainWindow* served = nullptr;
    audiolens::app::ControlServer control;
    const bool claimed =
        control.listen([&served](const QStringList& command, QString* reply) {
            // Cannot run before the event loop does, which is after the window
            // below exists; connections arriving meanwhile queue in the pipe.
            // That holds only as long as nothing between here and exec() spins a
            // nested event loop.
            return served != nullptr && served->handleControlMessage(command, reply);
        });
    if (!claimed) {
        // Either another launch won the name a moment ago, or it could not be
        // opened at all. Asking settles which.
        if (const std::optional<int> code = deliver()) {
            return *code;
        }
        // Nobody there, so the channel is simply unavailable -- logged inside
        // listen(). Carrying on regardless: without it the app is what it was
        // before F-36, which is to say fully usable.
    }

    if (request.verbose) {
        audiolens::setLogLevel(audiolens::LogLevel::Debug);
    }

    // Beside the settings, which is where a user already knows to look, and
    // opened before the window so that a failure during startup is recorded
    // too. A windowed process has no console, so without this every diagnostic
    // the app produces is discarded - including the ones from a fault that
    // happened overnight, which are the only ones nobody can watch happen.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty() && QDir().mkpath(dataDir)) {
        const QString logPath = QDir(dataDir).filePath(QStringLiteral("audiolens.log"));
        audiolens::setLogFile(logPath.toStdString());
    }
    AL_INFO("AudioLens {} starting", AUDIOLENS_VERSION);

    // Before the window is constructed, because every string it builds is
    // translated at construction time. Installing a catalogue afterwards would
    // leave the interface in whatever language it was first laid out in.
    audiolens::app::SettingsStore store;
    const audiolens::app::Language language =
        audiolens::app::installTranslator(app, store.loadSettings().language);
    AL_INFO("Interface language: {}",
            audiolens::app::languageToString(language).toStdString());

    audiolens::app::MainWindow window;
    // From here the channel has somewhere to deliver to. Anything that queued
    // while this was being built is dispatched as soon as exec() runs.
    served = &window;

    // The same request the running instance would have carried out, applied to
    // the one just built. Sharing the path is the point: `--preset movie` has to
    // mean the same thing whether or not AudioLens happened to be up already.
    if (request.actsOnRunningInstance()) {
        QString reply;
        window.applyControlRequest(request, &reply);
        writeConsole(reply);
    }

    if (!request.minimized && !request.hide) {
        window.show();
    }

    return QApplication::exec();
}
