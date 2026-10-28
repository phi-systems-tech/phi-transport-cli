#pragma once

#include <transportinterface.h>

#include <phi/runtime/loop.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace phicore::transport::cli {

// The CLI transport on the runtime loop (contract 2.0.0): a unix-socket
// server whose listen and client descriptors are watches on the loop core
// hands the plugin. No Qt and no thread of its own - the loop's thread is
// where everything here runs, including the contract callbacks.
class CliTransport final : public TransportPluginBase
{
public:
    CliTransport() = default;
    ~CliTransport() override;

    std::string pluginType() const override;
    std::string displayName() const override;
    std::string description() const override;

    bool start(std::string_view configJson, std::string *errorString) override;
    void stop() override;

protected:
    void onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson) override;
    void onCoreEvent(std::string_view topic, std::string_view payloadJson) override;

private:
    // One connected client. Destroyed only through a deferred post: a watch
    // must never be reset from inside its own firing callback, so teardown
    // happens on the next loop turn while a `closed` flag quiets this one.
    struct Client {
        int fd = -1;
        bool closed = false;
        /// The client sent its FIN: nothing more comes in, but what it asked
        /// for still goes out before the socket is closed.
        bool peerClosed = false;
        phi::runtime::Timer lingerDeadline;
        phi::runtime::FdWatch readWatch;
        phi::runtime::FdWatch writeWatch; // enabled only while outBuffer holds bytes
        std::string inBuffer;
        std::string outBuffer;
    };

    struct PendingCommand {
        int clientFd = -1;
        CmdId cid = 0;
        std::string cmdTopic;
    };

    void acceptClients();
    void onClientReadable(Client *client);
    void onClientWritable(Client *client);
    void processLine(Client *client, std::string_view line);
    void handleCommand(Client *client, CmdId cid, const std::string &topic, std::string_view payloadJson);

    void send(Client *client,
              std::string_view type,
              std::string_view topic,
              std::optional<CmdId> cid,
              std::string_view payloadJson);
    void sendProtocolError(Client *client, std::optional<CmdId> cid,
                           std::string_view code, std::string_view message);
    void sendCmdResponse(Client *client, CmdId cid,
                         const std::string &cmdTopic, std::string_view payloadJson);
    void broadcastEvent(std::string_view topic, std::string_view payloadJson);

    void flushClient(Client *client);
    void dropClient(int fd);
    /// Closes a client whose peer is gone once nothing it asked for is left.
    void closeIfDone(Client *client);
    bool hasPendingCommands(int fd) const;
    void closeListener();

    int m_listenFd = -1;
    phi::runtime::FdWatch m_listenWatch;
    std::string m_socketPath;
    bool m_running = false;

    std::map<int, std::shared_ptr<Client>> m_clients;   // key: fd
    std::map<CmdId, PendingCommand> m_pendingCommands;  // key: core cmdId
};

} // namespace phicore::transport::cli
