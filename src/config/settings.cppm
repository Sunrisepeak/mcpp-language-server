// Every configurable behaviour of mcppls, in one place (0.0.6 plan §9 T1). Before this module, the
// same fact -- the default of `mcppls.buildTool`, say -- lived separately in the command line's
// `--build-tool` help text, `handle_initialize_`'s reading of `initializationOptions`, the VS Code
// extension's `package.json`, and the hand-written table in `docs/30-settings.md`; keeping them in
// step was a matter of remembering to. Here it lives once, as a row of `registry()`, and every one
// of those sites is derived from it: `mcppls.cli.commands` builds the global options from it,
// `session_options` and `handle_initialize_` fill a `Settings` from it, `mcppls settings` and
// `mcppls report` render it, and `tests/test_settings.cpp` holds the rendered docs and
// `editors/vscode/package.json` to it.
export module mcppls.config.settings;

import std;
import nlohmann.json;
import mcpplibs.cmdline;

export namespace mcppls::config::settings {

using Json = nlohmann::json;

// What kind of value a row takes. Purely descriptive for `string` and `path` (either accepts any
// text; `path` says so in the generated docs) -- the difference is real for the rest: `boolean` and
// `enumeration` are validated against a closed vocabulary, `seconds` against a non-negative
// integer, and `list` against zero or more comma-separated (or, on the command line, repeated)
// members.
enum class Kind { boolean, enumeration, string, path, seconds, list };

// Where a row is read: `server` is mcppls's own behaviour; `client` is read only by an editor
// plugin (kept here so the docs and `package.json` stay one table); `environment` is a variable of
// the process environment rather than a setting at all.
enum class Surface { server, client, environment };

// How a change to a row's value takes effect once mcppls is already running.
enum class Applies { restart, reload, immediately };

// Where a row's current value came from, in ascending precedence (T1's rule: command line beats
// initializationOptions beats the default; a later didChangeConfiguration updates a `client` or
// `clientUpdated` value, never one the command line set).
enum class Origin { defaulted, environment, commandLine, client, clientUpdated };

std::string_view to_string(Kind kind);
std::string_view to_string(Surface surface);
std::string_view to_string(Applies applies);
std::string_view to_string(Origin origin);

// One row: everything about one configurable behaviour of mcppls. `key` is dotted, under the
// `mcppls.` namespace for a `server` or `client` row (`buildTool`, `semanticTokens.modules`); for
// an `environment` row it is the bare variable name (`MCPPLS_CACHE_DIR`). `values` is the closed
// vocabulary for `enumeration` and (when it has one) `list`; a `list` row with none accepts any
// non-empty member. `defaultValue` is the row's own string form of its default -- for `boolean`,
// `"true"` or `"false"`; for `list`, its members joined with `,`. `commandLine` is the flag's
// spelling (`"--build-tool"`), empty when there is none. `commandLineNegated` is for a boolean
// whose flag's presence means false against a true default (`--no-discover`).
// `commandLineRepeatable` is for a `list` row taken as one value per occurrence of the flag
// (`--disable-workaround`, repeated) rather than one occurrence with a comma-separated value
// (`--build-discovery-providers`). `clientConfigurable` says whether an editor's own settings UI
// is expected to expose this row at all: true for every `client` row and for a `server` row VS
// Code's `package.json` should carry a matching property for; false for one that is command-line
// only (a path to something on this machine, a workaround id, a timeout) or fixed by what a
// client's own capabilities declare (`semanticTokens.moduleType`) -- `tests/test_settings.cpp`
// reads this to know which rows to expect in `package.json` and which to expect absent.
struct Setting {
    std::string key;
    Kind kind { Kind::string };
    std::vector<std::string> values;
    std::string defaultValue;
    std::string commandLine;
    bool commandLineNegated { false };
    bool commandLineRepeatable { false };
    Surface surface { Surface::server };
    Applies applies { Applies::restart };
    std::string category;
    std::string since;
    std::string summary;
    std::string summaryZh;
    std::vector<std::string> aliases;
    bool clientConfigurable { false };
};

// The shipped registry (0.0.6 plan §9 T1), in the order the generated docs list it: grouped by
// category, each category's rows in a fixed, meaningful order.
std::span<const Setting> registry();

// `key` (bare, dotted, no `mcppls.` prefix) matched against `rows`' own key or any of its
// `aliases`; null when none of `rows` answers to it.
const Setting* find(std::span<const Setting> rows, std::string_view key);

// A row's effective value, in its own string form (see `Setting::defaultValue` above for what that
// form is per `Kind`), and where it came from.
struct Value {
    std::string text;
    Origin origin { Origin::defaulted };
};

// Something wrong with one layer's attempt to set a row: an unknown key (`key` empty) or a value
// outside its kind's vocabulary (`key` names the row; the row's value was left at what it already
// was, never silently changed by a value nobody here recognizes).
struct Problem {
    std::string key;
    std::string message;
};

// What a `workspace/didChangeConfiguration` (or an equivalent later layer) actually changed, split
// by what taking it needs: a caller reloads each workspace's model for `reloadKeys`, and tells the
// person `restartKeys` needs a restart to take effect (`immediately` rows need neither: this
// module's caller reads the new value straight from `Settings` the next time it looks).
struct ChangeResult {
    std::vector<std::string> changedKeys;
    std::vector<std::string> restartKeys;
    std::vector<std::string> reloadKeys;
};

// The registry resolved against however many layers have been applied: every row's effective value,
// its origin, and the problems every layer applied so far ran into. Constructed at the row
// defaults (and an `environment` row's value read from the process environment, once); each
// `apply_*` layers a source of values over what is there, per T1's precedence.
class Settings {
public:
    explicit Settings(std::span<const Setting> rows = registry());

    std::span<const Setting> rows() const;
    const Value& value(std::string_view key) const;
    std::string string_value(std::string_view key) const;
    bool bool_value(std::string_view key) const;
    std::chrono::seconds seconds_value(std::string_view key) const;
    // A `list` row's members, split from its stored comma-joined text; empty when the row's value is empty.
    std::vector<std::string> list_value(std::string_view key) const;
    Origin origin(std::string_view key) const;

    // The command-line layer (T1a): every row with a `commandLine` spelling that `args` gives,
    // validated and recorded at `Origin::commandLine`. Wins over every layer after it.
    void apply_command_line(const mcpplibs::cmdline::ParsedArgs& args);
    // `initializationOptions` (or the whole `initialize` params -- only that key is read): nested
    // objects, dotted keys, or either wrapped in a top-level `mcppls` object, all accepted (T1).
    // A row the command line already set is left alone.
    void apply_initialization_options(const Json& initializationOptionsOrParams);
    // `workspace/didChangeConfiguration`'s params: `params.settings.mcppls` (VS Code's own shape),
    // or the same nested/dotted/wrapped forms `apply_initialization_options` accepts, directly on
    // `params.settings` or `params` itself. Updates every row the command line did not set, and
    // says what actually changed.
    ChangeResult apply_configuration_change(const Json& params);

    const std::vector<Problem>& problems() const;

    // `{"<key>": {"value": ..., "origin": "..."}, ..., "problems": [...]}` (`mcppls report`'s
    // `settings` object, T1). `value` is typed by the row's `Kind`: a JSON boolean, number or
    // array of strings where that fits, a string otherwise.
    Json to_json() const;

private:
    std::span<const Setting> rows_;
    std::map<std::string, Value, std::less<>> values_;
    std::vector<Problem> problems_;
};

// `mcppls settings --format markdown [--lang en|zh-CN]`: the reference table grouped by category
// (`docs/30-settings.md` and its zh-CN mirror embed this verbatim between two markers, so
// `tests/test_settings.cpp` can hold the file to the renderer byte for byte).
std::string to_markdown(std::span<const Setting> rows, std::string_view lang = "en");

// `mcppls settings --format json`: every field of every row, for a machine reader.
Json registry_to_json(std::span<const Setting> rows);

} // namespace mcppls::config::settings
