#include "clitransport.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>

namespace phicore::transport::cli {

namespace {

// Envelope types and the topics a transport produces itself come from
// envelope.h: they are protocol surface, and a copy per transport is how two
// wires drift apart.
constexpr const char kDefaultSocketPath[] = "/var/lib/phi/@1/cli.sock";

} // namespace

CliTransport::CliTransport(QObject *parent)
    : QObject(parent)
{
}

std::string CliTransport::pluginType() const
{
    return "cli";
}

std::string CliTransport::displayName() const
{
    return "CLI";
}

std::string CliTransport::description() const
{
    return "Unix socket transport plugin for local CLI access.";
}

bool CliTransport::start(std::string_view configJson, std::string *errorString)
{
    // The private helpers below stay in QString; only the contract is Qt-free.
    QString localError;
    const auto reportError = [&]() {
        if (errorString)
            *errorString = localError.toStdString();
        return false;
    };
    // Config arrives as JSON text; parsed once here and used as before.
    const QJsonObject config =
        QJsonDocument::fromJson(QByteArray::fromRawData(configJson.data(),
                                                       static_cast<qsizetype>(configJson.size())))
            .object();
    if (m_running)
        stop();

    const QString socketPath = socketPathFromConfig(config);
    if (socketPath.trimmed().isEmpty()) {
        localError = QStringLiteral("Invalid socketPath");
        return reportError();
    }

    if (!startServer(socketPath, &localError))
        return reportError();

    m_socketPath = socketPath;
    m_running = true;
    const std::string socketPathText = m_socketPath.toStdString();
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "CLI transport started on unix socket %1",
             {Scalar{socketPathText}},
             "cli.start",
             jsonObject({{"socketPath", jsonQuoted(socketPathText)}}));
    return true;
}

void CliTransport::stop()
{
    if (!m_running && !m_server)
        return;

    closeAllClients();
    m_clients.clear();
    m_clientBuffers.clear();
    m_pendingCommands.clear();

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }

    if (!m_socketPath.isEmpty())
        QFile::remove(m_socketPath);

    m_socketPath.clear();
    m_running = false;
}

void CliTransport::onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end()) {
        const std::string cmdIdText = std::to_string(cmdId);
        writeLog(LogLevel::Warn,
                 makeCategory(LogCategory::Transport),
                 "No pending CLI command for cmdId=%1",
                 {Scalar{static_cast<std::int64_t>(cmdId)}},
                 "cli.asyncResultMissing",
                 jsonObject({{"cmdId", cmdIdText}}));
        return;
    }

    const PendingCommand pending = it.value();
    m_pendingCommands.erase(it);

    QLocalSocket *socket = pending.socket.data();
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    sendCmdResponse(socket, pending.cid, pending.cmdTopic, payloadJson);
}

void CliTransport::onCoreEvent(std::string_view topic, std::string_view payloadJson)
{
    const QString topicText = QString::fromUtf8(topic.data(), static_cast<qsizetype>(topic.size()));
    if (topicText.trimmed().isEmpty())
        return;
    broadcastEvent(topic, payloadJson);
}

void CliTransport::onNewConnection()
{
    if (!m_server)
        return;

    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            continue;

        m_clients.insert(socket);
        m_clientBuffers.insert(socket, ClientBuffer{socket, {}});
        connect(socket, &QLocalSocket::disconnected,
                this, &CliTransport::onSocketDisconnected);
        connect(socket, &QLocalSocket::readyRead,
                this, &CliTransport::onSocketReadyRead);
    }
}

void CliTransport::onSocketDisconnected()
{
    auto *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket)
        return;

    m_clients.remove(socket);
    m_clientBuffers.remove(socket);

    for (auto it = m_pendingCommands.begin(); it != m_pendingCommands.end();) {
        if (it.value().socket == socket)
            it = m_pendingCommands.erase(it);
        else
            ++it;
    }

    socket->deleteLater();
}

void CliTransport::onSocketReadyRead()
{
    auto *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket)
        return;

    auto it = m_clientBuffers.find(socket);
    if (it == m_clientBuffers.end())
        return;

    it->buffer.append(socket->readAll());
    while (true) {
        const int newlinePos = it->buffer.indexOf('\n');
        if (newlinePos < 0)
            break;

        const QByteArray line = it->buffer.left(newlinePos).trimmed();
        it->buffer.remove(0, newlinePos + 1);
        if (!line.isEmpty())
            processLine(socket, line);
    }
}

QString CliTransport::socketPathFromConfig(const QJsonObject &config)
{
    const QString configured = config.value(QStringLiteral("socketPath")).toString().trimmed();
    if (!configured.isEmpty())
        return configured;
    return QString::fromLatin1(kDefaultSocketPath);
}

std::optional<CmdId> CliTransport::readCid(const QJsonValue &value)
{
    if (value.isDouble())
        return cidFromNumber(value.toDouble(-1.0));
    if (value.isString())
        return cidFromString(value.toString().toStdString());
    return std::nullopt;
}

bool CliTransport::startServer(const QString &socketPath, QString *errorString)
{
    const QFileInfo pathInfo(socketPath);
    const QString dirPath = pathInfo.absolutePath();
    if (dirPath.isEmpty() || !QDir().mkpath(dirPath)) {
        if (errorString)
            *errorString = QStringLiteral("Failed to create socket directory: %1").arg(dirPath);
        return false;
    }

    QFile::remove(socketPath);

    auto *server = new QLocalServer(this);
    if (!server->listen(socketPath)) {
        const QString err = server->errorString();
        delete server;
        if (errorString)
            *errorString = err.isEmpty() ? QStringLiteral("Failed to listen on unix socket") : err;
        return false;
    }

    QFile::setPermissions(socketPath,
                          QFile::ReadOwner | QFile::WriteOwner
                              | QFile::ReadGroup | QFile::WriteGroup);

    connect(server, &QLocalServer::newConnection,
            this, &CliTransport::onNewConnection);

    m_server = server;
    return true;
}

void CliTransport::closeAllClients()
{
    const QList<QLocalSocket *> clients = m_clients.values();
    for (QLocalSocket *client : clients) {
        if (!client)
            continue;
        client->disconnectFromServer();
        client->close();
        client->deleteLater();
    }
}

// One Qt-side utility remains: turning a QJsonObject this transport built or
// parsed into the text the boundary speaks.
static JsonText jsonTextOf(const QJsonObject &object)
{
    if (object.isEmpty())
        return emptyJsonObject();
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return JsonText(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

void CliTransport::send(QLocalSocket *socket,
                        std::string_view type,
                        std::string_view topic,
                        std::optional<CmdId> cid,
                        std::string_view payloadJson) const
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    // The envelope shape comes from the shared header; the trailing newline is
    // this wire's framing and the only part that differs from the WS transport.
    JsonText out = makeEnvelope(type, topic, cid, payloadJson);
    out += '\n';
    socket->write(out.data(), static_cast<qint64>(out.size()));
    socket->flush();
}

void CliTransport::sendProtocolError(QLocalSocket *socket,
                                     std::optional<CmdId> cid,
                                     std::string_view code,
                                     std::string_view message) const
{
    send(socket, kEnvelopeTypeError, kTopicProtocolError, cid, makeProtocolErrorPayload(code, message));
}

void CliTransport::sendCmdResponse(QLocalSocket *socket,
                                   CmdId cid,
                                   const QString &cmdTopic,
                                   std::string_view payloadJson) const
{
    // The one outbound path that parses: `error: null` is added only if absent, and
    // deciding that from raw text would be a substring guess. Command responses are
    // user-driven, so the parse sits on the cheap side of the trade.
    QJsonObject out =
        QJsonDocument::fromJson(QByteArray::fromRawData(payloadJson.data(),
                                                       static_cast<qsizetype>(payloadJson.size())))
            .object();
    out.insert(QStringLiteral("cmd"), cmdTopic);
    if (!out.contains(QStringLiteral("error")))
        out.insert(QStringLiteral("error"), QJsonValue::Null);
    send(socket, kEnvelopeTypeResponse, kTopicCmdResponse, cid, jsonTextOf(out));
}

void CliTransport::broadcastEvent(std::string_view topic, std::string_view payloadJson) const
{
    // No cid on events; otherwise the same envelope as everything else.
    for (QLocalSocket *client : m_clients)
        send(client, kEnvelopeTypeEvent, topic, std::nullopt, payloadJson);
}

void CliTransport::handleCommand(QLocalSocket *socket,
                                 CmdId cid,
                                 const QString &topic,
                                 std::string_view payloadJson)
{
    // Routing is the protocol's decision, made once in TransportPluginBase - this
    // transport used to carry its own sync fallback for `cmd.*` topics, which the
    // WS transport never had. Same rule for both now, and no fallback: a command
    // core will not take is a rejected command.
    const CommandOutcome outcome = dispatchCommand(topic.toUtf8().toStdString(), payloadJson);

    if (outcome.cmdId > 0) {
        // Core took the command and answers later; the client waits under that id
        // until onCoreAsyncResult arrives.
        PendingCommand pending;
        pending.socket = socket;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.insert(outcome.cmdId, pending);
    }

    const auto [type, envelopeTopic] = envelopeFor(outcome.kind);
    send(socket, type, envelopeTopic, cid, outcome.payloadJson);
}

void CliTransport::processLine(QLocalSocket *socket, const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendProtocolError(socket, std::nullopt, kErrorCodeInvalidJson, kMessageInvalidJson);
        return;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QString topic = obj.value(QStringLiteral("topic")).toString();
    const QJsonObject payload = obj.value(QStringLiteral("payload")).toObject();

    const std::optional<CmdId> cid = readCid(obj.value(QStringLiteral("cid")));
    if (!cid.has_value()) {
        sendProtocolError(socket, std::nullopt, kErrorCodeMissingCid, kMessageMissingCid);
        return;
    }

    if (type.toStdString() != kEnvelopeTypeCmd) {
        sendProtocolError(socket, cid, kErrorCodeInvalidType, kMessageInvalidType);
        return;
    }

    if (topic.trimmed().isEmpty()) {
        sendProtocolError(socket, cid, kErrorCodeMissingTopic, kMessageMissingTopic);
        return;
    }

    // The API takes the payload as text; the frame was parsed to read the envelope, so
    // the sub-object is serialized once here. That is the cost side of the text
    // boundary, and it sits on the command path rather than the event path.
    const JsonText payloadJson = jsonTextOf(payload);
    handleCommand(socket, *cid, topic, payloadJson);
}

} // namespace phicore::transport::cli

PHI_TRANSPORT_PLUGIN(phicore::transport::cli::CliTransport)
