// Command-line and environment configuration (PROTOCOL.md "Configuration").
export module mcppls.model.config;

import std;

export namespace mcppls::model {

struct Config {
    std::string provider;          // "openai" | "anthropic" | "" (not configured)
    std::string endpoint;          // resolved base URL; provider's default when --endpoint is absent
    std::string model;             // default model id; may be empty
    std::string apiKeyEnvVar;      // name of the variable the key is read from; may be empty
    std::string apiKey;            // resolved value; empty when not configured or the variable is unset
    int timeoutSeconds { 60 };
};

// Parsing --help/--version, or a bad flag, is handled entirely here (cmdline
// prints help/version itself; a real error is printed to stderr): `config` is
// nullopt in both cases and `exitCode` is what main() should return without
// starting the gateway.
struct ConfigResult {
    std::optional<Config> config;
    int exitCode { 0 };
};

ConfigResult resolve_config(int argc, char* argv[]);

} // namespace mcppls::model
