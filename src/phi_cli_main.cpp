#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTextStream>

#include <cerrno>
#include <cstring>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr const char kDefaultSocketPath[] = "/var/lib/phi/cli.sock";

void printUsage()
{
    QTextStream out(stdout);
    out << "Usage:\n";
    out << "  phi-cli adapter list [--socket <path>] [--json]\n";
    out << "  phi-cli adapter start|stop|restart (--id <id> | --external-id <id> | --name <name>) [--socket <path>]\n";
    out << "  phi-cli adapter start|stop|restart --plugin-type <type> --all [--socket <path>]\n";
    out << "  phi-cli adapter reload --plugin-type <type> [--socket <path>]\n";
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
    if (!socket.waitForConnected(2000)) {
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
    if (!socket.waitForBytesWritten(2000)) {
        if (errorOut)
            *errorOut = QStringLiteral("Failed to send request");
        return false;
    }

    QByteArray buffer;
    bool ackSeen = false;
    while (true) {
        if (!socket.waitForReadyRead(5000)) {
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
    QString action;
    QString socketPath = QString::fromLatin1(kDefaultSocketPath);
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
    if (args.size() < 3 || args.at(1) != QStringLiteral("adapter")) {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid command");
        return false;
    }

    CliOptions opts;
    opts.action = args.at(2);
    if (opts.action != QStringLiteral("list")
        && opts.action != QStringLiteral("start")
        && opts.action != QStringLiteral("stop")
        && opts.action != QStringLiteral("restart")
        && opts.action != QStringLiteral("reload")) {
        if (errorOut)
            *errorOut = QStringLiteral("Unknown action: %1").arg(opts.action);
        return false;
    }

    for (int i = 3; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QStringLiteral("--json")) {
            opts.jsonOutput = true;
            continue;
        }
        if (arg == QStringLiteral("--all")) {
            opts.all = true;
            continue;
        }
        if (arg == QStringLiteral("--socket")) {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --socket");
                return false;
            }
            opts.socketPath = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--id")) {
            if (i + 1 >= args.size() || !parseInt(args.at(i + 1), &opts.adapterId)) {
                if (errorOut)
                    *errorOut = QStringLiteral("Invalid value for --id");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ById;
            ++i;
            continue;
        }
        if (arg == QStringLiteral("--external-id")) {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --external-id");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByExternalId;
            opts.selectorValue = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--name")) {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --name");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByName;
            opts.selectorValue = args.at(++i);
            continue;
        }
        if (arg == QStringLiteral("--plugin-type")) {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = QStringLiteral("Missing value for --plugin-type");
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByPluginType;
            opts.selectorValue = args.at(++i);
            continue;
        }
        if (errorOut)
            *errorOut = QStringLiteral("Unknown argument: %1").arg(arg);
        return false;
    }

    if (opts.action == QStringLiteral("list")) {
        if (opts.selectorType != AdapterSelectorType::None || opts.all) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter list does not support selectors or --all");
            return false;
        }
    } else if (opts.action == QStringLiteral("reload")) {
        if (opts.selectorType != AdapterSelectorType::ByPluginType || opts.selectorValue.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("adapter reload requires --plugin-type <type>");
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
                *errorOut = QStringLiteral("adapter %1 requires one selector (--id, --external-id, --name, or --plugin-type with --all)")
                                .arg(opts.action);
            return false;
        }
        if (opts.selectorType == AdapterSelectorType::ByPluginType) {
            if (!opts.all) {
                if (errorOut)
                    *errorOut = QStringLiteral("--plugin-type is only allowed with --all for adapter %1")
                                    .arg(opts.action);
                return false;
            }
        } else if (opts.all) {
            if (errorOut)
                *errorOut = QStringLiteral("--all is only supported with --plugin-type for adapter %1").arg(opts.action);
            return false;
        }
        if (opts.jsonOutput) {
            if (errorOut)
                *errorOut = QStringLiteral("--json is only supported with adapter list");
            return false;
        }
    }

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
