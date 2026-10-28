// phi-cli: the local client for the tenant's CLI transport socket.
//
// Qt-free (the Qt exit, last consumer of Qt6 in this repo): JSON is nlohmann,
// like the transport plugin beside it; the envelope types, the response topics
// and the cid rules come from phi/transport/api/envelope.h rather than from
// string literals typed a second time here; and the socket is cli_socket.h,
// which is QLocalSocket's waitFor... trio written as poll().

#include "cli_socket.h"
#include "scram_verifier.h"

#include <commandframe.h>

#include <envelope.h>
#include <nlohmann/json.hpp>
#include <phi/runtime/str.h>

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>

namespace {

using Json = nlohmann::json;
namespace str = phi::str;
namespace tapi = phicore::transport;

constexpr const char kDefaultTenant[] = "1";
constexpr int kConnectTimeoutMs = 2000;
constexpr int kWriteTimeoutMs = 2000;
constexpr int kResponseTimeoutMs = 15000;
constexpr int kDiscoveryTimeoutMs = 30000;

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ------------------------------------------------------------------ JSON

Json parseJson(std::string_view text)
{
    const Json parsed = Json::parse(text, nullptr, false);
    return parsed.is_discarded() ? Json() : parsed;
}

Json jsonValue(const Json &obj, std::string_view key)
{
    if (!obj.is_object())
        return Json();
    const auto it = obj.find(key);
    return it == obj.end() ? Json() : *it;
}

Json jsonObject(const Json &obj, std::string_view key)
{
    const Json value = jsonValue(obj, key);
    return value.is_object() ? value : Json::object();
}

Json jsonArray(const Json &obj, std::string_view key)
{
    const Json value = jsonValue(obj, key);
    return value.is_array() ? value : Json::array();
}

std::string jsonString(const Json &obj, std::string_view key)
{
    const Json value = jsonValue(obj, key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

bool jsonBool(const Json &obj, std::string_view key, bool fallback = false)
{
    const Json value = jsonValue(obj, key);
    return value.is_boolean() ? value.get<bool>() : fallback;
}

/// A scalar as text, the way QJsonValue::toVariant().toString() rendered it:
/// strings as they are, numbers and booleans spelled out, anything else empty.
std::string scalarText(const Json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer())
        return str::number(value.get<long long>());
    if (value.is_number_unsigned())
        return str::number(value.get<unsigned long long>());
    if (value.is_number_float())
        return str::number(value.get<double>());
    return {};
}

/// QJsonDocument::toJson(Indented): four spaces, and a newline at the end.
std::string dumpIndented(const Json &value)
{
    return value.dump(4, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::string dumpCompact(const Json &value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

/// An integer that may have arrived as a number or as a numeric string -
/// QVariant::toInt() across both. Nothing when it is neither.
std::optional<int> jsonInt(const Json &value)
{
    if (value.is_number_integer())
        return static_cast<int>(value.get<long long>());
    if (value.is_number_float())
        return static_cast<int>(value.get<double>());
    if (value.is_string()) {
        bool ok = false;
        const int parsed = str::toInt(str::trimmed(value.get<std::string>()), &ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ output

/// One table cell: left-aligned in `width`, never truncated - QTextStream's
/// setFieldWidth with AlignLeft, which also let a long value run over.
std::string cell(const std::string &text, std::size_t width)
{
    if (text.size() >= width)
        return text;
    return text + std::string(width - text.size(), ' ');
}

bool isValidTenant(const std::string &tenant)
{
    // ^[A-Za-z0-9][A-Za-z0-9._-]*$ - a tenant names a directory under
    // /var/lib/phi, so nothing that could walk out of it.
    if (tenant.empty())
        return false;
    const auto isAlnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!isAlnum(tenant.front()))
        return false;
    for (const char c : tenant) {
        if (!isAlnum(c) && c != '.' && c != '_' && c != '-')
            return false;
    }
    return true;
}

std::string defaultSocketPathForTenant(const std::string &tenant)
{
    return "/var/lib/phi/@" + tenant + "/cli.sock";
}

void printUsage()
{
    std::cout
        << "Usage:\n"
        << "  phi-cli discover [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli adapter list [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli adapter discover [<plugin>] [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli adapter start|stop|restart (--id <id> | --external-id <id> | --name <name>) [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli adapter start|stop|restart <plugin> --all [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli adapter reload <plugin> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli adapter rescan [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli adapter invoke <actionId> (--id <id> | --external-id <id> | --name <name>) [--params <json>] [--json] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli transport start|stop|restart|reload <plugin> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli radio list [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli radio probe <resource> [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli radio claim <resource> --claimant <name> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli radio release <resource> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backend list [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli backend intents [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli backend create --kind <kind> --id <name> [--resource <resource>] [--json] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backend remove <backend> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backend start|stop|restart <backend> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli channel remove <channelId> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli user list [--tenant <tenant>] [--socket <path>] [--json]\n"
        << "  phi-cli user passwd <username> [--password-stdin] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup list [--json] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup passphrase [--password-stdin] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup create [--history] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup show <file>\n"
        << "  phi-cli backup import <file> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup inspect <backupId|file> [--no-passphrase] [--password-stdin] [--json] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup restore <backupId|file> [--allow-other-tenant] [--password-stdin] [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup export <backupId> <file> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup delete <backupId> [--tenant <tenant>] [--socket <path>]\n"
        << "  phi-cli backup passphrase|create|list|inspect|restore|delete --box [...]   (root: every tenant and the host)\n"
        << "  phi-cli backup create|restore --host [...]   (root: the host part only)\n";
}

std::string boolText(bool value)
{
    return value ? "yes" : "no";
}

void printAdapterTable(const Json &adapters)
{
    std::cout << cell("ID", 8) << cell("PLUGIN", 16) << cell("EXTERNAL_ID", 20)
              << cell("NAME", 28) << cell("ENABLED", 10) << cell("RUNNING", 10)
              << cell("CONNECTED", 10) << "\n";

    for (const Json &entry : adapters) {
        std::cout << cell(scalarText(jsonValue(entry, "id")), 8)
                  << cell(jsonString(entry, "pluginType"), 16)
                  << cell(jsonString(entry, "externalId"), 20)
                  << cell(jsonString(entry, "name"), 28)
                  << cell(boolText(jsonBool(entry, "enabled")), 10)
                  << cell(boolText(jsonBool(entry, "running")), 10)
                  << cell(boolText(jsonBool(entry, "connected")), 10) << "\n";
    }
}

void printDiscoveryTable(const Json &candidates)
{
    std::cout << cell("PLUGIN", 16) << cell("PROVIDER", 12) << cell("LABEL", 28)
              << cell("EXTERNAL_ID", 22) << cell("HOST", 24) << cell("IP", 18)
              << cell("PORT", 8) << cell("SERVICE", 12) << "\n";

    for (const Json &entry : candidates) {
        std::cout << cell(jsonString(entry, "plugin"), 16)
                  << cell(jsonString(entry, "provider"), 12)
                  << cell(jsonString(entry, "label"), 28)
                  << cell(jsonString(entry, "externalId"), 22)
                  << cell(jsonString(entry, "hostname"), 24)
                  << cell(jsonString(entry, "ip"), 18)
                  << cell(scalarText(jsonValue(entry, "port")), 8)
                  << cell(jsonString(entry, "service"), 12) << "\n";
    }
}

/// One row per port the host layer is reporting, claimed or not.
///
/// The resource id comes first because it is the argument every other radio
/// command takes, and the device path comes last because it is the only column
/// that regularly runs over its width - a `by-id` link is longer than anything
/// worth aligning the rest of the table to.
void printRadioTable(const Json &radios)
{
    std::cout << cell("RESOURCE", 34) << cell("STATE", 12) << cell("FAMILY", 12)
              << cell("IDENTIFIED", 14) << cell("CLAIMANT", 20) << "DEVICE" << "\n";

    for (const Json &entry : radios) {
        // A port somebody else holds arrives as an id and a state and nothing
        // else. The empty cells are the honest rendering of that: the row is
        // there because the port is there, and the rest is not ours to read.
        const Json claim = jsonObject(entry, "claim");
        const std::string device = jsonString(entry, "devicePath").empty()
            ? jsonString(entry, "nodePath")
            : jsonString(entry, "devicePath");

        std::cout << cell(jsonString(entry, "id"), 34)
                  << cell(jsonString(entry, "state"), 12)
                  << cell(jsonString(entry, "radioFamily"), 12)
                  << cell(jsonString(entry, "identification"), 14)
                  << cell(jsonString(claim, "claimant"), 20) << device << "\n";
    }
}

/// One row per backend, with what is meant to happen beside what is happening.
///
/// The two columns are not the same question and reading only the second gets
/// it wrong: a backend somebody stopped, one whose radio has been gone long
/// enough that the host layer stopped expecting it, and one whose runtime keeps
/// crashing are three different situations that systemd calls `inactive`.
void printBackendTable(const Json &backends)
{
    std::cout << cell("NAME", 24) << cell("KIND", 16) << cell("TENANT", 8)
              << cell("INSTALLED", 11) << cell("EXPECTED", 10) << cell("STATE", 12)
              << "RESOURCE" << "\n";

    for (const Json &entry : backends) {
        std::cout << cell(jsonString(entry, "name"), 24)
                  << cell(jsonString(entry, "kind"), 16)
                  << cell(jsonString(entry, "tenant"), 8)
                  << cell(boolText(jsonBool(entry, "installed")), 11)
                  << cell(boolText(jsonBool(entry, "shouldRun")), 10)
                  << cell(jsonString(entry, "state"), 12)
                  << jsonString(entry, "resourceId") << "\n";
    }
}

/// What the packages on this box asked for, and what became of it.
///
/// A rule - a file that asks for one backend per coordinator rather than for a
/// named one - is printed as the rows it means right now, one per stick it
/// would take. A rule that means nothing at the moment keeps a row of its own
/// under the name `kind@*`, because "this box wants one per stick and no stick
/// suits" is the answer somebody is looking for and an empty list does not
/// give it.
void printIntentTable(const Json &intents)
{
    std::cout << cell("STATE", 12) << cell("BACKEND", 24) << cell("TENANT", 8)
              << cell("RESOURCE", 34) << "SOURCE" << "\n";

    for (const Json &entry : intents) {
        const std::string backend = jsonString(entry, "backend");
        const std::string shown = !backend.empty()
            ? backend
            : jsonString(entry, "kind") + (jsonBool(entry, "rule") ? "@*" : "@?");

        std::cout << cell(jsonString(entry, "state"), 12) << cell(shown, 24)
                  << cell(jsonString(entry, "tenant"), 8)
                  << cell(jsonString(entry, "resourceId"), 34)
                  << jsonString(entry, "source") << "\n";

        // Under its row rather than in a column: a note is a sentence, and a
        // sentence in a table pushes everything after it off the screen.
        const std::string note = jsonString(entry, "note");
        if (!note.empty())
            std::cout << "  " << note << "\n";
    }
}

/// The backend kinds this host has a runtime for, printed under the table.
///
/// Without it `backend create --kind` is a guess: the kinds come from whatever
/// descriptors are installed in `/usr/share/phi/backends`, so the only way to
/// know what this box will accept is to ask this box.
void printBackendKinds(const Json &kinds)
{
    if (kinds.empty()) {
        std::cout << "\nNo backend kinds are installed on this host.\n";
        return;
    }

    std::cout << "\nInstalled kinds:\n";
    for (const Json &entry : kinds) {
        const std::string displayName = jsonString(entry, "displayName");
        std::cout << "  " << cell(jsonString(entry, "kind"), 18) << displayName << "\n";
    }
}

/// Users as core lists them. The flags are core's UserFlag bits; the three
/// that decide what an account is are spelled out, the rest stay a number.
void printUserTable(const Json &users)
{
    constexpr int kEnabled = 0x0001;
    constexpr int kOwner = 0x0010;
    constexpr int kAdmin = 0x0020;

    std::cout << cell("ID", 6) << cell("USERNAME", 24) << cell("ENABLED", 10)
              << cell("OWNER", 8) << cell("ADMIN", 8) << "FLAGS\n";
    for (const Json &entry : users) {
        const int flags = jsonInt(jsonValue(entry, "flags")).value_or(0);
        std::cout << cell(scalarText(jsonValue(entry, "id")), 6)
                  << cell(jsonString(entry, "username"), 24)
                  << cell(boolText((flags & kEnabled) != 0), 10)
                  << cell(boolText((flags & kOwner) != 0), 8)
                  << cell(boolText((flags & kAdmin) != 0), 8)
                  << flags << "\n";
    }
}

// Core's error message is a translation key with %1..%9 placeholders and the
// values under "params"; a terminal shows it in English, filled in.
std::string errorText(const Json &errorValue)
{
    const std::string message = jsonString(errorValue, "message");
    const Json params = jsonValue(errorValue, "params");
    if (!params.is_array() || params.empty())
        return message;
    std::string out;
    out.reserve(message.size() + 16);
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (message[i] == '%' && i + 1 < message.size() && message[i + 1] >= '1' && message[i + 1] <= '9') {
            const std::size_t n = static_cast<std::size_t>(message[i + 1] - '1');
            if (n < params.size()) {
                out += params[n].is_string() ? params[n].get<std::string>() : dumpCompact(params[n]);
                ++i;
                continue;
            }
        }
        out += message[i];
    }
    return out;
}

std::string responseErrorMessage(const Json &payload)
{
    const Json errorValue = jsonValue(payload, "error");
    if (errorValue.is_object()) {
        const std::string msg = jsonString(errorValue, "msg");
        if (!msg.empty())
            return msg;
        return errorText(errorValue);
    }
    if (errorValue.is_string())
        return errorValue.get<std::string>();
    return {};
}

// ------------------------------------------------------------------ protocol

Json commandEnvelope(tapi::CmdId cid, const std::string &topic, const Json &payload)
{
    Json request = Json::object();
    request["type"] = tapi::kEnvelopeTypeCmd;
    request["cid"] = cid;
    request["topic"] = topic;
    request["payload"] = payload;
    return request;
}

bool sendCommand(const std::string &socketPath,
                 const std::string &topic,
                 const Json &payload,
                 Json *responsePayload,
                 std::string *errorOut)
{
    phicore::cli::LineSocket socket;
    if (!socket.connect(socketPath, kConnectTimeoutMs, errorOut))
        return false;

    const auto cid = static_cast<tapi::CmdId>(nowMs());
    if (!socket.writeAll(dumpCompact(commandEnvelope(cid, topic, payload)) + "\n",
                         kWriteTimeoutMs, errorOut))
        return false;

    // One deadline for the whole answer. Every client gets every event, so
    // timing each read on its own let a busy house keep a command waiting
    // forever for an answer that was never coming.
    const std::int64_t deadlineMs = nowMs() + kResponseTimeoutMs;
    bool ackSeen = false;
    while (true) {
        std::string line;
        const auto remainingMs = static_cast<int>(std::max<std::int64_t>(0, deadlineMs - nowMs()));
        const auto read = socket.readLine(remainingMs, &line, errorOut);
        if (read == phicore::cli::LineSocket::Read::Timeout) {
            if (errorOut)
                *errorOut = "Timeout waiting for response";
            return false;
        }
        if (read == phicore::cli::LineSocket::Read::Closed) {
            if (errorOut) {
                *errorOut = ackSeen ? "Connection closed before command response"
                                    : "Connection closed before the command was acknowledged";
            }
            return false;
        }
        if (read == phicore::cli::LineSocket::Read::Failed)
            return false;

        const Json envelope = parseJson(str::trimmed(line));
        if (!envelope.is_object())
            continue;

        const auto responseCid = tapi::readCid(jsonValue(envelope, "cid"));
        if (!responseCid.has_value() || *responseCid != cid)
            continue;

        const std::string type = jsonString(envelope, "type");
        const std::string responseTopic = jsonString(envelope, "topic");
        const Json response = jsonObject(envelope, "payload");

        if (type == tapi::kEnvelopeTypeResponse && responseTopic == tapi::kTopicCmdAck) {
            ackSeen = true;
            if (!jsonBool(response, "accepted")) {
                if (errorOut) {
                    const std::string err = responseErrorMessage(response);
                    *errorOut = "Command rejected: " + (err.empty() ? std::string("unknown error") : err);
                }
                return false;
            }
            continue;
        }

        if (type == tapi::kEnvelopeTypeResponse && responseTopic == tapi::kTopicCmdResponse) {
            // A command core refused comes back here, not in the ack: the ack
            // says the transport took the frame, and the answer says what core
            // did with it. `accepted` is only ever written on a rejection
            // (`makeSyncRejectionPayload`), so its absence is a success and
            // reading it is not a second opinion about one.
            //
            // Without this a refusal printed a success line with every field
            // empty - "usb-1a86:7523@1-4-if00: answered nothing this host
            // recognises" for a port the host had just declined to touch.
            const Json accepted = jsonValue(response, "accepted");
            if (accepted.is_boolean() && !accepted.get<bool>()) {
                if (errorOut) {
                    const std::string err = responseErrorMessage(response);
                    *errorOut = err.empty() ? std::string("Command refused") : err;
                }
                return false;
            }
            if (responsePayload)
                *responsePayload = response;
            return true;
        }

        if (type == tapi::kEnvelopeTypeError && responseTopic == tapi::kTopicProtocolError) {
            if (errorOut)
                *errorOut = "Protocol error: " + jsonString(response, "message");
            return false;
        }
    }
}

bool fetchAdapters(const std::string &socketPath, Json *adaptersOut, std::string *errorOut)
{
    Json response;
    if (!sendCommand(socketPath, "cmd.adapters.list", Json::object(), &response, errorOut))
        return false;
    if (adaptersOut)
        *adaptersOut = jsonArray(response, "adapters");
    return true;
}

Json sanitizeDiscoveryCandidate(Json candidate)
{
    candidate.erase("cmd");
    candidate.erase("seq");
    candidate.erase("tsMs");
    candidate.erase("streamId");
    return candidate;
}

bool runDiscoveryStream(const std::string &socketPath,
                        const std::string &kind,
                        const std::vector<std::string> &pluginTypes,
                        Json *candidatesOut,
                        std::string *errorOut)
{
    phicore::cli::LineSocket socket;
    if (!socket.connect(socketPath, kConnectTimeoutMs, errorOut))
        return false;

    const auto cid = static_cast<tapi::CmdId>(nowMs());

    Json params = Json::object();
    if (!pluginTypes.empty())
        params["pluginTypes"] = pluginTypes;

    Json payload = Json::object();
    payload["kind"] = kind;
    payload["params"] = params;

    if (!socket.writeAll(dumpCompact(commandEnvelope(cid, "cmd.stream.start", payload)) + "\n",
                         kWriteTimeoutMs, errorOut))
        return false;

    std::string streamId;
    Json candidates = Json::array();
    const std::int64_t deadlineMs = nowMs() + kDiscoveryTimeoutMs;

    while (nowMs() < deadlineMs) {
        const auto remainingMs = static_cast<int>(std::max<std::int64_t>(1, deadlineMs - nowMs()));
        std::string line;
        const auto read = socket.readLine(remainingMs, &line, errorOut);
        if (read == phicore::cli::LineSocket::Read::Timeout)
            break;
        if (read == phicore::cli::LineSocket::Read::Closed) {
            if (errorOut)
                *errorOut = "Connection closed before the discovery stream ended";
            return false;
        }
        if (read == phicore::cli::LineSocket::Read::Failed)
            return false;

        const Json envelope = parseJson(str::trimmed(line));
        if (!envelope.is_object())
            continue;

        const std::string type = jsonString(envelope, "type");
        const std::string topic = jsonString(envelope, "topic");
        const Json response = jsonObject(envelope, "payload");

        const auto responseCid = tapi::readCid(jsonValue(envelope, "cid"));
        const bool cidMatches = responseCid.has_value() && *responseCid == cid;

        if (type == tapi::kEnvelopeTypeResponse && topic == tapi::kTopicCmdAck && cidMatches) {
            if (!jsonBool(response, "accepted")) {
                if (errorOut) {
                    const std::string err = responseErrorMessage(response);
                    *errorOut = "Command rejected: " + (err.empty() ? std::string("unknown error") : err);
                }
                return false;
            }
            continue;
        }

        if (type == tapi::kEnvelopeTypeResponse && topic == tapi::kTopicCmdResponse && cidMatches) {
            // The same rule the plain commands follow: a refusal arrives as the
            // answer, and saying why beats "no streamId".
            const Json accepted = jsonValue(response, "accepted");
            if (accepted.is_boolean() && !accepted.get<bool>()) {
                if (errorOut) {
                    const std::string err = responseErrorMessage(response);
                    *errorOut = err.empty() ? std::string("Discovery refused") : err;
                }
                return false;
            }
            streamId = str::trimmed(jsonString(response, "streamId"));
            if (streamId.empty()) {
                if (errorOut)
                    *errorOut = "Discovery response did not include a streamId";
                return false;
            }
            continue;
        }

        if (type == tapi::kEnvelopeTypeError && topic == tapi::kTopicProtocolError) {
            if (errorOut)
                *errorOut = "Protocol error: " + jsonString(response, "message");
            return false;
        }

        if (streamId.empty())
            continue;

        if (str::trimmed(jsonString(response, "streamId")) != streamId
            || jsonString(response, "cmd") != "cmd.stream.start") {
            continue;
        }

        if (type == tapi::kEnvelopeTypeEvent && topic == "stream.data") {
            candidates.push_back(sanitizeDiscoveryCandidate(response));
            continue;
        }

        if (type == tapi::kEnvelopeTypeEvent && topic == "stream.error") {
            if (errorOut) {
                const std::string err = responseErrorMessage(response);
                *errorOut = err.empty() ? "Discovery stream failed" : err;
            }
            return false;
        }

        if (type == tapi::kEnvelopeTypeEvent && topic == "stream.end") {
            if (candidatesOut)
                *candidatesOut = candidates;
            return true;
        }
    }

    if (errorOut)
        *errorOut = "Timeout waiting for discovery stream";
    return false;
}

// ------------------------------------------------------------------ options

bool parseInt(const std::string &input, int *out)
{
    bool ok = false;
    const int value = str::toInt(input, &ok);
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
    std::string scope;
    std::string action;
    std::string tenant = kDefaultTenant;
    std::string socketPath = defaultSocketPathForTenant(kDefaultTenant);
    bool socketPathExplicit = false;
    bool jsonOutput = false;
    bool all = false;
    AdapterSelectorType selectorType = AdapterSelectorType::None;
    int adapterId = 0;
    std::string selectorValue;
    std::string invokeActionId;
    std::string invokeParamsJson;
    // The host layer's arguments. A resource id is what the inventory calls one
    // port, a claimant is who wants to hold it, and a backend is named by its
    // kind and the instance id the operator chose.
    std::string resourceId;
    std::string claimant;
    std::string backendKind;
    std::string backendId;
    // `user passwd`: whose password, and whether it comes from a pipe rather
    // than a prompt. Never from an argument, which ps and the shell history
    // would both keep.
    std::string username;
    bool passwordStdin = false;
    // `channel remove`: a channel its device no longer has. Core refuses one
    // still reported; a stale one (gone for now) stays greyed until removed.
    int channelId = 0;
    // `backup ...`: the backup or file named, where an export goes, and the
    // switches. The passphrase never comes as an argument either.
    std::string backupTarget;
    std::string backupOut;
    bool includeHistory = false;
    bool allowOtherTenant = false;
    bool noPassphrase = false;
};

/// `kind@id` back into its two halves - the shape `backendName()` composes on
/// the host side, and the only name a backend is ever printed under.
///
/// Split at the first `@` rather than the last: an instance id may not contain
/// one (the host layer refuses anything but letters, digits, hyphens and
/// underscores), so the first is the only one, and a name with a second `@` in
/// it is one this host never wrote.
bool splitBackendName(const std::string &name, std::string *kind, std::string *id)
{
    const std::size_t at = name.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= name.size())
        return false;
    if (kind)
        *kind = name.substr(0, at);
    if (id)
        *id = name.substr(at + 1);
    return true;
}

/// The host layer's own arguments: `radio ...` and `backend ...`.
///
/// Parsed apart from the adapter scope because nothing is shared with it. An
/// adapter is picked out of a list this core keeps, by id or name or plugin
/// type; a port and a backend are named outright, with the id the host layer
/// printed in the table above.
bool parseHostScope(const std::vector<std::string> &positionalArgs, CliOptions *opts,
                    std::string *errorOut)
{
    const bool isRadio = opts->scope == "radio";
    std::string positional;
    std::size_t i = 3;

    const auto takeValue = [&](std::string *out) {
        if (i + 1 >= positionalArgs.size())
            return false;
        *out = str::trimmed(positionalArgs.at(++i));
        return true;
    };
    const auto missing = [&](const char *option) {
        if (errorOut)
            *errorOut = std::string("Missing value for ") + option;
        return false;
    };

    for (; i < positionalArgs.size(); ++i) {
        const std::string &arg = positionalArgs.at(i);
        if (arg == "--json") {
            opts->jsonOutput = true;
            continue;
        }
        if (arg == "--socket") {
            if (!takeValue(&opts->socketPath))
                return missing("--socket");
            opts->socketPathExplicit = true;
            continue;
        }
        if (arg == "--claimant") {
            if (!takeValue(&opts->claimant))
                return missing("--claimant");
            continue;
        }
        if (arg == "--kind") {
            if (!takeValue(&opts->backendKind))
                return missing("--kind");
            continue;
        }
        if (arg == "--id") {
            if (!takeValue(&opts->backendId))
                return missing("--id");
            continue;
        }
        if (arg == "--resource") {
            if (!takeValue(&opts->resourceId))
                return missing("--resource");
            continue;
        }
        if (!arg.starts_with("-") && positional.empty()) {
            positional = str::trimmed(arg);
            continue;
        }
        if (errorOut)
            *errorOut = "Unknown argument: " + arg;
        return false;
    }

    const auto refuse = [&](const std::string &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (isRadio) {
        if (opts->action != "list" && opts->action != "probe" && opts->action != "claim"
            && opts->action != "release") {
            return refuse("Unknown action: " + opts->action);
        }
        if (!opts->backendKind.empty() || !opts->backendId.empty() || !opts->resourceId.empty())
            return refuse("radio " + opts->action + " takes <resource> rather than an option");
        if (opts->action != "claim" && !opts->claimant.empty())
            return refuse("--claimant is only supported with radio claim");
        if (opts->jsonOutput && opts->action != "list" && opts->action != "probe")
            return refuse("--json is only supported with radio list or radio probe");

        if (opts->action == "list") {
            if (!positional.empty())
                return refuse("radio list takes no argument");
            return true;
        }
        if (positional.empty())
            return refuse("radio " + opts->action + " requires <resource>");
        opts->resourceId = positional;
        if (opts->action == "claim" && opts->claimant.empty())
            return refuse("radio claim requires --claimant");
        return true;
    }

    if (opts->action != "list" && opts->action != "intents" && opts->action != "create"
        && opts->action != "remove" && opts->action != "start" && opts->action != "stop"
        && opts->action != "restart") {
        return refuse("Unknown action: " + opts->action);
    }
    if (!opts->claimant.empty())
        return refuse("--claimant is only supported with radio claim");
    if (opts->jsonOutput && opts->action != "list" && opts->action != "intents"
        && opts->action != "create") {
        return refuse("--json is only supported with backend list, backend intents or backend create");
    }

    if (opts->action == "list" || opts->action == "intents") {
        if (!positional.empty() || !opts->backendKind.empty() || !opts->backendId.empty()
            || !opts->resourceId.empty()) {
            return refuse("backend " + opts->action + " takes no argument");
        }
        return true;
    }

    if (opts->action == "create") {
        if (!positional.empty())
            return refuse("backend create names its backend with --kind and --id");
        if (opts->backendKind.empty())
            return refuse("backend create requires --kind");
        if (opts->backendId.empty())
            return refuse("backend create requires --id");
        // Named rather than guessed. The host layer picks a port by itself only
        // where a rule under /usr/share/phi/intents asked it to, and a person
        // at a terminal is not that rule: creating a backend claims a piece of
        // hardware, and which one is the operator's sentence to write. A kind
        // that drives no radio takes none, and the host layer says so when one
        // is named anyway.
        return true;
    }

    if (!opts->resourceId.empty())
        return refuse("backend " + opts->action + " does not take --resource");
    if (positional.empty()) {
        if (opts->backendKind.empty() || opts->backendId.empty())
            return refuse("backend " + opts->action + " requires <backend>, the name from backend list");
        return true;
    }
    if (!opts->backendKind.empty() || !opts->backendId.empty())
        return refuse("backend " + opts->action + " takes <backend> or --kind with --id, not both");
    if (!splitBackendName(positional, &opts->backendKind, &opts->backendId))
        return refuse("'" + positional + "' is not a backend name; they read as kind@name");
    return true;
}

/// `user list` and `user passwd <username>`.
bool parseUserScope(const std::vector<std::string> &positionalArgs, CliOptions *opts,
                    std::string *errorOut)
{
    const auto refuse = [errorOut](const std::string &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (opts->action != "list" && opts->action != "passwd")
        return refuse("Unknown action: " + opts->action);

    for (std::size_t i = 3; i < positionalArgs.size(); ++i) {
        const std::string &arg = positionalArgs.at(i);
        if (arg == "--json" && opts->action == "list") {
            opts->jsonOutput = true;
            continue;
        }
        if (arg == "--password-stdin" && opts->action == "passwd") {
            opts->passwordStdin = true;
            continue;
        }
        if (arg == "--socket") {
            if (i + 1 >= positionalArgs.size())
                return refuse("Missing value for --socket");
            opts->socketPath = positionalArgs.at(++i);
            opts->socketPathExplicit = true;
            continue;
        }
        if (!arg.starts_with("-") && opts->action == "passwd" && opts->username.empty()) {
            opts->username = str::trimmed(arg);
            continue;
        }
        return refuse("Unknown argument: " + arg);
    }

    if (opts->action == "passwd" && opts->username.empty())
        return refuse("user passwd requires <username>");
    return true;
}

/// `channel remove <channelId>`: core's cmd.channel.remove, for a channel the
/// device no longer supports - the UI does not offer it, because an
/// automation that uses the channel breaks (core disables it and says so).
bool parseChannelScope(const std::vector<std::string> &positionalArgs, CliOptions *opts, std::string *errorOut)
{
    const auto refuse = [errorOut](const std::string &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (opts->action != "remove")
        return refuse("Unknown action: " + opts->action);

    for (std::size_t i = 3; i < positionalArgs.size(); ++i) {
        const std::string &arg = positionalArgs.at(i);
        if (arg == "--socket") {
            if (i + 1 >= positionalArgs.size())
                return refuse("Missing value for --socket");
            opts->socketPath = positionalArgs.at(++i);
            opts->socketPathExplicit = true;
            continue;
        }
        if (!arg.starts_with("-") && opts->channelId == 0) {
            bool ok = false;
            const int id = str::toInt(arg, &ok);
            if (!ok || id <= 0)
                return refuse("Invalid channel id: " + arg);
            opts->channelId = id;
            continue;
        }
        return refuse("Unknown argument: " + arg);
    }
    if (opts->channelId == 0)
        return refuse("channel remove requires <channelId>");
    return true;
}

/// `backup <action> ...` (docs/backup-concept.md in phi-core).
bool parseBackupScope(const std::vector<std::string> &positionalArgs, CliOptions *opts, std::string *errorOut)
{
    const auto refuse = [errorOut](const std::string &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    static const std::vector<std::string> kActions = {"list",    "passphrase", "create", "show",  "import",
                                                      "inspect", "restore",    "export", "delete"};
    if (std::find(kActions.begin(), kActions.end(), opts->action) == kActions.end())
        return refuse("Unknown action: " + opts->action);

    for (std::size_t i = 3; i < positionalArgs.size(); ++i) {
        const std::string &arg = positionalArgs.at(i);
        if (arg == "--json") {
            opts->jsonOutput = true;
        } else if (arg == "--history") {
            opts->includeHistory = true;
        } else if (arg == "--allow-other-tenant") {
            opts->allowOtherTenant = true;
        } else if (arg == "--no-passphrase") {
            opts->noPassphrase = true;
        } else if (arg == "--password-stdin") {
            opts->passwordStdin = true;
        } else if (arg == "--socket") {
            if (i + 1 >= positionalArgs.size())
                return refuse("Missing value for --socket");
            opts->socketPath = positionalArgs.at(++i);
            opts->socketPathExplicit = true;
        } else if (!arg.starts_with("-") && opts->backupTarget.empty()) {
            opts->backupTarget = arg;
        } else if (!arg.starts_with("-") && opts->action == "export" && opts->backupOut.empty()) {
            opts->backupOut = arg;
        } else {
            return refuse("Unknown argument: " + arg);
        }
    }
    const bool needsTarget = opts->action != "list" && opts->action != "passphrase" && opts->action != "create";
    if (needsTarget && opts->backupTarget.empty())
        return refuse("backup " + opts->action + " needs a backup id or a file");
    if (opts->action == "export" && opts->backupOut.empty())
        return refuse("backup export needs the file to write");
    return true;
}

bool parseCliOptions(const std::vector<std::string> &args, CliOptions *optsOut, std::string *errorOut)
{
    if (!optsOut)
        return false;
    if (args.size() < 2) {
        if (errorOut)
            *errorOut = "Invalid command";
        return false;
    }

    CliOptions opts;
    std::vector<std::string> positionalArgs;
    std::string positionalPluginType;
    positionalArgs.reserve(args.size());
    positionalArgs.push_back(args.at(0));

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &arg = args.at(i);
        if (arg == "--tenant") {
            if (i + 1 >= args.size()) {
                if (errorOut)
                    *errorOut = "Missing value for --tenant";
                return false;
            }
            opts.tenant = str::trimmed(args.at(++i));
            continue;
        }
        if (arg.starts_with("--tenant=")) {
            opts.tenant = str::trimmed(arg.substr(std::string_view("--tenant=").size()));
            continue;
        }
        positionalArgs.push_back(arg);
    }

    if (positionalArgs.size() < 2) {
        if (errorOut)
            *errorOut = "Invalid command";
        return false;
    }

    if (positionalArgs.at(1) == "discover") {
        opts.scope = "discover";
        opts.action = "discover";
    } else {
        if (positionalArgs.size() < 3) {
            if (errorOut)
                *errorOut = "Invalid command";
            return false;
        }
        opts.scope = positionalArgs.at(1);
        opts.action = positionalArgs.at(2);
    }

    if (opts.scope == "discover") {
        for (std::size_t i = 2; i < positionalArgs.size(); ++i) {
            const std::string &arg = positionalArgs.at(i);
            if (arg == "--json") {
                opts.jsonOutput = true;
                continue;
            }
            if (arg == "--socket") {
                if (i + 1 >= positionalArgs.size()) {
                    if (errorOut)
                        *errorOut = "Missing value for --socket";
                    return false;
                }
                opts.socketPath = positionalArgs.at(++i);
                opts.socketPathExplicit = true;
                continue;
            }
            if (errorOut)
                *errorOut = "Unknown argument: " + arg;
            return false;
        }

        if (!isValidTenant(opts.tenant)) {
            if (errorOut)
                *errorOut = "Invalid tenant: " + opts.tenant;
            return false;
        }
        if (!opts.socketPathExplicit)
            opts.socketPath = defaultSocketPathForTenant(opts.tenant);

        *optsOut = opts;
        return true;
    }

    if (opts.scope == "radio" || opts.scope == "backend" || opts.scope == "user" || opts.scope == "backup"
        || opts.scope == "channel") {
        const bool parsed = opts.scope == "user"      ? parseUserScope(positionalArgs, &opts, errorOut)
                            : opts.scope == "backup"  ? parseBackupScope(positionalArgs, &opts, errorOut)
                            : opts.scope == "channel" ? parseChannelScope(positionalArgs, &opts, errorOut)
                                                      : parseHostScope(positionalArgs, &opts, errorOut);
        if (!parsed)
            return false;

        if (!isValidTenant(opts.tenant)) {
            if (errorOut)
                *errorOut = "Invalid tenant: " + opts.tenant;
            return false;
        }
        if (!opts.socketPathExplicit)
            opts.socketPath = defaultSocketPathForTenant(opts.tenant);

        *optsOut = opts;
        return true;
    }

    if (opts.scope != "adapter" && opts.scope != "transport") {
        if (errorOut)
            *errorOut = "Unknown scope: " + opts.scope;
        return false;
    }

    if (opts.scope == "transport") {
        if (opts.action != "start" && opts.action != "stop" && opts.action != "restart"
            && opts.action != "reload") {
            if (errorOut)
                *errorOut = "Unknown action: " + opts.action;
            return false;
        }

        for (std::size_t i = 3; i < positionalArgs.size(); ++i) {
            const std::string &arg = positionalArgs.at(i);
            if (arg == "--socket") {
                if (i + 1 >= positionalArgs.size()) {
                    if (errorOut)
                        *errorOut = "Missing value for --socket";
                    return false;
                }
                opts.socketPath = positionalArgs.at(++i);
                opts.socketPathExplicit = true;
                continue;
            }
            if (!arg.starts_with("-") && positionalPluginType.empty()) {
                positionalPluginType = arg;
                continue;
            }
            if (errorOut)
                *errorOut = "Unknown argument: " + arg;
            return false;
        }

        if (opts.selectorType == AdapterSelectorType::None && !positionalPluginType.empty()) {
            opts.selectorType = AdapterSelectorType::ByPluginType;
            opts.selectorValue = positionalPluginType;
        }

        if (!isValidTenant(opts.tenant)) {
            if (errorOut)
                *errorOut = "Invalid tenant: " + opts.tenant;
            return false;
        }
        if (!opts.socketPathExplicit)
            opts.socketPath = defaultSocketPathForTenant(opts.tenant);

        if (opts.selectorType != AdapterSelectorType::ByPluginType || opts.selectorValue.empty()) {
            if (errorOut)
                *errorOut = "transport " + opts.action + " requires <plugin>";
            return false;
        }
        if (opts.all || opts.jsonOutput) {
            if (errorOut)
                *errorOut = "transport commands do not support --all or --json";
            return false;
        }

        *optsOut = opts;
        return true;
    }

    if (opts.action != "list" && opts.action != "discover" && opts.action != "start"
        && opts.action != "stop" && opts.action != "restart" && opts.action != "reload"
        && opts.action != "rescan" && opts.action != "invoke") {
        if (errorOut)
            *errorOut = "Unknown action: " + opts.action;
        return false;
    }

    for (std::size_t i = 3; i < positionalArgs.size(); ++i) {
        const std::string &arg = positionalArgs.at(i);
        if (arg == "--json") {
            opts.jsonOutput = true;
            continue;
        }
        if (arg == "--all") {
            opts.all = true;
            continue;
        }
        if (arg == "--socket") {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = "Missing value for --socket";
                return false;
            }
            opts.socketPath = positionalArgs.at(++i);
            opts.socketPathExplicit = true;
            continue;
        }
        if (arg == "--id") {
            if (i + 1 >= positionalArgs.size() || !parseInt(positionalArgs.at(i + 1), &opts.adapterId)) {
                if (errorOut)
                    *errorOut = "Invalid value for --id";
                return false;
            }
            opts.selectorType = AdapterSelectorType::ById;
            ++i;
            continue;
        }
        if (arg == "--external-id") {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = "Missing value for --external-id";
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByExternalId;
            opts.selectorValue = positionalArgs.at(++i);
            continue;
        }
        if (arg == "--name") {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = "Missing value for --name";
                return false;
            }
            opts.selectorType = AdapterSelectorType::ByName;
            opts.selectorValue = positionalArgs.at(++i);
            continue;
        }
        if (arg == "--params") {
            if (i + 1 >= positionalArgs.size()) {
                if (errorOut)
                    *errorOut = "Missing value for --params";
                return false;
            }
            opts.invokeParamsJson = positionalArgs.at(++i);
            continue;
        }
        if (!arg.starts_with("-") && positionalPluginType.empty()) {
            positionalPluginType = arg;
            continue;
        }
        if (errorOut)
            *errorOut = "Unknown argument: " + arg;
        return false;
    }

    if (opts.action == "invoke") {
        // For invoke the positional argument is the action id, not a plugin type.
        opts.invokeActionId = str::trimmed(positionalPluginType);
        positionalPluginType.clear();
    }

    if (opts.selectorType == AdapterSelectorType::None && !positionalPluginType.empty()) {
        opts.selectorType = AdapterSelectorType::ByPluginType;
        opts.selectorValue = positionalPluginType;
    }

    if (opts.action == "list") {
        if (opts.selectorType != AdapterSelectorType::None || opts.all) {
            if (errorOut)
                *errorOut = "adapter list does not support selectors or --all";
            return false;
        }
    } else if (opts.action == "rescan") {
        // Nothing to select. The whole point of it is that it takes no
        // argument: dpkg has just put something in the plugin directory and
        // core is being told to look, not told what to look for.
        if (opts.selectorType != AdapterSelectorType::None || opts.all) {
            if (errorOut)
                *errorOut = "adapter rescan does not support selectors or --all";
            return false;
        }
    } else if (opts.action == "discover") {
        if (opts.selectorType != AdapterSelectorType::None
            && opts.selectorType != AdapterSelectorType::ByPluginType) {
            if (errorOut)
                *errorOut = "adapter discover supports at most one optional <plugin> filter";
            return false;
        }
        if (opts.all) {
            if (errorOut)
                *errorOut = "adapter discover does not support --all";
            return false;
        }
    } else if (opts.action == "reload") {
        if (opts.selectorType != AdapterSelectorType::ByPluginType || opts.selectorValue.empty()) {
            if (errorOut)
                *errorOut = "adapter reload requires <plugin>";
            return false;
        }
        if (opts.all || opts.jsonOutput) {
            if (errorOut)
                *errorOut = "adapter reload does not support --all or --json";
            return false;
        }
    } else if (opts.action == "invoke") {
        if (opts.invokeActionId.empty()) {
            if (errorOut)
                *errorOut = "adapter invoke requires <actionId>";
            return false;
        }
        if (opts.selectorType != AdapterSelectorType::ById
            && opts.selectorType != AdapterSelectorType::ByExternalId
            && opts.selectorType != AdapterSelectorType::ByName) {
            if (errorOut)
                *errorOut = "adapter invoke requires one selector (--id, --external-id, or --name)";
            return false;
        }
        if (opts.all) {
            if (errorOut)
                *errorOut = "adapter invoke does not support --all";
            return false;
        }
    } else {
        if (opts.selectorType == AdapterSelectorType::None) {
            if (errorOut)
                *errorOut = "adapter " + opts.action
                    + " requires one selector (--id, --external-id, --name, or <plugin> with --all)";
            return false;
        }
        if (opts.selectorType == AdapterSelectorType::ByPluginType) {
            if (!opts.all) {
                if (errorOut)
                    *errorOut = "<plugin> is only allowed with --all for adapter " + opts.action;
                return false;
            }
        } else if (opts.all) {
            if (errorOut)
                *errorOut = "--all is only supported with <plugin> for adapter " + opts.action;
            return false;
        }
        if (opts.jsonOutput) {
            if (errorOut)
                *errorOut = "--json is only supported with adapter list or adapter discover";
            return false;
        }
    }

    if (!isValidTenant(opts.tenant)) {
        if (errorOut)
            *errorOut = "Invalid tenant: " + opts.tenant;
        return false;
    }
    if (!opts.socketPathExplicit)
        opts.socketPath = defaultSocketPathForTenant(opts.tenant);

    *optsOut = opts;
    return true;
}

// ------------------------------------------------------------------ identity

bool dropRootToPhi(std::string *errorOut)
{
    if (geteuid() != 0)
        return true;

    struct passwd *pw = getpwnam("phi");
    if (!pw) {
        if (errorOut)
            *errorOut = "User 'phi' not found; cannot drop root privileges";
        return false;
    }

    if (setgid(pw->pw_gid) != 0) {
        if (errorOut)
            *errorOut = std::string("Failed to drop group: ") + std::strerror(errno);
        return false;
    }

    if (setgroups(1, &pw->pw_gid) != 0) {
        if (errorOut)
            *errorOut = std::string("Failed to reset supplementary groups: ") + std::strerror(errno);
        return false;
    }

    if (setuid(pw->pw_uid) != 0) {
        if (errorOut)
            *errorOut = std::string("Failed to drop user: ") + std::strerror(errno);
        return false;
    }

    if (setuid(0) == 0) {
        if (errorOut)
            *errorOut = "Unexpectedly able to regain root privileges; aborting";
        return false;
    }

    return true;
}

// ------------------------------------------------------------------ passwords

/// The UI's floor, so an account set up here can also be changed there.
constexpr std::size_t kMinPasswordChars = 8;

/// Characters, not bytes: "ä" is one character to whoever typed it.
std::size_t utf8Length(const std::string &text)
{
    std::size_t count = 0;
    for (const unsigned char c : text) {
        if ((c & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

/// One line from stdin with its line ending removed. With `prompt`, the
/// terminal stops echoing while it is typed.
bool readSecretLine(const char *prompt, std::string *out)
{
    termios saved{};
    const bool tty = prompt != nullptr && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (tty) {
        std::cerr << prompt << std::flush;
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    const bool ok = static_cast<bool>(std::getline(std::cin, *out));
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::cerr << "\n";
    }
    if (ok && !out->empty() && out->back() == '\r')
        out->pop_back();
    return ok;
}

/// The new password: typed twice at a terminal, or one line from a pipe.
bool readNewPassword(const CliOptions &opts, std::string *out, std::string *errorOut)
{
    const auto refuse = [errorOut](const std::string &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    if (opts.passwordStdin || !isatty(STDIN_FILENO)) {
        if (!readSecretLine(nullptr, out))
            return refuse("No password on stdin");
    } else {
        std::string again;
        const std::string prompt = "New password for " + opts.username + ": ";
        if (!readSecretLine(prompt.c_str(), out) || !readSecretLine("Repeat it: ", &again))
            return refuse("No password entered");
        if (*out != again)
            return refuse("The two passwords differ");
    }

    if (utf8Length(*out) < kMinPasswordChars)
        return refuse("A password needs at least " + str::number(static_cast<int>(kMinPasswordChars))
                      + " characters");
    return true;
}

std::vector<int> resolveAdapterIds(const CliOptions &opts, const Json &adapters, std::string *errorOut)
{
    std::vector<int> ids;

    if (opts.selectorType == AdapterSelectorType::ById) {
        ids.push_back(opts.adapterId);
        return ids;
    }

    const auto collectBy = [&](std::string_view key) {
        for (const Json &entry : adapters) {
            if (jsonString(entry, key) != opts.selectorValue)
                continue;
            const auto id = jsonInt(jsonValue(entry, "id"));
            if (id.has_value() && *id > 0)
                ids.push_back(*id);
        }
    };

    if (opts.selectorType == AdapterSelectorType::ByExternalId) {
        collectBy("externalId");
        if (ids.size() != 1 && errorOut) {
            *errorOut = ids.empty()
                ? "No adapter found for externalId '" + opts.selectorValue + "'"
                : "Multiple adapters found for externalId '" + opts.selectorValue + "'";
        }
        return ids;
    }

    if (opts.selectorType == AdapterSelectorType::ByName) {
        collectBy("name");
        if (ids.size() != 1 && errorOut) {
            *errorOut = ids.empty()
                ? "No adapter found for name '" + opts.selectorValue + "'"
                : "ambiguous_adapter_name: '" + opts.selectorValue + "' matches "
                      + str::number(ids.size()) + " adapters";
        }
        return ids;
    }

    if (opts.selectorType == AdapterSelectorType::ByPluginType) {
        collectBy("pluginType");
        if (ids.empty() && errorOut)
            *errorOut = "No adapters found for pluginType '" + opts.selectorValue + "'";
    }

    return ids;
}


// ------------------------------------------------------------------ backup

constexpr std::size_t kBackupPieceBytes = 512 * 1024;
constexpr int kBackupWaitMs = 15 * 60 * 1000;

/// The files a backup command reads or writes, opened while still root: the
/// one an admin names is usually theirs, not phi's.
struct BackupFiles {
    std::FILE *in = nullptr;
    std::FILE *out = nullptr;
    ~BackupFiles()
    {
        if (in)
            std::fclose(in);
        if (out)
            std::fclose(out);
    }
};

bool isFileTarget(const std::string &target)
{
    struct stat st {};
    return target.find('/') != std::string::npos || (::stat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode));
}

bool openBackupFiles(const CliOptions &opts, BackupFiles *files, std::string *errorOut)
{
    const bool reads = opts.action == "show" || opts.action == "import"
                       || ((opts.action == "inspect" || opts.action == "restore") && isFileTarget(opts.backupTarget));
    if (reads) {
        files->in = std::fopen(opts.backupTarget.c_str(), "rb");
        if (!files->in) {
            *errorOut = "Cannot read " + opts.backupTarget + ": " + std::strerror(errno);
            return false;
        }
    }
    if (opts.action == "export") {
        const int fd = ::open(opts.backupOut.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || !(files->out = ::fdopen(fd, "wb"))) {
            if (fd >= 0)
                ::close(fd);
            *errorOut = "Cannot create " + opts.backupOut + ": " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

std::uint64_t readBe(const unsigned char *in, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i)
        value = (value << 8) | in[i];
    return value;
}

/// The clear header of a .phibackup file: what `show` prints without a key.
std::optional<Json> readBackupHeader(std::FILE *file, std::string *errorOut)
{
    char magic[8];
    unsigned char footer[20];
    if (::fseeko(file, 0, SEEK_END) != 0) {
        *errorOut = "Cannot read the file";
        return std::nullopt;
    }
    const off_t size = ::ftello(file);
    if (size < 28 || ::fseeko(file, 0, SEEK_SET) != 0 || std::fread(magic, 1, 8, file) != 8
        || std::memcmp(magic, "PHIBAK01", 8) != 0) {
        *errorOut = "Not a backup file";
        return std::nullopt;
    }
    if (::fseeko(file, size - 20, SEEK_SET) != 0 || std::fread(footer, 1, 20, file) != 20
        || std::memcmp(footer + 12, "PHIBAKFT", 8) != 0) {
        *errorOut = "The backup file is incomplete";
        return std::nullopt;
    }
    const std::uint64_t offset = readBe(footer, 8);
    const std::uint64_t length = readBe(footer + 8, 4);
    if (offset < 8 || length > 4 * 1024 * 1024 || offset + length != static_cast<std::uint64_t>(size) - 20) {
        *errorOut = "The backup file is damaged";
        return std::nullopt;
    }
    std::string text(static_cast<std::size_t>(length), '\0');
    if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0
        || std::fread(text.data(), 1, text.size(), file) != text.size()) {
        *errorOut = "The backup file is damaged";
        return std::nullopt;
    }
    Json header = parseJson(text);
    if (!header.is_object() || jsonString(header, "format") != "phibackup") {
        *errorOut = "The backup file is damaged";
        return std::nullopt;
    }
    return header;
}

std::string utcTime(long long ms)
{
    if (ms <= 0)
        return "-";
    const std::time_t sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return buf;
}

std::string byteSize(long long bytes)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 3) {
        value /= 1024;
        ++unit;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buf;
}

long long jsonLong(const Json &obj, std::string_view key)
{
    const Json value = jsonValue(obj, key);
    return value.is_number() ? value.get<long long>() : 0;
}

void printBackupSummary(const Json &header)
{
    std::cout << "Created  " << utcTime(jsonLong(header, "createdMs")) << "\n"
              << "Host     " << jsonString(header, "host") << "\n"
              << "Core     " << jsonString(header, "coreVersion") << ", schema "
              << jsonLong(header, "schemaVersion") << "\n\n";
}

void printBackupHeader(const Json &header)
{
    printBackupSummary(header);
    std::cout << cell("PART", 16) << cell("KIND", 10) << cell("SIZE", 12) << "HISTORY\n";
    for (const Json &part : jsonArray(header, "parts")) {
        const Json history = jsonValue(part, "includesHistory");
        std::cout << cell(jsonString(part, "id"), 16) << cell(jsonString(part, "kind"), 10)
                  << cell(byteSize(jsonLong(part, "length")), 12)
                  << (history.is_boolean() ? boolText(history.get<bool>()) : std::string("-")) << "\n";
    }
    std::vector<std::string> labels;
    for (const Json &recipient : jsonArray(header, "recipients"))
        labels.push_back(jsonString(recipient, "label"));
    std::cout << "\nOpened by the passphrase of: " << str::join(labels, ", ") << "\n";
}

bool sha256OfFile(std::FILE *file, std::uint64_t *size, std::string *hex)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 && ::fseeko(file, 0, SEEK_SET) == 0;
    std::vector<char> buffer(64 * 1024);
    *size = 0;
    while (ok) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), file);
        if (n == 0)
            break;
        ok = EVP_DigestUpdate(ctx, buffer.data(), n) == 1;
        *size += n;
    }
    unsigned char digest[32];
    unsigned int len = 0;
    ok = ok && !std::ferror(file) && EVP_DigestFinal_ex(ctx, digest, &len) == 1 && len == 32;
    EVP_MD_CTX_free(ctx);
    if (ok)
        *hex = str::toHex(std::string_view(reinterpret_cast<const char *>(digest), 32));
    return ok;
}

/// Sends the file to the tenant's core in pieces; the backupId it became.
std::optional<std::string> uploadBackup(const CliOptions &opts, std::FILE *file, std::string *errorOut)
{
    if (!readBackupHeader(file, errorOut))
        return std::nullopt;
    std::uint64_t size = 0;
    std::string hex;
    if (!sha256OfFile(file, &size, &hex)) {
        *errorOut = "Cannot read the file";
        return std::nullopt;
    }
    Json response;
    if (!sendCommand(opts.socketPath, "cmd.backup.upload.begin", Json{{"sizeBytes", size}, {"sha256", hex}},
                     &response, errorOut))
        return std::nullopt;
    const std::string uploadId = jsonString(response, "uploadId");
    std::vector<char> buffer(kBackupPieceBytes);
    std::uint64_t offset = 0;
    ::fseeko(file, 0, SEEK_SET);
    while (offset < size) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), file);
        if (n == 0) {
            *errorOut = "Cannot read the file";
            return std::nullopt;
        }
        const Json piece{{"uploadId", uploadId},
                         {"offset", offset},
                         {"data", str::toBase64(std::string_view(buffer.data(), n))}};
        if (!sendCommand(opts.socketPath, "cmd.backup.upload.chunk", piece, &response, errorOut))
            return std::nullopt;
        offset += n;
        if (jsonBool(response, "done"))
            return jsonString(response, "backupId");
    }
    *errorOut = "The upload did not finish";
    return std::nullopt;
}

/// One passphrase: a line from a pipe, or typed once at the terminal.
bool readPassphrase(const CliOptions &opts, const std::string &prompt, std::string *out, std::string *errorOut)
{
    const bool piped = opts.passwordStdin || !isatty(STDIN_FILENO);
    if (!readSecretLine(piped ? nullptr : prompt.c_str(), out)) {
        *errorOut = opts.passwordStdin || isatty(STDIN_FILENO)
                        ? "No passphrase entered"
                        : "No passphrase entered: there is no terminal to ask at; pipe it in with --password-stdin";
        return false;
    }
    return true;
}

int runBackupCommand(const CliOptions &opts, BackupFiles &files)
{
    std::string error;
    const auto fail = [&error]() {
        std::cerr << error << "\n";
        return 1;
    };

    if (opts.action == "show") {
        const auto header = readBackupHeader(files.in, &error);
        if (!header)
            return fail();
        if (opts.jsonOutput)
            std::cout << dumpIndented(*header);
        else
            printBackupHeader(*header);
        return 0;
    }

    Json response;
    if (opts.action == "list") {
        if (!sendCommand(opts.socketPath, "cmd.backups.list", Json::object(), &response, &error))
            return fail();
        if (opts.jsonOutput) {
            std::cout << dumpIndented(response);
            return 0;
        }
        std::cout << "Backup passphrase: " << (jsonBool(response, "passphraseSet") ? "set" : "not set") << "\n\n";
        std::cout << cell("BACKUP", 44) << cell("CREATED", 26) << cell("SIZE", 12) << "PARTS\n";
        for (const Json &entry : jsonArray(response, "backups")) {
            std::vector<std::string> parts;
            for (const Json &part : jsonArray(entry, "parts"))
                parts.push_back(jsonString(part, "id"));
            std::cout << cell(jsonString(entry, "backupId"), 44) << cell(utcTime(jsonLong(entry, "tsMs")), 26)
                      << cell(byteSize(jsonLong(entry, "sizeBytes")), 12) << str::join(parts, ", ") << "\n";
        }
        return 0;
    }

    if (opts.action == "passphrase") {
        if (!sendCommand(opts.socketPath, "cmd.backups.list", Json::object(), &response, &error))
            return fail();
        Json payload = Json::object();
        std::string current;
        if (jsonBool(response, "passphraseSet")) {
            if (!readPassphrase(opts, "Current backup passphrase: ", &current, &error))
                return fail();
            payload["oldPassphrase"] = current;
        }
        std::string next;
        if (!readPassphrase(opts, "New backup passphrase: ", &next, &error))
            return fail();
        if (!opts.passwordStdin && isatty(STDIN_FILENO)) {
            std::string again;
            if (!readPassphrase(opts, "Repeat it: ", &again, &error))
                return fail();
            if (again != next) {
                error = "The two passphrases differ";
                return fail();
            }
        }
        payload["passphrase"] = next;
        const bool ok = sendCommand(opts.socketPath, "cmd.backup.passphrase.set", payload, &response, &error);
        std::fill(next.begin(), next.end(), '\0');
        std::fill(current.begin(), current.end(), '\0');
        if (!ok)
            return fail();
        std::cout << "Backup passphrase set. Without it no backup of this tenant can be restored.\n";
        return 0;
    }

    if (opts.action == "create") {
        if (!sendCommand(opts.socketPath, "cmd.backup.create", Json{{"includeHistory", opts.includeHistory}},
                         &response, &error))
            return fail();
        const std::string backupId = jsonString(response, "backupId");
        std::cerr << "Making " << backupId << " ..." << std::flush;
        // The end comes as an event; a command line has none, so it asks.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBackupWaitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            ::usleep(500 * 1000);
            if (!sendCommand(opts.socketPath, "cmd.backups.list", Json::object(), &response, &error))
                return fail();
            if (jsonBool(response, "busy"))
                continue;
            std::cerr << "\n";
            for (const Json &entry : jsonArray(response, "backups")) {
                if (jsonString(entry, "backupId") == backupId) {
                    std::cout << backupId << " (" << byteSize(jsonLong(entry, "sizeBytes")) << ")\n";
                    return 0;
                }
            }
            error = "The backup failed; journalctl -u phi-core@" + opts.tenant + " says why";
            return fail();
        }
        error = "\nThe backup is still running";
        return fail();
    }

    std::string backupId = opts.backupTarget;
    if (opts.action == "import" || files.in) {
        const auto uploaded = uploadBackup(opts, files.in, &error);
        if (!uploaded)
            return fail();
        backupId = *uploaded;
        if (opts.action == "import") {
            std::cout << backupId << "\n";
            return 0;
        }
        std::cerr << "Uploaded as " << backupId << "\n";
    }

    if (opts.action == "delete") {
        if (!sendCommand(opts.socketPath, "cmd.backup.delete", Json{{"backupId", backupId}}, &response, &error))
            return fail();
        std::cout << "Deleted " << backupId << "\n";
        return 0;
    }

    if (opts.action == "export") {
        for (std::uint64_t offset = 0;;) {
            const Json ask{{"backupId", backupId}, {"offset", offset}, {"length", kBackupPieceBytes}};
            if (!sendCommand(opts.socketPath, "cmd.backup.read", ask, &response, &error))
                return fail();
            const std::string data = str::fromBase64(jsonString(response, "data"));
            if (std::fwrite(data.data(), 1, data.size(), files.out) != data.size()) {
                error = "Cannot write " + opts.backupOut;
                return fail();
            }
            offset += data.size();
            if (jsonBool(response, "eof") || data.empty())
                break;
        }
        if (std::fflush(files.out) != 0 || ::fsync(::fileno(files.out)) != 0) {
            error = "Cannot write " + opts.backupOut;
            return fail();
        }
        std::cout << "Wrote " << opts.backupOut << "\n";
        return 0;
    }

    std::string passphrase;
    if (!(opts.action == "inspect" && opts.noPassphrase)
        && !readPassphrase(opts, "Backup passphrase: ", &passphrase, &error))
        return fail();

    if (opts.action == "inspect") {
        const bool ok = sendCommand(opts.socketPath, "cmd.backup.inspect",
                                    Json{{"backupId", backupId}, {"passphrase", passphrase}}, &response, &error);
        std::fill(passphrase.begin(), passphrase.end(), '\0');
        if (!ok)
            return fail();
        response.erase("accepted");
        response.erase("error");
        if (opts.jsonOutput) {
            std::cout << dumpIndented(response);
            return 0;
        }
        printBackupSummary(response);
        for (const Json &part : jsonArray(response, "parts")) {
            std::cout << jsonString(part, "id") << " (" << jsonString(part, "kind") << ")";
            const Json opens = jsonValue(part, "opens");
            if (opens.is_boolean())
                std::cout << (opens.get<bool>() ? ": opens" : ": does not open with this passphrase");
            std::cout << "\n";
            for (const Json &file : jsonArray(jsonObject(part, "manifest"), "files"))
                std::cout << "  " << scalarText(file) << "\n";
        }
        if (!jsonString(response, "problem").empty())
            std::cout << "\nCannot be restored here: " << jsonString(response, "problem") << "\n";
        else if (!jsonString(response, "applies").empty())
            std::cout << "\nA restore here applies " << jsonString(response, "applies")
                      << (jsonBool(response, "otherTenant") ? " (another tenant's: --allow-other-tenant)" : "")
                      << "\n";
        return 0;
    }

    // restore
    const bool ok = sendCommand(opts.socketPath, "cmd.backup.restore",
                                Json{{"backupId", backupId},
                                     {"passphrase", passphrase},
                                     {"allowOtherTenant", opts.allowOtherTenant}},
                                &response, &error);
    std::fill(passphrase.begin(), passphrase.end(), '\0');
    if (!ok)
        return fail();
    std::cout << "Restore of " << backupId << " staged; phi-core@" << opts.tenant
              << " restarts and swaps it in. The replaced state stays in /var/lib/phi/@" << opts.tenant
              << "/restore/.\n";
    return 0;
}

} // namespace

/// The box backup is a root tool of phi-core's (docs/backup-concept.md, P2):
/// it reads every tenant and the host, so it keeps root, which this program
/// drops before anything else.
constexpr const char *kBoxBackupTool = "/usr/libexec/phi/phi-box-backup";

[[noreturn]] void runBoxBackup(const std::vector<std::string> &args)
{
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(kBoxBackupTool));
    for (std::size_t i = 2; i < args.size(); ++i)
        argv.push_back(const_cast<char *>(args[i].c_str()));
    argv.push_back(nullptr);
    ::execv(kBoxBackupTool, argv.data());
    std::cerr << "Cannot run " << kBoxBackupTool << ": " << std::strerror(errno) << " (is phi-core installed?)\n";
    std::exit(1);
}

int main(int argc, char **argv)
{
    const std::vector<std::string> args(argv, argv + argc);
    if (args.size() > 2 && args[1] == "backup"
        && std::any_of(args.begin() + 2, args.end(), [](const std::string &a) { return a == "--box" || a == "--host"; }))
        runBoxBackup(args);
    CliOptions opts;
    std::string parseError;
    if (!parseCliOptions(args, &opts, &parseError)) {
        if (!parseError.empty())
            std::cerr << parseError << "\n";
        printUsage();
        return 2;
    }

    // A backup file is opened before root is dropped: the one an admin names
    // is theirs, and phi may not read it.
    BackupFiles backupFiles;
    if (opts.scope == "backup") {
        std::string openError;
        if (!openBackupFiles(opts, &backupFiles, &openError)) {
            std::cerr << openError << "\n";
            return 1;
        }
    }

    std::string dropError;
    if (!dropRootToPhi(&dropError)) {
        std::cerr << dropError << "\n";
        return 1;
    }

    if (opts.scope == "backup")
        return runBackupCommand(opts, backupFiles);

    if (opts.scope == "channel") {
        Json payload = Json::object();
        payload["channelId"] = opts.channelId;
        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.channel.remove", payload, &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Channel " << opts.channelId << " removed\n";
        return 0;
    }

    if (opts.scope == "user") {
        if (opts.action == "list") {
            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.users.list", Json::object(), &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            const Json users = jsonArray(response, "users");
            if (opts.jsonOutput)
                std::cout << dumpIndented(users);
            else
                printUserTable(users);
            return 0;
        }

        std::string password;
        std::string error;
        if (!readNewPassword(opts, &password, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        phicore::cli::ScramVerifier verifier;
        const bool derived = phicore::cli::deriveScramVerifier(
            password, phicore::cli::kScramIterations, &verifier, &error);
        std::fill(password.begin(), password.end(), '\0');
        if (!derived) {
            std::cerr << error << "\n";
            return 1;
        }

        Json payload = Json::object();
        payload["username"] = opts.username;
        payload["salt"] = verifier.salt;
        payload["iterations"] = verifier.iterations;
        payload["storedKey"] = verifier.storedKey;
        payload["serverKey"] = verifier.serverKey;
        Json response;
        if (!sendCommand(opts.socketPath, "cmd.users.password.set", payload, &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Password set for " << jsonString(response, "username")
                  << "; their sessions have ended\n";
        return 0;
    }

    // The host layer first, because `list` means one thing under `radio` and
    // `backend` and another under `adapter`, and the adapter branch below reads
    // the action without its scope.
    if (opts.scope == "radio") {
        if (opts.action == "list") {
            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.host.radio.list", Json::object(),
                             &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            const Json radios = jsonArray(response, "radios");
            if (opts.jsonOutput)
                std::cout << dumpIndented(radios);
            else
                printRadioTable(radios);
            return 0;
        }

        Json payload = Json::object();
        payload["resourceId"] = opts.resourceId;
        if (opts.action == "claim")
            payload["claimant"] = opts.claimant;

        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.host.radio." + opts.action, payload,
                         &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }

        if (opts.jsonOutput) {
            std::cout << dumpIndented(response);
            return 0;
        }

        if (opts.action == "probe") {
            const std::string family = jsonString(response, "radioFamily");
            // A port that answers nothing anybody here understands is the
            // interesting case rather than a failure: the raw answer is printed
            // so that whoever plugged the stick in can say what it was.
            std::cout << opts.resourceId << ": "
                      << (family.empty() ? "answered nothing this host recognises" : family) << "\n";
            const std::string answer = jsonString(response, "answer");
            if (!answer.empty())
                std::cout << "answer: " << answer << "\n";
            return 0;
        }

        if (opts.action == "claim") {
            std::cout << opts.resourceId << " is held by " << opts.claimant << " for tenant "
                      << jsonString(response, "tenant") << "\n";
            return 0;
        }

        std::cout << opts.resourceId << " released\n";
        return 0;
    }

    if (opts.scope == "backend") {
        if (opts.action == "list") {
            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.host.backends.list", Json::object(),
                             &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            if (opts.jsonOutput) {
                std::cout << dumpIndented(response);
                return 0;
            }
            printBackendTable(jsonArray(response, "backends"));
            printBackendKinds(jsonArray(response, "kinds"));
            return 0;
        }

        if (opts.action == "intents") {
            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.host.intents.list", Json::object(),
                             &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            const Json intents = jsonArray(response, "intents");
            if (opts.jsonOutput) {
                std::cout << dumpIndented(intents);
                return 0;
            }
            if (intents.empty()) {
                std::cout << "No package on this host asked for a backend.\n";
                return 0;
            }
            printIntentTable(intents);
            return 0;
        }

        if (opts.action == "create") {
            Json payload = Json::object();
            payload["kind"] = opts.backendKind;
            payload["id"] = opts.backendId;
            payload["resourceId"] = opts.resourceId;

            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.host.backend.create", payload,
                             &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            if (opts.jsonOutput) {
                std::cout << dumpIndented(response);
                return 0;
            }

            const Json backend = jsonObject(response, "backend");
            std::cout << jsonString(backend, "name") << " created on "
                      << jsonString(backend, "resourceId") << "\n";
            const std::string unit = jsonString(backend, "unit");
            if (!unit.empty())
                std::cout << "unit: " << unit << " (" << jsonString(backend, "state") << ")\n";
            // The backend exists and holds its radio, so this is a report on
            // the unit rather than on the command: the record is the intention,
            // and a runtime that will not come up today is brought up when
            // whatever is wrong is fixed.
            const std::string note = jsonString(response, "note");
            if (!note.empty())
                std::cerr << note << "\n";
            return 0;
        }

        if (opts.action == "remove") {
            Json payload = Json::object();
            payload["name"] = opts.backendKind + "@" + opts.backendId;

            Json response;
            std::string error;
            if (!sendCommand(opts.socketPath, "cmd.host.backend.remove", payload,
                             &response, &error)) {
                std::cerr << error << "\n";
                return 1;
            }
            std::cout << jsonString(response, "name") << " removed\n";
            const std::string note = jsonString(response, "note");
            if (!note.empty())
                std::cerr << note << "\n";
            return 0;
        }

        Json payload = Json::object();
        payload["kind"] = opts.backendKind;
        payload["id"] = opts.backendId;

        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.host.backend." + opts.action, payload,
                         &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << jsonString(response, "backend") << " " << opts.action << ": "
                  << jsonString(response, "state") << "\n";
        return 0;
    }

    if (opts.action == "list") {
        Json adapters;
        std::string error;
        if (!fetchAdapters(opts.socketPath, &adapters, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (opts.jsonOutput)
            std::cout << dumpIndented(adapters);
        else
            printAdapterTable(adapters);
        return 0;
    }

    if (opts.scope == "discover") {
        Json candidates;
        std::string error;
        if (!runDiscoveryStream(opts.socketPath, "network.discover", {}, &candidates, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (opts.jsonOutput)
            std::cout << dumpIndented(candidates);
        else
            printDiscoveryTable(candidates);
        return 0;
    }

    if (opts.action == "discover") {
        std::vector<std::string> pluginTypes;
        if (opts.selectorType == AdapterSelectorType::ByPluginType && !opts.selectorValue.empty())
            pluginTypes.push_back(opts.selectorValue);

        Json candidates;
        std::string error;
        if (!runDiscoveryStream(opts.socketPath, "adapter.discover", pluginTypes, &candidates, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        if (opts.jsonOutput)
            std::cout << dumpIndented(candidates);
        else
            printDiscoveryTable(candidates);
        return 0;
    }

    if (opts.scope == "transport") {
        Json payload = Json::object();
        payload["pluginType"] = opts.selectorValue;
        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.transport." + opts.action, payload, &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Transport " << opts.action << " completed for pluginType '"
                  << opts.selectorValue << "'\n";
        return 0;
    }

    if (opts.action == "rescan") {
        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.adapter.plugins.rescan", Json::object(),
                         &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Plugin directories read again: " << jsonArray(response, "factories").size()
                  << " adapter types available\n";
        return 0;
    }

    if (opts.action == "reload") {
        Json payload = Json::object();
        payload["pluginType"] = opts.selectorValue;
        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.adapter.reload", payload, &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }
        std::cout << "Reload triggered for pluginType '" << opts.selectorValue << "'\n";
        return 0;
    }

    if (opts.action == "invoke") {
        Json params = Json::object();
        if (!str::trimmed(opts.invokeParamsJson).empty()) {
            const Json parsed = parseJson(opts.invokeParamsJson);
            if (!parsed.is_object()) {
                std::cerr << "Invalid --params: expected a JSON object\n";
                return 2;
            }
            params = parsed;
        }

        Json adapters;
        std::string listError;
        if (!fetchAdapters(opts.socketPath, &adapters, &listError)) {
            std::cerr << listError << "\n";
            return 1;
        }
        std::string resolveError;
        const std::vector<int> ids = resolveAdapterIds(opts, adapters, &resolveError);
        if (!resolveError.empty()) {
            std::cerr << resolveError << "\n";
            return 1;
        }
        if (ids.size() != 1) {
            std::cerr << "adapter invoke requires exactly one matching adapter (matched "
                      << ids.size() << ")\n";
            return 1;
        }

        Json payload = Json::object();
        payload["scope"] = "instance";
        payload["adapterId"] = ids.front();
        payload["actionId"] = opts.invokeActionId;
        payload["params"] = params;

        Json response;
        std::string error;
        if (!sendCommand(opts.socketPath, "cmd.adapter.action.invoke", payload, &response, &error)) {
            std::cerr << error << "\n";
            return 1;
        }

        const int status = jsonInt(jsonValue(response, "status")).value_or(-1);
        if (opts.jsonOutput) {
            std::cout << dumpIndented(response);
        } else {
            const std::string statusName = jsonString(response, "statusName");
            std::cout << "status: " << (statusName.empty() ? "unknown" : statusName)
                      << " (" << status << ")\n";
            const Json resultValue = jsonValue(response, "resultValue");
            if (!resultValue.is_null()) {
                if (resultValue.is_object() || resultValue.is_array())
                    std::cout << "result: " << dumpCompact(resultValue) << "\n";
                else
                    std::cout << "result: " << scalarText(resultValue) << "\n";
            }
            const Json errorValue = jsonValue(response, "error");
            if (errorValue.is_object())
                std::cout << "error: " << errorText(errorValue) << "\n";
        }
        return status == 0 ? 0 : 1;
    }

    Json adapters;
    std::string listError;
    if (!fetchAdapters(opts.socketPath, &adapters, &listError)) {
        std::cerr << listError << "\n";
        return 1;
    }

    std::string resolveError;
    const std::vector<int> ids = resolveAdapterIds(opts, adapters, &resolveError);
    if (!resolveError.empty()) {
        std::cerr << resolveError << "\n";
        return 1;
    }
    if (ids.empty()) {
        std::cerr << "No adapter ids resolved\n";
        return 1;
    }

    const std::string topic = "cmd.adapter." + opts.action;
    int okCount = 0;
    int failCount = 0;
    std::vector<std::string> failures;
    for (const int id : ids) {
        Json payload = Json::object();
        payload["adapterId"] = id;
        Json response;
        std::string error;
        if (sendCommand(opts.socketPath, topic, payload, &response, &error)) {
            ++okCount;
        } else {
            ++failCount;
            failures.push_back("adapterId=" + str::number(id) + ": " + error);
        }
    }

    std::cout << opts.action << " completed: ok=" << okCount << " failed=" << failCount << "\n";
    for (const std::string &failure : failures)
        std::cerr << failure << "\n";
    return failCount == 0 ? 0 : 1;
}
