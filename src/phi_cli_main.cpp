#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QTextStream>

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr const char kDefaultTenant[] = "1";
constexpr int kConnectTimeoutMs = 2000;
constexpr int kWriteTimeoutMs = 2000;
constexpr int kResponseTimeoutMs = 15000;
constexpr int kDiscoveryTimeoutMs = 30000;

bool isValidTenant(const QString &tenant)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return pattern.match(tenant).hasMatch();
}

QString defaultSocketPathForTenant(const QString &tenant)
{
    return QDir(QStringLiteral("/var/lib/phi")).filePath(QStringLiteral("@%1/cli.sock").arg(tenant));
}

void printUsage()
{
    QTextStream out(stdout);
    out << "Usage:\n";
    out << "  phi-cli adapter list [--tenant <tenant>] [--socket <path>] [--json]\n";
    out << "  phi-cli adapter discover [<plugin>] [--tenant <tenant>] [--socket <path>] [--json]\n";
    out << "  phi-cli adapter start|stop|restart (--id <id> | --external-id <id> | --name <name>) [--tenant <tenant>] [--socket <path>]\n";
    out << "  phi-cli adapter start|stop|restart <plugin> --all [--tenant <tenant>] [--socket <path>]\n";
    out << "  phi-cli adapter reload <plugin> [--tenant <tenant>] [--socket <path>]\n";
    out << "  phi-cli transport start|stop|restart|reload <plugin> [--tenant <tenant>] [--socket <path>]\n";
}

bool tryReadCid(const QJsonValue &value, quint64 *cidOut)
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

QString boolText(bool value)
{
    return value ? QStringLiteral("yes") : QStringLiteral("no");
}

void printAdapterTable(const QJsonArray &adapters)
{
    QTextStream out(stdout);
    out.setFieldAlignment(QTextStream::AlignLeft);
    out.setFieldWidth(8);
    out << "ID";
    out.setFieldWidth(16);
    out << "PLUGIN";
    out.setFieldWidth(20);
    out << "EXTERNAL_ID";
    out.setFieldWidth(28);
    out << "NAME";
    out.setFieldWidth(10);
    out << "CONNECTED";
    out.setFieldWidth(0);
    out << "\n";

    for (const QJsonValue &entry : adapters) {
        const QJsonObject obj = entry.toObject();
        const QString id = obj.value(QStringLiteral("id")).toVariant().toString();
        const QString pluginType = obj.value(QStringLiteral("pluginType")).toString();
        const QString externalId = obj.value(QStringLiteral("externalId")).toString();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const QString connected = boolText(obj.value(QStringLiteral("connected")).toBool(false));

        out.setFieldWidth(8);
        out << id;
        out.setFieldWidth(16);
        out << pluginType;
        out.setFieldWidth(20);
        out << externalId;
        out.setFieldWidth(28);
        out << name;
        out.setFieldWidth(10);
        out << connected;
        out.setFieldWidth(0);
        out << "\n";
    }
}

void printDiscoveryTable(const QJsonArray &candidates)
{
    QTextStream out(stdout);
    out.setFieldAlignment(QTextStream::AlignLeft);
    out.setFieldWidth(16);
    out << "PLUGIN";
    out.setFieldWidth(28);
    out << "LABEL";
    out.setFieldWidth(22);
    out << "DISCOVERED_ID";
    out.setFieldWidth(24);
    out << "HOST";
    out.setFieldWidth(18);
    out << "IP";
    out.setFieldWidth(8);
    out << "PORT";
    out.setFieldWidth(12);
    out << "KIND";
    out.setFieldWidth(0);
    out << "\n";

    for (const QJsonValue &entry : candidates) {
        const QJsonObject obj = entry.toObject();
        const QString pluginType = obj.value(QStringLiteral("pluginType")).toString();
        const QString label = obj.value(QStringLiteral("label")).toString();
        const QString discoveredId = obj.value(QStringLiteral("discoveredExternalId")).toString();
        const QString host = obj.value(QStringLiteral("hostname")).toString();
        const QString ip = obj.value(QStringLiteral("ip")).toString();
        const QString port = obj.value(QStringLiteral("port")).toVariant().toString();
        const QString kind = obj.value(QStringLiteral("kind")).toString();

        out.setFieldWidth(16);
        out << pluginType;
        out.setFieldWidth(28);
        out << label;
        out.setFieldWidth(22);
        out << discoveredId;
        out.setFieldWidth(24);
        out << host;
        out.setFieldWidth(18);
        out << ip;
        out.setFieldWidth(8);
        out << port;
        out.setFieldWidth(12);
        out << kind;
        out.setFieldWidth(0);
        out << "\n";
    }
}

QString responseErrorMessage(const QJsonObject &payload)
{
    const QJsonValue errorValue = payload.value(QStringLiteral("error"));
    if (errorValue.isObject()) {
        const QJsonObject errObj = errorValue.toObject();
        const QString msg = errObj.value(QStringLiteral("msg")).toString();
        if (!msg.isEmpty())
            return msg;
        return errObj.value(QStringLiteral("message")).toString();
    }
    if (errorValue.isString())
        return errorValue.toString();
    return {};
}

bool sendCommand(const QString &socketPath,
                 const QString &topic,
                 const QJsonObject &payload,
                 QJsonObject *responsePayload,
                 QString *errorOut)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to connect to socket: %1").arg(socketPath);
        return false;
    }

    const quint64 cid = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    QJsonObject request;
    request.insert(QStringLiteral("type"), QStringLiteral("cmd"));
    request.insert(QStringLiteral("cid"), static_cast<qint64>(cid));
    request.insert(QStringLiteral("topic"), topic);
    request.insert(QStringLiteral("payload"), payload);

    const QByteArray wire = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    socket.write(wire);
    if (!socket.waitForBytesWritten(kWriteTimeoutMs)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to send request");
        return false;
    }

    QByteArray buffer;
    bool ackSeen = false;
    while (true) {
        if (!socket.waitForReadyRead(kResponseTimeoutMs)) {
            if (errorOut)
                *errorOut = QStringLiteral("Timeout waiting for response");
            return false;
        }
        buffer.append(socket.readAll());

        while (true) {
            const int newlinePos = buffer.indexOf('\n');
            if (newlinePos < 0)
                break;
            const QByteArray line = buffer.left(newlinePos).trimmed();
            buffer.remove(0, newlinePos + 1);
            if (line.isEmpty())
                continue;

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                continue;

            const QJsonObject obj = doc.object();
            quint64 responseCid = 0;
            if (!tryReadCid(obj.value(QStringLiteral("cid")), &responseCid) || responseCid != cid)
                continue;

            const QString type = obj.value(QStringLiteral("type")).toString();
            const QString responseTopic = obj.value(QStringLiteral("topic")).toString();
            const QJsonObject response = obj.value(QStringLiteral("payload")).toObject();

            if (type == QStringLiteral("response") && responseTopic == QStringLiteral("cmd.ack")) {
                ackSeen = true;
                if (!response.value(QStringLiteral("accepted")).toBool()) {
                    if (errorOut) {
                        const QString err = responseErrorMessage(response);
                        *errorOut = QStringLiteral("Command rejected: %1")
                                        .arg(err.isEmpty() ? QStringLiteral("unknown error") : err);
                    }
                    return false;
                }
                continue;
            }

            if (type == QStringLiteral("response") && responseTopic == QStringLiteral("cmd.response")) {
                if (responsePayload)
                    *responsePayload = response;
                return true;
            }

            if (type == QStringLiteral("error") && responseTopic == QStringLiteral("protocol.error")) {
                if (errorOut)
                    *errorOut = QStringLiteral("Protocol error: %1")
                                    .arg(response.value(QStringLiteral("message")).toString());
                return false;
            }
        }

        if (ackSeen && socket.state() != QLocalSocket::ConnectedState) {
            if (errorOut)
                *errorOut = QStringLiteral("Connection closed before command response");
            return false;
        }
    }
}

bool fetchAdapters(const QString &socketPath, QJsonArray *adaptersOut, QString *errorOut)
{
    QJsonObject response;
    if (!sendCommand(socketPath, QStringLiteral("cmd.adapters.list"), QJsonObject{}, &response, errorOut))
        return false;
    if (adaptersOut)
        *adaptersOut = response.value(QStringLiteral("adapters")).toArray();
    return true;
}

QJsonObject sanitizeDiscoveryCandidate(QJsonObject candidate)
{
    candidate.remove(QStringLiteral("cmd"));
    candidate.remove(QStringLiteral("seq"));
    candidate.remove(QStringLiteral("tsMs"));
    candidate.remove(QStringLiteral("streamId"));
    return candidate;
}

bool runDiscoveryStream(const QString &socketPath,
                        const QStringList &pluginTypes,
                        QJsonArray *candidatesOut,
                        QString *errorOut)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to connect to socket: %1").arg(socketPath);
        return false;
    }

    const quint64 cid = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    QJsonObject params;
    if (!pluginTypes.isEmpty()) {
        QJsonArray pluginArray;
        for (const QString &pluginType : pluginTypes)
            pluginArray.append(pluginType);
        params.insert(QStringLiteral("pluginTypes"), pluginArray);
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("adapterId"), 0);
    payload.insert(QStringLiteral("kind"), QStringLiteral("adapter.discover"));
    payload.insert(QStringLiteral("params"), params);

    QJsonObject request;
    request.insert(QStringLiteral("type"), QStringLiteral("cmd"));
    request.insert(QStringLiteral("cid"), static_cast<qint64>(cid));
    request.insert(QStringLiteral("topic"), QStringLiteral("cmd.adapters.stream.start"));
    request.insert(QStringLiteral("payload"), payload);

    const QByteArray wire = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    socket.write(wire);
    if (!socket.waitForBytesWritten(kWriteTimeoutMs)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to send request");
        return false;
    }

    QByteArray buffer;
    QString streamId;
    QJsonArray candidates;
    const qint64 deadlineMs = QDateTime::currentMSecsSinceEpoch() + kDiscoveryTimeoutMs;

    auto remainingMs = [&]() -> int {
        return static_cast<int>(qMax<qint64>(1, deadlineMs - QDateTime::currentMSecsSinceEpoch()));
    };

    while (QDateTime::currentMSecsSinceEpoch() < deadlineMs) {
        if (!socket.waitForReadyRead(remainingMs())) {
            if (errorOut)
                *errorOut = QStringLiteral("Timeout waiting for discovery stream");
            return false;
        }
        buffer.append(socket.readAll());

        while (true) {
            const int newlinePos = buffer.indexOf('\n');
            if (newlinePos < 0)
                break;
            const QByteArray line = buffer.left(newlinePos).trimmed();
            buffer.remove(0, newlinePos + 1);
            if (line.isEmpty())
                continue;

            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                continue;

            const QJsonObject obj = doc.object();
            const QString type = obj.value(QStringLiteral("type")).toString();
            const QString topic = obj.value(QStringLiteral("topic")).toString();
            const QJsonObject response = obj.value(QStringLiteral("payload")).toObject();

            quint64 responseCid = 0;
            const bool cidMatches = tryReadCid(obj.value(QStringLiteral("cid")), &responseCid) && responseCid == cid;

            if (type == QStringLiteral("response") && topic == QStringLiteral("cmd.ack") && cidMatches) {
                if (!response.value(QStringLiteral("accepted")).toBool()) {
                    if (errorOut) {
                        const QString err = responseErrorMessage(response);
                        *errorOut = QStringLiteral("Command rejected: %1")
                                        .arg(err.isEmpty() ? QStringLiteral("unknown error") : err);
                    }
                    return false;
                }
                continue;
            }

            if (type == QStringLiteral("response") && topic == QStringLiteral("cmd.response") && cidMatches) {
                streamId = response.value(QStringLiteral("streamId")).toString().trimmed();
                if (streamId.isEmpty()) {
                    if (errorOut)
                        *errorOut = QStringLiteral("Discovery response did not include a streamId");
                    return false;
                }
                continue;
            }

            if (type == QStringLiteral("error") && topic == QStringLiteral("protocol.error")) {
                if (errorOut)
                    *errorOut = QStringLiteral("Protocol error: %1")
                                    .arg(response.value(QStringLiteral("message")).toString());
                return false;
            }

            if (streamId.isEmpty())
                continue;

            const QString payloadStreamId = response.value(QStringLiteral("streamId")).toString().trimmed();
            const QString payloadCmd = response.value(QStringLiteral("cmd")).toString();
            if (payloadStreamId != streamId || payloadCmd != QStringLiteral("cmd.adapters.stream.start"))
                continue;

            if (type == QStringLiteral("event") && topic == QStringLiteral("stream.data")) {
                candidates.append(sanitizeDiscoveryCandidate(response));
                continue;
            }

            if (type == QStringLiteral("event") && topic == QStringLiteral("stream.error")) {
                if (errorOut) {
                    const QString err = responseErrorMessage(response);
                    *errorOut = err.isEmpty()
                        ? QStringLiteral("Discovery stream failed")
                        : err;
                }
                return false;
            }

            if (type == QStringLiteral("event") && topic == QStringLiteral("stream.end")) {
                if (candidatesOut)
                    *candidatesOut = candidates;
                return true;
            }
        }
    }

    if (errorOut)
        *errorOut = QStringLiteral("Timeout waiting for discovery stream");
    return false;
}

bool parseInt(const QString &input, int *out)
{
    bool ok = false;
    const int value = input.toInt(&ok);
    if (!ok)
        return false;
    if (out)
        *out = value;
    return true;
}

enum class AdapterSelectorType {
    None,
    ById,
    ByExternalId,
    ByName,
    ByPluginType
};

struct CliOptions {
    QString scope;
    QString action;
    QString tenant = QString::fromLatin1(kDefaultTenant);
    QString socketPath = defaultSocketPathForTenant(QString::fromLatin1(kDefaultTenant));
    bool socketPathExplicit = false;
    bool jsonOutput = false;
    bool all = false;
    AdapterSelectorType selectorType = AdapterSelectorType::None;
    int adapterId = 0;
    QString selectorValue;
};

bool parseCliOptions(const QStringList &args, CliOptions *optsOut, QString *errorOut)
{
    if (!optsOut)
        return false;
    if (args.size() < 3) {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid command");
        return false;
    }

    CliOptions opts;
    QStringList positionalArgs;
    QString positionalPluginType;
    positionalArgs.reserve(args.size());
    positionalArgs.push_back(args.at(0));

    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QStringLiteral("--tenant")) {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --tenant");
                return false;
            }
            opts.tenant = args.at(++i).trimmed();
            continue;
        }
        if (arg.startsWith(QStringLiteral("--tenant="))) {
            opts.tenant = arg.mid(QStringLiteral("--tenant=").size()).trimmed();
            continue;
        }
        positionalArgs.push_back(arg);
    }

    if (positionalArgs.size() < 3) {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid command");
        return false;
    }

    opts.scope = positionalArgs.at(1);
    opts.action = positionalArgs.at(2);
    if (opts.scope != QStringLiteral("adapter") && opts.scope != QStringLiteral("transport")) {
        if (errorOut)
            *errorOut = QStringLiteral("Unknown scope: %1").arg(opts.scope);
        return false;
    }

    if (opts.scope == QStringLiteral("transport")) {
        if (opts.action != QStringLiteral("start")
            && opts.action != QStringLiteral("stop")
            && opts.action != QStringLiteral("restart")
            && opts.action != QStringLiteral("reload")) {
            if (errorOut)
                *errorOut = QStringLiteral("Unknown action: %1").arg(opts.action);
            return false;
        }

        for (int i = 3; i < positionalArgs.size(); ++i) {
            const QString arg = positionalArgs.at(i);
            if (arg == QStringLiteral("--socket")) {
                if (i + 1 >= positionalArgs.size()) {
                    if (errorOut)
                        *errorOut = QStringLiteral("Missing value for --socket");
                    return false;
                }
                opts.socketPath = positionalArgs.at(++i);
                opts.socketPathExplicit = true;
                continue;
            }
            if (!arg.startsWith(QLatin1Char('-')) && positionalPluginType.isEmpty()) {
                positionalPluginType = arg;
                continue;
            }
            if (errorOut)
                *errorOut = QStringLiteral("Unknown argument: %1").arg(arg);
            return false;
        }

        if (opts.selectorType == AdapterSelectorType::None && !positionalPluginType.isEmpty()) {
            opts.selectorType = AdapterSelectorType::ByPluginType;
            opts.selectorValue = positionalPluginType;
        }

        if (!isValidTenant(opts.tenant)) {
            if (errorOut)
                *errorOut = QStringLiteral("Invalid tenant: %1").arg(opts.tenant);
            return false;
        }
        if (!opts.socketPathExplicit)
            opts.socketPath = defaultSocketPathForTenant(opts.tenant);

        if (opts.selectorType != AdapterSelectorType::ByPluginType || opts.selectorValue.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("transport %1 requires <plugin>").arg(opts.action);
            return false;
        }
        if (opts.all || opts.jsonOutput) {
            if (errorOut)
                *errorOut = QStringLiteral("transport commands do not support --all or --json");
            return false;
        }

        *optsOut = opts;
        return true;
    }

    if (opts.action != QStringLiteral("list")
        && opts.action != QStringLiteral("discover")
        && opts.action != QStringLiteral("start")
        && opts.action != QStringLiteral("stop")
        && opts.action != QStringLiteral("restart")
        && opts.action != QStringLiteral("reload")) {
        if (errorOut)
            *errorOut = QStringLiteral("Unknown action: %1").arg(opts.action);
        return false;
    }

    for (int i = 3; i < positionalArgs.size(); ++i) {
        const QString arg = positionalArgs.at(i);
        if (arg == QStringLiteral("--json")) {
            opts.jsonOutput = true;
            continue;
        }
        if (arg == QStringLiteral("--all")) {
            opts.all = true;
            continue;
        }
        if (arg == QStringLiteral("--socket")) {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --socket");
                return false;
            }
            opts.socketPath = positionalArgs.at(++i);
            opts.socketPathExplicit = true;
            continue;
        }
        if (arg == QStringLiteral("--id")) {
            if (i + 1 >= positionalArgs.size() || !parseInt(positionalArgs.at(i + 1), &opts.adapterId)) {
                if (errorOut)
                    *errorOut = QStringLiteral("Invalid value for --id");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ById;
            ++i;
            continue;
        }
        if (arg == QStringLiteral("--external-id")) {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --external-id");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByExternalId;
            opts.selectorValue = positionalArgs.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--name")) {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --name");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByName;
            opts.selectorValue = positionalArgs.at(++i);
            continue;
        }
        if (!arg.startsWith(QLatin1Char('-')) && positionalPluginType.isEmpty()) {
            positionalPluginType = arg;
            continue;
        }
        if (errorOut)
            *errorOut = QStringLiteral("Unknown argument: %1").arg(arg);
        return false;
    }

    if (opts.selectorType == AdapterSelectorType::None && !positionalPluginType.isEmpty()) {
        opts.selectorType = AdapterSelectorType::ByPluginType;
        opts.selectorValue = positionalPluginType;
    }

    if (opts.action == QStringLiteral("list")) {
        if (opts.selectorType != AdapterSelectorType::None || opts.all) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter list does not support selectors or --all");
            return false;
        }
    } else if (opts.action == QStringLiteral("discover")) {
        if (opts.selectorType != AdapterSelectorType::None
            && opts.selectorType != AdapterSelectorType::ByPluginType) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter discover supports at most one optional <plugin> filter");
            return false;
        }
        if (opts.all) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter discover does not support --all");
            return false;
        }
    } else if (opts.action == QStringLiteral("reload")) {
        if (opts.selectorType != AdapterSelectorType::ByPluginType || opts.selectorValue.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter reload requires <plugin>");
            return false;
        }
        if (opts.all || opts.jsonOutput) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter reload does not support --all or --json");
            return false;
        }
    } else {
        if (opts.selectorType == AdapterSelectorType::None) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter %1 requires one selector (--id, --external-id, --name, or <plugin> with --all)")
                                .arg(opts.action);
            return false;
        }
        if (opts.selectorType == AdapterSelectorType::ByPluginType) {
            if (!opts.all) {
                if (errorOut)
                    *errorOut = QStringLiteral("<plugin> is only allowed with --all for adapter %1")
                                    .arg(opts.action);
                return false;
            }
        } else if (opts.all) {
            if (errorOut)
                *errorOut = QStringLiteral("--all is only supported with <plugin> for adapter %1").arg(opts.action);
            return false;
        }
        if (opts.jsonOutput) {
            if (errorOut)
                *errorOut = QStringLiteral("--json is only supported with adapter list or adapter discover");
            return false;
        }
    }

    if (!isValidTenant(opts.tenant)) {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid tenant: %1").arg(opts.tenant);
        return false;
    }
    if (!opts.socketPathExplicit)
        opts.socketPath = defaultSocketPathForTenant(opts.tenant);

    *optsOut = opts;
    return true;
}

bool dropRootToPhi(QString *errorOut)
{
    if (geteuid() != 0)
        return true;

    struct passwd *pw = getpwnam("phi");
    if (!pw) {
        if (errorOut)
            *errorOut = QStringLiteral("User 'phi' not found; cannot drop root privileges");
        return false;
    }

    if (setgid(pw->pw_gid) != 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to drop group: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    if (setgroups(1, &pw->pw_gid) != 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to reset supplementary groups: %1")
                             .arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    if (setuid(pw->pw_uid) != 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to drop user: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    if (setuid(0) == 0) {
        if (errorOut)
            *errorOut = QStringLiteral("Unexpectedly able to regain root privileges; aborting");
        return false;
    }

    return true;
}

QList<int> resolveAdapterIds(const CliOptions &opts, const QJsonArray &adapters, QString *errorOut)
{
    QList<int> ids;

    if (opts.selectorType == AdapterSelectorType::ById) {
        ids.append(opts.adapterId);
        return ids;
    }

    auto readId = [](const QJsonObject &obj) -> int {
        bool ok = false;
        const int id = obj.value(QStringLiteral("id")).toVariant().toInt(&ok);
        return ok ? id : -1;
    };

    if (opts.selectorType == AdapterSelectorType::ByExternalId) {
        for (const QJsonValue &entry : adapters) {
            const QJsonObject obj = entry.toObject();
            if (obj.value(QStringLiteral("externalId")).toString() == opts.selectorValue) {
                const int id = readId(obj);
                if (id > 0)
                    ids.append(id);
            }
        }
        if (ids.size() != 1 && errorOut) {
            *errorOut = ids.isEmpty()
                            ? QStringLiteral("No adapter found for externalId '%1'").arg(opts.selectorValue)
                            : QStringLiteral("Multiple adapters found for externalId '%1'").arg(opts.selectorValue);
        }
        return ids;
    }

    if (opts.selectorType == AdapterSelectorType::ByName) {
        for (const QJsonValue &entry : adapters) {
            const QJsonObject obj = entry.toObject();
            if (obj.value(QStringLiteral("name")).toString() == opts.selectorValue) {
                const int id = readId(obj);
                if (id > 0)
                    ids.append(id);
            }
        }
        if (ids.size() != 1 && errorOut) {
            *errorOut = ids.isEmpty()
                            ? QStringLiteral("No adapter found for name '%1'").arg(opts.selectorValue)
                            : QStringLiteral("ambiguous_adapter_name: '%1' matches %2 adapters").arg(opts.selectorValue).arg(ids.size());
        }
        return ids;
    }

    if (opts.selectorType == AdapterSelectorType::ByPluginType) {
        for (const QJsonValue &entry : adapters) {
            const QJsonObject obj = entry.toObject();
            if (obj.value(QStringLiteral("pluginType")).toString() == opts.selectorValue) {
                const int id = readId(obj);
                if (id > 0)
                    ids.append(id);
            }
        }
        if (ids.isEmpty() && errorOut)
            *errorOut = QStringLiteral("No adapters found for pluginType '%1'").arg(opts.selectorValue);
    }

    return ids;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QString dropError;
    if (!dropRootToPhi(&dropError)) {
        QTextStream(stderr) << dropError << "\n";
        return 1;
    }

    const QStringList args = app.arguments();
    CliOptions opts;
    QString parseError;
    if (!parseCliOptions(args, &opts, &parseError)) {
        if (!parseError.isEmpty())
            QTextStream(stderr) << parseError << "\n";
        printUsage();
        return 2;
    }

    if (opts.action == QStringLiteral("list")) {
        QJsonArray adapters;
        QString error;
        if (!fetchAdapters(opts.socketPath, &adapters, &error)) {
            QTextStream(stderr) << error << "\n";
            return 1;
        }
        if (opts.jsonOutput)
            QTextStream(stdout) << QString::fromUtf8(QJsonDocument(adapters).toJson(QJsonDocument::Indented));
        else
            printAdapterTable(adapters);
        return 0;
    }

    if (opts.action == QStringLiteral("discover")) {
        QStringList pluginTypes;
        if (opts.selectorType == AdapterSelectorType::ByPluginType && !opts.selectorValue.isEmpty())
            pluginTypes.push_back(opts.selectorValue);

        QJsonArray candidates;
        QString error;
        if (!runDiscoveryStream(opts.socketPath, pluginTypes, &candidates, &error)) {
            QTextStream(stderr) << error << "\n";
            return 1;
        }
        if (opts.jsonOutput)
            QTextStream(stdout) << QString::fromUtf8(QJsonDocument(candidates).toJson(QJsonDocument::Indented));
        else
            printDiscoveryTable(candidates);
        return 0;
    }

    if (opts.scope == QStringLiteral("transport")) {
        QJsonObject response;
        QString error;
        QJsonObject payload;
        payload.insert(QStringLiteral("pluginType"), opts.selectorValue);
        const QString topic = QStringLiteral("cmd.transport.%1").arg(opts.action);
        if (!sendCommand(opts.socketPath, topic, payload, &response, &error)) {
            QTextStream(stderr) << error << "\n";
            return 1;
        }
        QTextStream(stdout) << "Transport " << opts.action << " completed for pluginType '"
                            << opts.selectorValue << "'\n";
        return 0;
    }

    if (opts.action == QStringLiteral("reload")) {
        QJsonObject response;
        QString error;
        QJsonObject payload;
        payload.insert(QStringLiteral("pluginType"), opts.selectorValue);
        if (!sendCommand(opts.socketPath, QStringLiteral("cmd.adapter.reload"), payload, &response, &error)) {
            QTextStream(stderr) << error << "\n";
            return 1;
        }
        QTextStream(stdout) << "Reload triggered for pluginType '" << opts.selectorValue << "'\n";
        return 0;
    }

    QJsonArray adapters;
    QString listError;
    if (!fetchAdapters(opts.socketPath, &adapters, &listError)) {
        QTextStream(stderr) << listError << "\n";
        return 1;
    }

    QString resolveError;
    const QList<int> ids = resolveAdapterIds(opts, adapters, &resolveError);
    if (!resolveError.isEmpty()) {
        QTextStream(stderr) << resolveError << "\n";
        return 1;
    }
    if (ids.isEmpty()) {
        QTextStream(stderr) << "No adapter ids resolved\n";
        return 1;
    }

    const QString topic = QStringLiteral("cmd.adapter.%1").arg(opts.action);
    int okCount = 0;
    int failCount = 0;
    QStringList failures;
    for (const int id : ids) {
        QJsonObject cmdPayload;
        cmdPayload.insert(QStringLiteral("adapterId"), id);
        QJsonObject response;
        QString error;
        if (sendCommand(opts.socketPath, topic, cmdPayload, &response, &error)) {
            ++okCount;
        } else {
            ++failCount;
            failures.push_back(QStringLiteral("adapterId=%1: %2").arg(id).arg(error));
        }
    }

    QTextStream out(stdout);
    out << opts.action << " completed: ok=" << okCount << " failed=" << failCount << "\n";
    for (const QString &failure : failures)
        QTextStream(stderr) << failure << "\n";
    return failCount == 0 ? 0 : 1;
}
