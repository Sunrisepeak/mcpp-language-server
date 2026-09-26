module mcppls.spec.discovery;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.process;
import mcppls.platform.toolrun;
import mcppls.spec.database;

namespace mcppls::spec {

namespace {

std::vector<std::string> string_list(const nlohmann::json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

// The diagnostics of an envelope whatever else it says. An envelope that carries no database is a
// failure, and its reason is exactly what these diagnostics hold.
std::vector<EnvelopeDiagnostic> parse_envelope_diagnostics(std::string_view output) {
    const nlohmann::json envelope = nlohmann::json::parse(output, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) return {};
    std::vector<EnvelopeDiagnostic> diagnostics;
    for (const auto& diagnostic : envelope.value("diagnostics", nlohmann::json::array())) {
        if (!diagnostic.is_object()) continue;
        diagnostics.push_back(EnvelopeDiagnostic { diagnostic.value("code", std::string {}), diagnostic.value("severity", std::string {}),
                                                   diagnostic.value("message", std::string {}), diagnostic.value("path", std::string {}) });
    }
    return diagnostics;
}

} // namespace

base::Result<ProducerProtocol> parse_producer_protocol(std::string_view output) {
    const nlohmann::json document = nlohmann::json::parse(output, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return base::fail("producer-protocol", "the protocol description is not a JSON object");
    ProducerProtocol protocol;
    if (const auto kinds = document.find("kinds"); kinds != document.end() && kinds->is_object()) {
        for (const auto& item : kinds->items()) {
            if (item.value().is_number_integer()) protocol.kinds.emplace(item.key(), item.value().get<int>());
        }
    }
    if (const auto commands = document.find("commands"); commands != document.end() && commands->is_object()) {
        for (const auto& item : commands->items()) {
            if (item.value().is_object()) protocol.commandEffects.emplace(item.key(), string_list(item.value().value("effects", nlohmann::json::array())));
        }
    }
    return protocol;
}

bool effects_acceptable(std::span<const std::string> effects) {
    return std::ranges::none_of(effects, [](const std::string& effect) { return effect == "write-project"; });
}

base::Result<DatabaseDocument> parse_database_envelope(std::string_view output) {
    const nlohmann::json envelope = nlohmann::json::parse(output, nullptr, false);
    if (envelope.is_discarded() || !envelope.is_object()) return base::fail("discovery-protocol", "the producer's output is not one JSON object");
    if (envelope.value("schemaVersion", 0) != 1) return base::fail("discovery-protocol", "unsupported envelope schemaVersion");
    if (envelope.value("kindVersion", 0) != 1) return base::fail("discovery-protocol", "unsupported kindVersion");
    DatabaseDocument document;
    document.effects = string_list(envelope.value("effects", nlohmann::json::array()));
    for (const auto& diagnostic : envelope.value("diagnostics", nlohmann::json::array())) {
        if (!diagnostic.is_object()) continue;
        document.diagnostics.push_back(EnvelopeDiagnostic { diagnostic.value("code", std::string {}), diagnostic.value("severity", std::string {}),
                                                            diagnostic.value("message", std::string {}), diagnostic.value("path", std::string {}) });
    }
    const std::string kind { envelope.value("kind", std::string {}) };
    const auto data = envelope.find("data");
    if (!kind.ends_with(BUILD_DATABASE_KIND_SUFFIX) || data == envelope.end() || !data->is_object() || !data->contains("database")) {
        std::string reason { "the producer answered without a database" };
        for (const auto& diagnostic : document.diagnostics) {
            if (diagnostic.severity == "error") reason = std::format("{}: {}", diagnostic.code, diagnostic.message);
        }
        return base::fail("discovery-failed", reason);
    }
    document.database = (*data)["database"];
    if (!document.database.is_object()) return base::fail("discovery-protocol", "data.database is not an object");
    document.watch = string_list(data->value("watch", nlohmann::json::array()));
    document.inputsFingerprint = data->value("inputs-fingerprint", std::string {});
    return document;
}

std::optional<std::string> download_required(std::span<const EnvelopeDiagnostic> diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == OFFLINE_DOWNLOAD_REQUIRED) return diagnostic.message;
        // mcpp before 2026.9.16.1 had no code of its own for this; its message is the only sign.
        if (diagnostic.message.contains("offline mode:") || diagnostic.message.contains("run without --offline")) {
            return diagnostic.message;
        }
    }
    return std::nullopt;
}

base::Result<DatabaseDocument> run_database_command(std::span<const std::string> command, std::string_view workDirectory,
                                                    const RunContext& how) {
    if (command.empty()) return base::fail("discovery-command", "empty discovery command");
    // The user's build tool in the user's environment (design 4.3), offline unless the user asked
    // otherwise (4.4), in a unit the bound can end whole (4.2).
    platform::toolrun::Request request {
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = std::string { workDirectory },
        .purpose = how.purpose,
        .root = how.root,
        .network = how.offline ? platform::toolrun::Network::offline : platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = how.hard },
        .soft = how.soft,
        .environmentWait = how.environmentWait,
        .onSoftDeadline = how.onSoftDeadline,
    };
    platform::toolrun::Record record;
    auto result = platform::toolrun::run(request, &record);
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut) {
        return base::fail("discovery-timeout", std::format("{} did not finish in {} ms", command.front(), how.hard.count()));
    }
    auto document = parse_database_envelope(result->output);
    if (!document) {
        // An envelope that failed still says why. A producer that printed nothing has only its
        // exit status and standard error to speak with.
        if (auto envelope = parse_envelope_diagnostics(result->output); !envelope.empty()) {
            if (auto missing = download_required(envelope)) {
                return base::fail(std::string { NEEDS_DOWNLOAD }, *missing);
            }
        }
        if (base::trim(result->output).empty()) {
            const std::string held { result->outputHeldOpen ? " (something it started held its output open)" : "" };
            return base::fail("discovery-failed", std::format("{} exited with {}{}: {}", command.front(), result->exitCode,
                                                              held, base::trim(result->error)));
        }
        return document;
    }
    document->runId = record.id;
    document->networkObserved = std::ranges::find(document->effects, std::string_view { "network" }) != document->effects.end();
    if (document->networkObserved) platform::toolrun::observed_network(record.id);
    return document;
}

nlohmann::json make_discovery_request(const DiscoveryRequest& request) {
    nlohmann::json value = nlohmann::json::object();
    value["workspace"] = request.workspace;
    value["files"] = request.files;
    if (!request.configuration.empty()) value["configuration"] = request.configuration;
    // S2 3.2: a consumer that will not have this run reach the network says so. A producer may
    // ignore it, so it is a courtesy, not the mechanism (design 4.4).
    value["network"] = request.network;
    value["profile-version"] = std::string { PROFILE_VERSION };
    return value;
}

base::Result<DiscoveryResult> parse_discovery_output(std::string_view output) {
    DiscoveryResult result;
    for (auto line : base::split_lines(output)) {
        line = base::trim(line);
        if (line.empty()) continue;
        nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object()) {
            return base::fail("discovery-protocol", std::format("not a JSON object line: {}", line));
        }
        const std::string kind { message.value("kind", std::string {}) };
        if (kind == "progress") {
            result.progress.push_back(message.value("message", std::string {}));
        } else if (kind == "finished") {
            result.database = message.value("database", std::string {});
            if (result.database.empty()) return base::fail("discovery-protocol", "finished without a database path");
            // S2 3.3: the database path is absolute; a relative one would be read against a guessed directory.
            if (!base::is_absolute_path(result.database)) return base::fail("discovery-protocol", std::format("finished with a relative database path: {}", result.database));
            for (const auto& path : message.value("watch", nlohmann::json::array())) {
                if (path.is_string()) result.watch.push_back(path.get<std::string>());
            }
            return result;
        } else if (kind == "error") {
            return base::fail("discovery-failed", message.value("message", std::string { "the producer reported an error" }));
        }
        // Unknown kinds are ignored, as consumers must ignore unknown fields.
    }
    return base::fail("discovery-protocol", "the producer ended without finished or error");
}

base::Result<DiscoveryResult> run_discovery(std::span<const std::string> command, const DiscoveryRequest& request,
                                            std::string_view workDirectory, const RunContext& how) {
    if (command.empty()) return base::fail("discovery-command", "empty discovery command");
    // One runner for every external program (design 4.2): the user's environment, offline unless
    // the user allowed the network, a unit the bound ends whole, and a read that ends even when
    // something the producer started still holds the pipe.
    platform::toolrun::Request run {
        .program = command.front(),
        .arguments = { command.begin() + 1, command.end() },
        .workDirectory = std::string { workDirectory },
        .purpose = how.purpose,
        .root = how.root,
        .network = how.offline ? platform::toolrun::Network::offline : platform::toolrun::Network::allowed,
        .bounds = platform::RunBounds { .hard = how.hard },
        .soft = how.soft,
        .input = make_discovery_request(request).dump() + "\n",
        .environmentWait = how.environmentWait,
        .onSoftDeadline = how.onSoftDeadline,
    };
    auto result = platform::toolrun::run(run);
    if (!result) return std::unexpected { result.error() };
    if (result->timedOut) {
        return base::fail("discovery-timeout", std::format("{} did not finish in {} ms", command.front(), how.hard.count()));
    }
    auto parsed = parse_discovery_output(result->output);
    // A producer that reported its error says why better than its exit status does.
    if (!parsed && result->exitCode != 0 && parsed.error().code != "discovery-failed") {
        return base::fail("discovery-failed", std::format("{} exited with {}: {}", command.front(), result->exitCode, base::trim(result->error)));
    }
    return parsed;
}

} // namespace mcppls::spec
