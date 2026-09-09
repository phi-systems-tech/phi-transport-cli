#include "cli_socket.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace phicore::cli {

namespace {

constexpr std::size_t kReadChunk = 16 * 1024;

std::string errnoText(int code)
{
    return std::strerror(code);
}

} // namespace

LineSocket::~LineSocket()
{
    close();
}

bool LineSocket::connect(const std::string &path, int timeoutMs, std::string *error)
{
    const auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };

    ::sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
        return fail("Socket path is too long: " + path);
    std::memcpy(addr.sun_path, path.data(), path.size());

    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_fd < 0)
        return fail("socket(): " + errnoText(errno));

    const auto length = static_cast<::socklen_t>(offsetof(::sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(m_fd, reinterpret_cast<const ::sockaddr *>(&addr), length) == 0)
        return true;
    if (errno != EINPROGRESS) {
        close();
        return fail("Failed to connect to socket: " + path);
    }

    if (!waitFor(POLLOUT, timeoutMs, nullptr)) {
        close();
        return fail("Failed to connect to socket: " + path);
    }
    int soError = 0;
    ::socklen_t soLength = sizeof(soError);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &soLength) != 0)
        soError = errno;
    if (soError != 0) {
        close();
        return fail("Failed to connect to socket: " + path);
    }
    return true;
}

bool LineSocket::writeAll(std::string_view bytes, int timeoutMs, std::string *error)
{
    while (!bytes.empty()) {
        const ssize_t sent = ::send(m_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!waitFor(POLLOUT, timeoutMs, error))
                return false;
            continue;
        }
        if (error)
            *error = "Failed to send request";
        return false;
    }
    return true;
}

LineSocket::Read LineSocket::readLine(int timeoutMs, std::string *line, std::string *error)
{
    if (takeBufferedLine(line))
        return Read::Line;

    std::vector<char> chunk(kReadChunk);
    while (true) {
        const ssize_t got = ::read(m_fd, chunk.data(), chunk.size());
        if (got > 0) {
            m_buffer.append(chunk.data(), static_cast<std::size_t>(got));
            if (takeBufferedLine(line))
                return Read::Line;
            continue;
        }
        if (got == 0)
            return Read::Closed;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!waitFor(POLLIN, timeoutMs, error))
                return Read::Timeout;
            continue;
        }
        if (error)
            *error = "read(): " + errnoText(errno);
        return Read::Failed;
    }
}

bool LineSocket::takeBufferedLine(std::string *line)
{
    const std::size_t newline = m_buffer.find('\n');
    if (newline == std::string::npos)
        return false;
    if (line)
        *line = m_buffer.substr(0, newline);
    m_buffer.erase(0, newline + 1);
    return true;
}

bool LineSocket::waitFor(short events, int timeoutMs, std::string *error)
{
    ::pollfd pfd{};
    pfd.fd = m_fd;
    pfd.events = events;
    while (true) {
        const int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc > 0)
            return true;
        if (rc == 0) {
            if (error)
                *error = "Timeout waiting for response";
            return false;
        }
        if (errno == EINTR)
            continue;
        if (error)
            *error = "poll(): " + errnoText(errno);
        return false;
    }
}

void LineSocket::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
}

} // namespace phicore::cli
