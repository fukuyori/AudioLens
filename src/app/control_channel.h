#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace audiolens::app {

/// Name of the pipe the running instance listens on (a named pipe on Windows).
///
/// Per user, not per machine. Two people signed in at once each have their own
/// audio session and their own settings file, so a shared channel would let one
/// of them change the other's volume.
QString controlChannelName();

enum class Delivery {
    /// Nobody is listening. The caller is the first instance and should become
    /// the window rather than the messenger.
    NoInstance,
    /// Carried out. `reply` is the text to print, `accepted` whether it worked.
    Answered,
    /// Somebody is there and the exchange failed anyway. Kept apart from
    /// NoInstance on purpose: starting a second AudioLens because the first one
    /// would not talk is how two copies end up fighting over the default output
    /// device, which is the thing this channel exists to prevent. `reply` says
    /// what went wrong.
    Unreachable,
};

/// Hands `arguments` to an AudioLens that is already running.
Delivery sendToRunningInstance(const QStringList& arguments, QString* reply, bool* accepted);

/// The listening half. Owned by the window, because the window is what the
/// commands act on.
class ControlServer : public QObject {
    Q_OBJECT

public:
    /// Returns false when the request could not be carried out; the string it
    /// fills in is printed either way.
    using Handler = std::function<bool(const QStringList& arguments, QString* reply)>;

    explicit ControlServer(QObject* parent = nullptr);

    /// Returns false if the channel could not be opened, which leaves the app
    /// perfectly usable through its window and only costs it the command line.
    bool listen(Handler handler);

private:
    void onNewConnection();

    QLocalServer server_;
    Handler handler_;
};

}  // namespace audiolens::app
