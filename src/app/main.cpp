#include "app/default_device_guard.h"
#include "app/main_window.h"
#include "common/com.h"
#include "common/log.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>

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

    bool startMinimized = false;
    bool verbose = false;
    for (const QString& argument : QApplication::arguments().mid(1)) {
        if (argument == QStringLiteral("--minimized")) {
            startMinimized = true;
        } else if (argument == QStringLiteral("--verbose")) {
            verbose = true;
        } else if (argument == QStringLiteral("--version")) {
            // A GUI app has no console to print to, so this has to be a box.
            QMessageBox::information(nullptr, QStringLiteral("AudioLens"),
                                     QStringLiteral("AudioLens %1").arg(AUDIOLENS_VERSION));
            return 0;
        }
    }
    if (verbose) {
        audiolens::setLogLevel(audiolens::LogLevel::Debug);
    }

    // Beside the settings, which is where a user already knows to look, and
    // opened before the window so that a failure during startup is recorded
    // too. A windowed process has no console, so without this every diagnostic
    // the app produces is discarded — including the ones from a fault that
    // happened overnight, which are the only ones nobody can watch happen.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty() && QDir().mkpath(dataDir)) {
        const QString logPath = QDir(dataDir).filePath(QStringLiteral("audiolens.log"));
        audiolens::setLogFile(logPath.toStdString());
    }
    AL_INFO("AudioLens {} 起動", AUDIOLENS_VERSION);

    audiolens::app::MainWindow window;
    if (!startMinimized) {
        window.show();
    }

    return QApplication::exec();
}
