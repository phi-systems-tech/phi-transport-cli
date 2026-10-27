#include "clitransport.h"

#include <commandframe.h>

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <variant>

namespace phicore::transport::cli {

namespace {

using Json = nlohmann::json;

// Envelope types and the topics a transport produces itself come from
// envelope.h: they are protocol surface, and a copy per transport is how two
// wires drift apart.
constexpr const char kDefaultSocketPath[] = "/var/lib/phi/@1/cli.sock";

/// A line a client may grow before it is a protocol violation, not a command.
constexpr std::size_t kMaxLineBytes = 4 * 1024 * 1024;

std::string socketPathFromConfig(const Json &config)
{
    if (config.is_object()) {
        const auto it = config.find("socketPath");
        if (it != config.end() && it->is_string()) {
            std::string configured = it->get<std::string>();
            while (!configured.empty() && (configured.back() == ' ' || configured.back() == '\t'))
                configured.pop_back();
            if (!configured.empty())
                return configured;
        }
    }
    return kDefaultSocketPath;
}

} // namespace

CliTransport::~CliTransport()
{
    stop();
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
    const auto fail = [&](std::string message) {
        if (errorString)
            *errorString = std::move(message);
        return false;
    };

    const Json config = Json::parse(configJson, nullptr, false);
    if (m_running)
        stop();

    const std::string socketPath = socketPathFromConfig(config);
    if (socketPath.empty())
        return fail("Invalid socketPath");
    if (socketPath.size() >= sizeof(sockaddr_un{}.sun_path))
        return fail("socketPath is longer than a unix socket path may be");

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(socketPath).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return fail("Failed to create socket directory: " + dir.string());
    }
    ::unlink(socketPath.c_str());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail(std::string("socket() failed: ") + std::strerror(errno));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        const int err = errno;
        ::close(fd);
        return fail(std::string("bind() failed: ") + std::strerror(err));
    }
    // The socket file's permissions are the credential: owner and group only.
    ::chmod(socketPath.c_str(), 0660);
    if (::listen(fd, 16) < 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(socketPath.c_str());
        return fail(std::string("listen() failed: ") + std::strerror(err));
    }

    m_listenFd = fd;
    m_socketPath = socketPath;
    m_listenWatch = runtimeLoop()->watchFd(m_listenFd, phi::runtime::Loop::FdEvent::Read,
                                           [this]() { acceptClients(); });
    m_running = true;
    writeLog(LogLevel::Info,
             makeCategory(LogCategory::Transport),
             "CLI transport started on unix socket %1",
             {Scalar{m_socketPath}},
             "cli.start",
             jsonObject({{"socketPath", jsonQuoted(m_socketPath)}}));
    return true;
}

void CliTransport::stop()
{
    if (!m_running && m_listenFd < 0)
        return;

    // stop() is a contract call on the loop's own thread and may run inside
    // no watch callback of these clients, so the teardown is direct here.
    for (auto &entry : m_clients) {
        Client *client = entry.second.get();
        client->closed = true;
        client->readWatch.reset();
        client->writeWatch.reset();
        ::close(client->fd);
    }
    m_clients.clear();
    m_pendingCommands.clear();
    closeListener();
    m_running = false;
}

void CliTransport::closeListener()
{
    m_listenWatch.reset();
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    if (!m_socketPath.empty())
        ::unlink(m_socketPath.c_str());
    m_socketPath.clear();
}

void CliTransport::acceptClients()
{
    while (true) {
        const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            return; // transient accept errors: the watch will fire again
        }

        auto client = std::make_shared<Client>();
        client->fd = fd;
        Client *raw = client.get();
        client->readWatch = runtimeLoop()->watchFd(fd, phi::runtime::Loop::FdEvent::Read,
                                                   [this, raw]() { onClientReadable(raw); });
        client->writeWatch = runtimeLoop()->watchFd(fd, phi::runtime::Loop::FdEvent::Write,
                                                    [this, raw]() { onClientWritable(raw); });
        client->writeWatch.setEnabled(false);
        m_clients.emplace(fd, std::move(client));
    }
}

void CliTransport::onClientReadable(Client *client)
{
    if (client->closed)
        return;

    char chunk[16 * 1024];
    while (true) {
        const ssize_t got = ::read(client->fd, chunk, sizeof(chunk));
        if (got > 0) {
            client->inBuffer.append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (got < 0 && errno == EINTR)
            continue;
        dropClient(client->fd); // closed by the peer, or a read error
        return;
    }

    std::size_t start = 0;
    while (!client->closed) {
        const std::size_t newline = client->inBuffer.find('\n', start);
        if (newline == std::string::npos)
            break;
        std::string_view line(client->inBuffer.data() + start, newline - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' '))
            line.remove_prefix(1);
        if (!line.empty())
            processLine(client, line);
        start = newline + 1;
    }
    client->inBuffer.erase(0, start);
    if (client->inBuffer.size() > kMaxLineBytes)
        dropClient(client->fd);
}

void CliTransport::onClientWritable(Client *client)
{
    if (client->closed)
        return;
    flushClient(client);
}

void CliTransport::flushClient(Client *client)
{
    while (!client->outBuffer.empty()) {
        const ssize_t sent = ::write(client->fd, client->outBuffer.data(), client->outBuffer.size());
        if (sent > 0) {
            client->outBuffer.erase(0, static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            client->writeWatch.setEnabled(true);
            return;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        dropClient(client->fd);
        return;
    }
    client->writeWatch.setEnabled(false);
}

void CliTransport::dropClient(int fd)
{
    auto it = m_clients.find(fd);
    if (it == m_clients.end())
        return;
    std::shared_ptr<Client> victim = std::move(it->second);
    m_clients.erase(it);
    victim->closed = true;

    for (auto pendingIt = m_pendingCommands.begin(); pendingIt != m_pendingCommands.end();) {
        if (pendingIt->second.clientFd == fd)
            pendingIt = m_pendingCommands.erase(pendingIt);
        else
            ++pendingIt;
    }

    // The watches die on the next loop turn: this may run inside one of their
    // own callbacks, and a watch must not be reset from inside itself.
    runtimeLoop()->post([victim]() {
        victim->readWatch.reset();
        victim->writeWatch.reset();
        ::close(victim->fd);
    });
}

void CliTransport::onCoreAsyncResult(CmdId cmdId, std::string_view payloadJson)
{
    auto it = m_pendingCommands.find(cmdId);
    if (it == m_pendingCommands.end()) {
        writeLog(LogLevel::Warn,
                 makeCategory(LogCategory::Transport),
                 "No pending CLI command for cmdId=%1",
                 {Scalar{static_cast<std::int64_t>(cmdId)}},
                 "cli.asyncResultMissing",
                 jsonObject({{"cmdId", std::to_string(cmdId)}}));
        return;
    }

    const PendingCommand pending = it->second;
    m_pendingCommands.erase(it);

    auto clientIt = m_clients.find(pending.clientFd);
    if (clientIt == m_clients.end())
        return;
    sendCmdResponse(clientIt->second.get(), pending.cid, pending.cmdTopic, payloadJson);
}

void CliTransport::onCoreEvent(std::string_view topic, std::string_view payloadJson)
{
    if (topic.empty())
        return;
    broadcastEvent(topic, payloadJson);
}

void CliTransport::send(Client *client,
                        std::string_view type,
                        std::string_view topic,
                        std::optional<CmdId> cid,
                        std::string_view payloadJson)
{
    if (!client || client->closed)
        return;

    // The envelope shape comes from the shared header; the trailing newline is
    // this wire's framing and the only part that differs from the WS transport.
    JsonText out = makeEnvelope(type, topic, cid, payloadJson);
    out += '\n';
    client->outBuffer += out;
    flushClient(client);
}

void CliTransport::sendProtocolError(Client *client,
                                     std::optional<CmdId> cid,
                                     std::string_view code,
                                     std::string_view message)
{
    send(client, kEnvelopeTypeError, kTopicProtocolError, cid, makeProtocolErrorPayload(code, message));
}

void CliTransport::sendCmdResponse(Client *client,
                                   CmdId cid,
                                   const std::string &cmdTopic,
                                   std::string_view payloadJson)
{
    const std::string bytes = makeCmdResponsePayload(cmdTopic, payloadJson);
    send(client, kEnvelopeTypeResponse, kTopicCmdResponse, cid, bytes);
}

void CliTransport::broadcastEvent(std::string_view topic, std::string_view payloadJson)
{
    // No cid on events; otherwise the same envelope as everything else. The
    // client list may shrink underneath (a send failure drops the client), so
    // the iteration works from a snapshot of the keys.
    std::vector<int> fds;
    fds.reserve(m_clients.size());
    for (const auto &entry : m_clients)
        fds.push_back(entry.first);
    for (int fd : fds) {
        auto it = m_clients.find(fd);
        if (it != m_clients.end())
            send(it->second.get(), kEnvelopeTypeEvent, topic, std::nullopt, payloadJson);
    }
}

void CliTransport::handleCommand(Client *client,
                                 CmdId cid,
                                 const std::string &topic,
                                 std::string_view payloadJson)
{
    // Routing is the protocol's decision, made once in TransportPluginBase - this
    // transport used to carry its own sync fallback for `cmd.*` topics, which the
    // WS transport never had. Same rule for both now, and no fallback: a command
    // core will not take is a rejected command.
    //
    // The identity is TrustedLocal: this transport listens on a unix socket whose
    // permissions are the credential, so whoever reached it is already privileged
    // on the box. Core is told that rather than left to guess (F-60).
    CallerIdentity caller;
    caller.kind = CallerIdentity::Kind::TrustedLocal;
    const int clientFd = client->fd;
    const CommandOutcome outcome = dispatchCommand(topic, payloadJson, caller);

    // dispatchCommand can block on core; the client may have died meanwhile.
    auto clientIt = m_clients.find(clientFd);
    if (clientIt == m_clients.end())
        return;
    client = clientIt->second.get();

    if (outcome.cmdId > 0) {
        // Core took the command and answers later; the client waits under that id
        // until onCoreAsyncResult arrives.
        PendingCommand pending;
        pending.clientFd = clientFd;
        pending.cid = cid;
        pending.cmdTopic = topic;
        m_pendingCommands.emplace(outcome.cmdId, pending);
    }

    const auto [type, envelopeTopic] = envelopeFor(outcome.kind);
    send(client, type, envelopeTopic, cid, outcome.payloadJson);
}

void CliTransport::processLine(Client *client, std::string_view line)
{
    auto parsed = parseCommandFrame(line);
    if (const auto *error = std::get_if<FrameError>(&parsed)) {
        sendProtocolError(client, error->cid, error->code, error->message);
        return;
    }
    const CommandFrame &frame = std::get<CommandFrame>(parsed);

    // The API takes the payload as text; the frame was parsed to read the envelope, so
    // the sub-object is serialized once here. That is the cost side of the text
    // boundary, and it sits on the command path rather than the event path.
    handleCommand(client, frame.cid, frame.topic, frame.payload.dump());
}

} // namespace phicore::transport::cli

PHI_TRANSPORT_PLUGIN(phicore::transport::cli::CliTransport)
