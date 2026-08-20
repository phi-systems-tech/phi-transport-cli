#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <optional>
#include <string>
#include <string_view>

#include <transportinterface.h>

class QLocalServer;
class QLocalSocket;

namespace phicore::transport::cli {

// QObject first, as Qt requires for multiple inheritance. The transport
// contract itself is Qt-free; this plugin uses Qt for its own I/O, which is its
// business rather than the contract's.
class CliTransport final : public QObject, public TransportPluginBase
{
    Q_OBJECT

public:
    explicit CliTransport(QObject *parent = nullptr);

    std::string pluginType() const override;
    std::string displayName() const override;
    std::string description() const override;

    bool start(std::string_view configJson, std::string *errorString) override;
    void stop() override;

protected:
    void onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson) override;
    void onCoreEvent(std::string_view topic, std::string_view payloadJson) override;

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
    // Which JSON shapes a cid may arrive in; what counts as a valid one is the
    // protocol's answer and lives in the shared header.
    static std::optional<CmdId> readCid(const QJsonValue &value);

    bool startServer(const QString &socketPath, QString *errorString);
    void closeAllClients();
    // The one outbound primitive. Envelope and payload shapes come from
    // envelope.h; the newline that frames them on this wire is this transport's
    // own business.
    void send(QLocalSocket *socket,
              std::string_view type,
              std::string_view topic,
              std::optional<CmdId> cid,
              std::string_view payloadJson) const;
    void sendProtocolError(QLocalSocket *socket,
                           std::optional<CmdId> cid,
                           std::string_view code,
                           std::string_view message) const;
    void sendCmdResponse(QLocalSocket *socket,
                         CmdId cid,
                         const QString &cmdTopic,
                         std::string_view payloadJson) const;
    void broadcastEvent(std::string_view topic, std::string_view payloadJson) const;
    void handleCommand(QLocalSocket *socket,
                       CmdId cid,
                       const QString &topic,
                       std::string_view payloadJson);
    void processLine(QLocalSocket *socket, const QByteArray &line);

    bool m_running = false;
    QString m_socketPath;
    QLocalServer *m_server = nullptr;
    QSet<QLocalSocket *> m_clients;
    QHash<QLocalSocket *, ClientBuffer> m_clientBuffers;
    QHash<CmdId, PendingCommand> m_pendingCommands;
};

} // namespace phicore::transport::cli
