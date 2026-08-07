#include "app/control_channel.h"

#include "common/log.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

#include <memory>

namespace audiolens::app {
namespace {

/// A request is a handful of short flags. Anything larger is not one of ours,
/// and a peer that keeps writing without ever sending the terminator would
/// otherwise grow the buffer without bound.
constexpr int kMaxRequestBytes = 64 * 1024;

/// Long enough that a busy machine still answers, short enough that a wedged
/// instance does not hang the shell it was typed into.
///
/// The reply allowance is generous on purpose. An AudioLens that is still
/// starting has accepted the connection but cannot answer until its event loop
/// runs, which is a couple of seconds away while WASAPI comes up. Waiting is
/// the right thing to do with that: giving up would report no running instance,
/// and the caller would start a second one.
constexpr int kConnectTimeoutMs = 500;
constexpr int kReplyTimeoutMs = 15000;
constexpr int kIdleClientTimeoutMs = 5000;

/// Newline-terminated JSON, one message each way. The framing exists because a
/// pipe is a byte stream: without it, a request split across two reads is
/// indistinguishable from a short one.
QByteArray frame(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace

QString controlChannelName() {
    // Hashed rather than used directly. A user name can contain characters a
    // pipe name cannot, and on a domain machine it can be long enough to run
    // into the limit; the hash is neither.
    const QByteArray user = qgetenv("USERNAME") + '\\' + qgetenv("USERDOMAIN");
    const QByteArray digest =
        QCryptographicHash::hash(user, QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("AudioLens-control-") + QString::fromLatin1(digest);
}

Delivery sendToRunningInstance(const QStringList& arguments, QString* reply, bool* accepted) {
    const auto unreachable = [reply](const QString& why) {
        if (reply != nullptr) {
            *reply = QStringLiteral("AudioLens is running but would not answer: %1\n").arg(why);
        }
        return Delivery::Unreachable;
    };

    QLocalSocket socket;
    socket.connectToServer(controlChannelName());
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        // The one error that means what it says. Everything else -- a pipe that
        // exists but refuses, a peer that accepts and then stops -- is a running
        // AudioLens we failed to reach, and must not be mistaken for an absent
        // one.
        if (socket.error() == QLocalSocket::ServerNotFoundError) {
            return Delivery::NoInstance;
        }
        return unreachable(socket.errorString());
    }

    QJsonObject request;
    request.insert(QStringLiteral("args"), QJsonArray::fromStringList(arguments));
    socket.write(frame(request));
    if (!socket.waitForBytesWritten(kReplyTimeoutMs)) {
        return unreachable(socket.errorString());
    }

    QByteArray buffer;
    while (!buffer.contains('\n')) {
        if (!socket.waitForReadyRead(kReplyTimeoutMs)) {
            return unreachable(socket.errorString());
        }
        buffer.append(socket.readAll());
        if (buffer.size() > kMaxRequestBytes) {
            return unreachable(QStringLiteral("the answer did not end"));
        }
    }

    const QJsonObject answer =
        QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n'))).object();
    if (reply != nullptr) {
        *reply = answer.value(QStringLiteral("text")).toString();
    }
    if (accepted != nullptr) {
        *accepted = answer.value(QStringLiteral("ok")).toBool(false);
    }
    return Delivery::Answered;
}

ControlServer::ControlServer(QObject* parent) : QObject(parent) {
    connect(&server_, &QLocalServer::newConnection, this, &ControlServer::onNewConnection);
}

bool ControlServer::listen(Handler handler) {
    handler_ = std::move(handler);

    // Only the user who started AudioLens may talk to it. The default on
    // Windows is a pipe every account on the machine can open, and the commands
    // here reach the audio routing of a logged-in session.
    server_.setSocketOptions(QLocalServer::UserAccessOption);

    if (!server_.listen(controlChannelName())) {
        // A name left behind by a process that died. On Windows a named pipe
        // goes with its owner, so this only ever clears a genuinely dead one --
        // and we are only here because connecting to it just failed.
        QLocalServer::removeServer(controlChannelName());
        if (!server_.listen(controlChannelName())) {
            AL_WARN("Command-line control is unavailable: {}",
                    server_.errorString().toStdString());
            return false;
        }
    }
    return true;
}

void ControlServer::onNewConnection() {
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();

        connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const int end = buffer->indexOf('\n');
            if (end < 0) {
                if (buffer->size() > kMaxRequestBytes) {
                    socket->abort();
                }
                return;
            }

            QStringList arguments;
            const QJsonObject request =
                QJsonDocument::fromJson(buffer->left(end)).object();
            for (const QJsonValue& value : request.value(QStringLiteral("args")).toArray()) {
                arguments << value.toString();
            }

            QString text;
            const bool ok = handler_ ? handler_(arguments, &text) : false;

            QJsonObject answer;
            answer.insert(QStringLiteral("ok"), ok);
            answer.insert(QStringLiteral("text"), text);
            socket->write(frame(answer));

            // Waited on rather than left to the event loop. --quit is answered
            // by a process that is about to stop running one, and an unflushed
            // reply would be lost exactly when the user most wants to know
            // whether the app took the instruction.
            socket->flush();
            socket->waitForBytesWritten(kReplyTimeoutMs);
            socket->disconnectFromServer();
        });

        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);

        // A peer that connects and then says nothing holds a slot forever. The
        // socket is the timer's context object, so a connection that finishes
        // normally cancels this on its way out.
        QTimer::singleShot(kIdleClientTimeoutMs, socket, [socket] {
            if (socket->state() != QLocalSocket::UnconnectedState) {
                socket->abort();
            }
        });
    }
}

}  // namespace audiolens::app
