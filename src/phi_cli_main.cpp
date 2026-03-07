#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTextStream>

namespace {

constexpr const char kDefaultSocketPath[] = "/var/lib/phi/cli.sock";

void printUsage()
{
    QTextStream out(stdout);
    out << "Usage:\n";
    out << "  phi-cli adapter list [--socket <path>] [--json]\n";
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() < 3 || args.at(1) != QStringLiteral("adapter") || args.at(2) != QStringLiteral("list")) {
        printUsage();
        return 2;
    }

    QString socketPath = QString::fromLatin1(kDefaultSocketPath);
    bool jsonOutput = false;

    for (int i = 3; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QStringLiteral("--json")) {
            jsonOutput = true;
            continue;
        }
        if (arg == QStringLiteral("--socket")) {
            if (i + 1 >= args.size()) {
                QTextStream(stderr) << "Missing value for --socket\n";
                return 2;
            }
            socketPath = args.at(++i);
            continue;
        }
        QTextStream(stderr) << "Unknown argument: " << arg << "\n";
        printUsage();
        return 2;
    }

    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(2000)) {
        QTextStream(stderr) << "Failed to connect to socket: " << socketPath << "\n";
        return 1;
    }

    const quint64 cid = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());

    QJsonObject request;
    request.insert(QStringLiteral("type"), QStringLiteral("cmd"));
    request.insert(QStringLiteral("cid"), static_cast<qint64>(cid));
    request.insert(QStringLiteral("topic"), QStringLiteral("cmd.adapters.list"));
    request.insert(QStringLiteral("payload"), QJsonObject{});

    const QByteArray wire = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    socket.write(wire);
    if (!socket.waitForBytesWritten(2000)) {
        QTextStream(stderr) << "Failed to send request\n";
        return 1;
    }

    QByteArray buffer;
    bool ackSeen = false;
    while (true) {
        if (!socket.waitForReadyRead(5000)) {
            QTextStream(stderr) << "Timeout waiting for response\n";
            return 1;
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
            const QString topic = obj.value(QStringLiteral("topic")).toString();
            const QJsonObject payload = obj.value(QStringLiteral("payload")).toObject();

            if (type == QStringLiteral("response") && topic == QStringLiteral("cmd.ack")) {
                ackSeen = true;
                if (!payload.value(QStringLiteral("accepted")).toBool()) {
                    const QString err = payload.value(QStringLiteral("error")).toObject().value(QStringLiteral("msg")).toString();
                    QTextStream(stderr) << "Command rejected: " << (err.isEmpty() ? QStringLiteral("unknown error") : err) << "\n";
                    return 1;
                }
                continue;
            }

            if (type == QStringLiteral("response") && topic == QStringLiteral("cmd.response")) {
                QJsonArray adapters = payload.value(QStringLiteral("adapters")).toArray();
                if (jsonOutput) {
                    QTextStream(stdout) << QString::fromUtf8(QJsonDocument(adapters).toJson(QJsonDocument::Indented));
                } else {
                    printAdapterTable(adapters);
                }
                return 0;
            }

            if (type == QStringLiteral("error") && topic == QStringLiteral("protocol.error")) {
                const QString msg = payload.value(QStringLiteral("message")).toString();
                QTextStream(stderr) << "Protocol error: " << msg << "\n";
                return 1;
            }
        }

        if (ackSeen && socket.state() != QLocalSocket::ConnectedState) {
            QTextStream(stderr) << "Connection closed before command response\n";
            return 1;
        }
    }
}
