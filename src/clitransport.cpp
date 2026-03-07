#include "clitransport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(cliTransportLog, "phi-transport.cli")

namespace phicore::transport::cli {

namespace {

constexpr const char kTypeEvent[] = "event";
constexpr const char kTypeCmd[] = "cmd";
constexpr const char kTypeResponse[] = "response";
constexpr const char kTypeError[] = "error";

constexpr const char kTopicCmdAck[] = "cmd.ack";
constexpr const char kTopicCmdResponse[] = "cmd.response";
constexpr const char kTopicSyncResponse[] = "sync.response";
constexpr const char kTopicProtocolError[] = "protocol.error";

constexpr const char kDefaultSocketPath[] = "/var/lib/phi/cli.sock";

} // namespace

CliTransport::CliTransport(QObject *parent)
    : TransportInterface(parent)
{
}

QString CliTransport::pluginType() const
{
    return QStringLiteral("cli");
}

QString CliTransport::displayName() const
{
    return QStringLiteral("CLI");
}

QString CliTransport::description() const
{
    return QStringLiteral("Unix socket transport plugin for local CLI access.");
}

QString CliTransport::apiVersion() const
{
    return QStringLiteral("1.0.0");
}

bool CliTransport::start(const QJsonObject &config, QString *errorString)
{
    if (m_running)
        stop();

    const QString socketPath = socketPathFromConfig(config);
    if (socketPath.trimmed().isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("Invalid socketPath");
        return false;
    }

    if (!startServer(socketPath, errorString))
        return false;

    m_socketPath = socketPath;
    m_running = true;
    qCInfo(cliTransportLog).noquote()
        << tr("CLI transport started on unix socket %1").arg(m_socketPath);
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

bool CliTransport::reloadConfig(const QJsonObject &config, QString *errorString)
{
    if (!m_running)
        return start(config, errorString);

    stop();
    return start(config, errorString);
}

void CliTransport::onCoreAsyncResult(CmdId cmdId, const QJsonObject &payload)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end())
        return;

    const PendingCommand pending = it.value();
    m_pendingCommands.erase(it);

    QLocalSocket *socket = pending.socket.data();
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    sendCmdResponse(socket, pending.cid, pending.cmdTopic, payload);
}

void CliTransport::onCoreEvent(const QString &topic, const QJsonObject &payload)
{
    if (topic.trimmed().isEmpty())
        return;
    broadcastEvent(topic, payload);
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

bool CliTransport::tryReadCid(const QJsonValue &value, quint64 *cidOut)
{
    if (!cidOut)
        return false;

    if (value.isDouble()) {
        const double raw = value.toDouble(-1.0);
        if (raw < 0.0)
            return false;
        *cidOut = static_cast<quint64>(raw);
        return true;
    }

    if (value.isString()) {
        bool ok = false;
        const quint64 parsed = value.toString().toULongLong(&ok);
        if (!ok)
            return false;
        *cidOut = parsed;
        return true;
    }

    return false;
}

QJsonObject CliTransport::makeAckPayload(bool accepted,
                                         const QString &cmdTopic,
                                         const QString &errorMsg,
                                         const QString &errorCode)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("accepted"), accepted);
    payload.insert(QStringLiteral("cmd"), cmdTopic);
    if (errorMsg.isEmpty()) {
        payload.insert(QStringLiteral("error"), QJsonValue::Null);
    } else {
        QJsonObject err;
        err.insert(QStringLiteral("code"), errorCode);
        err.insert(QStringLiteral("msg"), errorMsg);
        payload.insert(QStringLiteral("error"), err);
    }
    return payload;
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

void CliTransport::sendEnvelope(QLocalSocket *socket,
                                const QString &type,
                                const QString &topic,
                                quint64 cid,
                                const QJsonObject &payload) const
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    QJsonObject env;
    env.insert(QStringLiteral("type"), type);
    env.insert(QStringLiteral("topic"), topic);
    env.insert(QStringLiteral("cid"), static_cast<qint64>(cid));
    env.insert(QStringLiteral("payload"), payload);
    const QByteArray data = QJsonDocument(env).toJson(QJsonDocument::Compact) + '\n';
    socket->write(data);
    socket->flush();
}

void CliTransport::sendProtocolError(QLocalSocket *socket,
                                     std::optional<quint64> cid,
                                     const QString &code,
                                     const QString &message) const
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    QJsonObject payload;
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("message"), message);

    QJsonObject env;
    env.insert(QStringLiteral("type"), QString::fromLatin1(kTypeError));
    env.insert(QStringLiteral("topic"), QString::fromLatin1(kTopicProtocolError));
    if (cid.has_value())
        env.insert(QStringLiteral("cid"), static_cast<qint64>(*cid));
    env.insert(QStringLiteral("payload"), payload);
    const QByteArray data = QJsonDocument(env).toJson(QJsonDocument::Compact) + '\n';
    socket->write(data);
    socket->flush();
}

void CliTransport::sendSyncResponse(QLocalSocket *socket,
                                    quint64 cid,
                                    const QString &syncTopic,
                                    const QJsonObject &payload) const
{
    QJsonObject out = payload;
    out.insert(QStringLiteral("sync"), syncTopic);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicSyncResponse), cid, out);
}

void CliTransport::sendAck(QLocalSocket *socket,
                           quint64 cid,
                           bool accepted,
                           const QString &cmdTopic,
                           const QString &errorMsg) const
{
    const QJsonObject payload = makeAckPayload(accepted, cmdTopic, errorMsg);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicCmdAck), cid, payload);
}

void CliTransport::sendCmdResponse(QLocalSocket *socket,
                                   quint64 cid,
                                   const QString &cmdTopic,
                                   const QJsonObject &payload) const
{
    QJsonObject out = payload;
    out.insert(QStringLiteral("cmd"), cmdTopic);
    if (!out.contains(QStringLiteral("error")))
        out.insert(QStringLiteral("error"), QJsonValue::Null);
    sendEnvelope(socket, QString::fromLatin1(kTypeResponse), QString::fromLatin1(kTopicCmdResponse), cid, out);
}

void CliTransport::sendEvent(QLocalSocket *socket,
                             const QString &topic,
                             const QJsonObject &payload) const
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;

    QJsonObject env;
    env.insert(QStringLiteral("type"), QString::fromLatin1(kTypeEvent));
    env.insert(QStringLiteral("topic"), topic);
    env.insert(QStringLiteral("payload"), payload);
    const QByteArray data = QJsonDocument(env).toJson(QJsonDocument::Compact) + '\n';
    socket->write(data);
    socket->flush();
}

void CliTransport::broadcastEvent(const QString &topic, const QJsonObject &payload) const
{
    for (QLocalSocket *client : m_clients)
        sendEvent(client, topic, payload);
}

void CliTransport::handleCommand(QLocalSocket *socket,
                                 quint64 cid,
                                 const QString &topic,
                                 const QJsonObject &payload)
{
    if (topic.startsWith(QStringLiteral("sync."))) {
        const SyncResult result = callCoreSync(topic, payload);
        if (result.accepted) {
            sendSyncResponse(socket, cid, topic, result.payload);
        } else {
            const QString err = result.error.has_value() ? result.error->msg : QStringLiteral("Sync call rejected");
            QJsonObject out;
            out.insert(QStringLiteral("accepted"), false);
            QJsonObject errObj;
            errObj.insert(QStringLiteral("msg"), err);
            if (result.error.has_value() && !result.error->ctx.isEmpty())
                errObj.insert(QStringLiteral("ctx"), result.error->ctx);
            out.insert(QStringLiteral("error"), errObj);
            sendSyncResponse(socket, cid, topic, out);
        }
        return;
    }

    if (!topic.startsWith(QStringLiteral("cmd."))) {
        sendProtocolError(socket, cid, QStringLiteral("unknown_topic"),
                          QStringLiteral("Unknown command topic: %1").arg(topic));
        return;
    }

    const AsyncResult asyncSubmit = callCoreAsync(topic, payload);
    if (asyncSubmit.accepted && asyncSubmit.cmdId > 0) {
        PendingCommand pending;
        pending.socket = socket;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.insert(asyncSubmit.cmdId, pending);
        sendAck(socket, cid, true, topic);
        return;
    }

    const SyncResult syncResult = callCoreSync(topic, payload);
    if (syncResult.accepted) {
        sendAck(socket, cid, true, topic);
        sendCmdResponse(socket, cid, topic, syncResult.payload);
        return;
    }

    const bool unknownTopic =
        asyncSubmit.error.has_value()
        && syncResult.error.has_value()
        && asyncSubmit.error->msg == QStringLiteral("Unsupported async topic")
        && syncResult.error->msg == QStringLiteral("Unsupported sync topic");

    if (unknownTopic) {
        sendProtocolError(socket, cid, QStringLiteral("unknown_topic"),
                          QStringLiteral("Unknown command topic: %1").arg(topic));
        return;
    }

    const QString errorMsg =
        syncResult.error.has_value() && !syncResult.error->msg.isEmpty()
            ? syncResult.error->msg
            : (asyncSubmit.error.has_value() && !asyncSubmit.error->msg.isEmpty()
                   ? asyncSubmit.error->msg
                   : QStringLiteral("Command rejected"));
    sendAck(socket, cid, false, topic, errorMsg);
}

void CliTransport::processLine(QLocalSocket *socket, const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        sendProtocolError(socket, std::nullopt, QStringLiteral("invalid_json"),
                          QStringLiteral("Payload must be a valid JSON object."));
        return;
    }

    const QJsonObject obj = doc.object();
    const QString type = obj.value(QStringLiteral("type")).toString();
    const QString topic = obj.value(QStringLiteral("topic")).toString();
    const QJsonValue cidValue = obj.value(QStringLiteral("cid"));
    const QJsonObject payload = obj.value(QStringLiteral("payload")).toObject();

    quint64 cid = 0;
    if (!tryReadCid(cidValue, &cid)) {
        sendProtocolError(socket, std::nullopt, QStringLiteral("missing_cid"),
                          QStringLiteral("Commands must include numeric 'cid'."));
        return;
    }

    if (type != QLatin1String(kTypeCmd)) {
        sendProtocolError(socket, cid, QStringLiteral("invalid_type"),
                          QStringLiteral("Only messages with type='cmd' are supported."));
        return;
    }

    if (topic.trimmed().isEmpty()) {
        sendProtocolError(socket, cid, QStringLiteral("missing_topic"),
                          QStringLiteral("Missing command topic."));
        return;
    }

    handleCommand(socket, cid, topic, payload);
}

} // namespace phicore::transport::cli
