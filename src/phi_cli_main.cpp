// phi-cli: the local client for the tenant's CLI transport socket.
//
// Qt-free (the Qt exit, last consumer of Qt6 in this repo): JSON is nlohmann,
// like the transport plugin beside it; the envelope types, the response topics
// and the cid rules come from phi/transport/api/envelope.h rather than from
// string literals typed a second time here; and the socket is cli_socket.h,
// which is QLocalSocket's waitFor... trio written as poll().

#include "cli_socket.h"

#include <envelope.h>
#include <nlohmann/json.hpp>
#include <phi/runtime/str.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
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

std::optional<tapi::CmdId> readCid(const Json &value)
{
    // The contract's own rules, rather than a second reading of them here.
    if (value.is_number())
        return tapi::cidFromNumber(value.get<double>());
    if (value.is_string())
        return tapi::cidFromString(value.get<std::string>());
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
        << "  phi-cli transport start|stop|restart|reload <plugin> [--tenant <tenant>] [--socket <path>]\n";
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

std::string responseErrorMessage(const Json &payload)
{
    const Json errorValue = jsonValue(payload, "error");
    if (errorValue.is_object()) {
        const std::string msg = jsonString(errorValue, "msg");
        if (!msg.empty())
            return msg;
        return jsonString(errorValue, "message");
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

    bool ackSeen = false;
    while (true) {
        std::string line;
        const auto read = socket.readLine(kResponseTimeoutMs, &line, errorOut);
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

        const auto responseCid = readCid(jsonValue(envelope, "cid"));
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

        const auto responseCid = readCid(jsonValue(envelope, "cid"));
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
};

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

} // namespace

int main(int argc, char **argv)
{
    std::string dropError;
    if (!dropRootToPhi(&dropError)) {
        std::cerr << dropError << "\n";
        return 1;
    }

    const std::vector<std::string> args(argv, argv + argc);
    CliOptions opts;
    std::string parseError;
    if (!parseCliOptions(args, &opts, &parseError)) {
        if (!parseError.empty())
            std::cerr << parseError << "\n";
        printUsage();
        return 2;
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
                std::cout << "error: " << jsonString(errorValue, "message") << "\n";
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
