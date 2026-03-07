#pragma once

#include <QHash>
#include <QPointer>
#include <QSet>

#include <optional>

#include <transportinterface.h>

class QLocalServer;
class QLocalSocket;

namespace phicore::transport::cli {

class CliTransport final : public TransportInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PHI_TRANSPORT_INTERFACE_IID)
    Q_INTERFACES(phicore::transport::TransportInterface)

public:
    explicit CliTransport(QObject *parent = nullptr);

    QString pluginType() const override;
    QString displayName() const override;
    QString description() const override;
    QString apiVersion() const override;

    bool start(const QJsonObject &config, QString *errorString) override;
    void stop() override;
    bool reloadConfig(const QJsonObject &config, QString *errorString) override;

protected:
    void onCoreAsyncResult(CmdId cmdId, const QJsonObject &payload) override;
    void onCoreEvent(const QString &topic, const QJsonObject &payload) override;

private slots:
    void onNewConnection();
    void onSocketDisconnected();
    void onSocketReadyRead();

private:
    struct ClientBuffer {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
    };

    struct PendingCommand {
        QPointer<QLocalSocket> socket;
        quint64 cid = 0;
        QString cmdTopic;
    };

    static QString socketPathFromConfig(const QJsonObject &config);
    static bool tryReadCid(const QJsonValue &value, quint64 *cidOut);
    static QJsonObject makeAckPayload(bool accepted,
                                      const QString &cmdTopic,
                                      const QString &errorMsg = QString(),
                                      const QString &errorCode = QStringLiteral("core_error"));

    bool startServer(const QString &socketPath, QString *errorString);
    void closeAllClients();
    void sendEnvelope(QLocalSocket *socket,
                      const QString &type,
                      const QString &topic,
                      quint64 cid,
                      const QJsonObject &payload) const;
    void sendProtocolError(QLocalSocket *socket,
                           std::optional<quint64> cid,
                           const QString &code,
                           const QString &message) const;
    void sendSyncResponse(QLocalSocket *socket,
                          quint64 cid,
                          const QString &syncTopic,
                          const QJsonObject &payload) const;
    void sendAck(QLocalSocket *socket,
                 quint64 cid,
                 bool accepted,
                 const QString &cmdTopic,
                 const QString &errorMsg = QString()) const;
    void sendCmdResponse(QLocalSocket *socket,
                         quint64 cid,
                         const QString &cmdTopic,
                         const QJsonObject &payload) const;
    void sendEvent(QLocalSocket *socket,
                   const QString &topic,
                   const QJsonObject &payload) const;
    void broadcastEvent(const QString &topic, const QJsonObject &payload) const;
    void handleCommand(QLocalSocket *socket,
                       quint64 cid,
                       const QString &topic,
                       const QJsonObject &payload);
    void processLine(QLocalSocket *socket, const QByteArray &line);

    bool m_running = false;
    QString m_socketPath;
    QLocalServer *m_server = nullptr;
    QSet<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, ClientBuffer> m_clientBuffers;
    QHash<CmdId, PendingCommand> m_pendingCommands;
};

} // namespace phicore::transport::cli
