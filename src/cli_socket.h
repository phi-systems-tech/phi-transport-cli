#pragma once

// The line protocol the CLI speaks over the tenant's Unix socket, blocking.
//
// What QLocalSocket's waitForConnected/waitForBytesWritten/waitForReadyRead
// trio was here for. A CLI waits for its answer and then exits, so blocking is
// the right shape - but the waiting has to be bounded, which is what poll() is
// for, and the descriptor stays non-blocking so a half-written request cannot
// park the process forever.

#include <cstddef>
#include <string>
#include <string_view>

namespace phicore::cli {

class LineSocket
{
public:
    /// What a read attempt produced. `Closed` is the peer's FIN, which for this
    /// protocol means the answer is never coming.
    enum class Read { Line, Timeout, Closed, Failed };

    LineSocket() = default;
    ~LineSocket();

    LineSocket(const LineSocket &) = delete;
    LineSocket &operator=(const LineSocket &) = delete;

    bool connect(const std::string &path, int timeoutMs, std::string *error);
    bool writeAll(std::string_view bytes, int timeoutMs, std::string *error);
    /// One complete line without its newline. Lines already in the buffer are
    /// returned before the socket is polled again.
    Read readLine(int timeoutMs, std::string *line, std::string *error);
    void close();

private:
    bool waitFor(short events, int timeoutMs, std::string *error);
    bool takeBufferedLine(std::string *line);

    int m_fd = -1;
    std::string m_buffer;
};

} // namespace phicore::cli
