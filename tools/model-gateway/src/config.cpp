module mcppls.model.config;

import std;
import mcpplibs.cmdline;

namespace mcppls::model {

namespace {

namespace cmdline = mcpplibs::cmdline;

// Flag value, else the named environment variable, else empty.
std::string pick(const cmdline::ParsedArgs& args, std::string_view flag, const char* envName) {
    if (auto value = args.value(flag); value && !value->empty()) return *value;
    if (const char* fromEnv = std::getenv(envName); fromEnv && *fromEnv) return std::string { fromEnv };
    return {};
}

std::string default_endpoint(const std::string& provider) {
    if (provider == "openai") return "https://api.openai.com/v1";
    if (provider == "anthropic") return "https://api.anthropic.com/v1";
    return {};
}

} // namespace

ConfigResult resolve_config(int argc, char* argv[]) {
    cmdline::App app { "mcppls-model" };
    (void)app.version(std::string { "0.1.0" });
    (void)app.description("Reference model gateway for mcppls (see PROTOCOL.md)");
    (void)app.option("provider").takes_value().help("openai | anthropic (env MCPPLS_MODEL_PROVIDER)");
    (void)app.option("endpoint").takes_value().help("Base URL, e.g. http://127.0.0.1:11434/v1 (env MCPPLS_MODEL_ENDPOINT)");
    (void)app.option("model").takes_value().help("Default model id (env MCPPLS_MODEL_NAME)");
    (void)app.option("api-key-env").takes_value().help("Env var holding the API key (env MCPPLS_MODEL_API_KEY_ENV)");
    (void)app.option("timeout").takes_value().help("Request timeout in seconds, default 60 (env MCPPLS_MODEL_TIMEOUT)");

    auto parsed = app.parse(argc, argv);
    if (!parsed) {
        if (!parsed.error().is_error()) return ConfigResult { .config = std::nullopt, .exitCode = 0 }; // --help / --version
        std::println(std::cerr, "mcppls-model: {}", parsed.error().message);
        return ConfigResult { .config = std::nullopt, .exitCode = 2 };
    }

    Config config;

    config.provider = pick(*parsed, "provider", "MCPPLS_MODEL_PROVIDER");
    if (!config.provider.empty() && config.provider != "openai" && config.provider != "anthropic") {
        std::println(std::cerr, "mcppls-model: unknown --provider '{}' (want openai or anthropic); treating as not configured", config.provider);
        config.provider.clear();
    }

    config.endpoint = pick(*parsed, "endpoint", "MCPPLS_MODEL_ENDPOINT");
    if (config.endpoint.empty()) config.endpoint = default_endpoint(config.provider);

    config.model = pick(*parsed, "model", "MCPPLS_MODEL_NAME");

    config.apiKeyEnvVar = pick(*parsed, "api-key-env", "MCPPLS_MODEL_API_KEY_ENV");
    if (!config.apiKeyEnvVar.empty()) {
        // Absent is fine for local endpoints (PROTOCOL.md): apiKey simply stays empty.
        if (const char* key = std::getenv(config.apiKeyEnvVar.c_str()); key && *key) config.apiKey = key;
    }

    if (std::string timeoutText = pick(*parsed, "timeout", "MCPPLS_MODEL_TIMEOUT"); !timeoutText.empty()) {
        try {
            if (int seconds = std::stoi(timeoutText); seconds > 0) config.timeoutSeconds = seconds;
            else std::println(std::cerr, "mcppls-model: --timeout must be positive; using {}s", config.timeoutSeconds);
        } catch (...) {
            std::println(std::cerr, "mcppls-model: invalid --timeout '{}'; using {}s", timeoutText, config.timeoutSeconds);
        }
    }

    return ConfigResult { .config = std::move(config), .exitCode = 0 };
}

} // namespace mcppls::model
