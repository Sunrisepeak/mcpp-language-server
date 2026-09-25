// What leaves this machine in a report or a diagnostic bundle (issue #23 fix plan F18): the user's
// home directory, the user's and the machine's names and anything that looks like a secret are
// replaced by placeholders, the same original always by the same placeholder so that paths stay
// comparable, and a check afterwards finds whatever is left. The mapping from placeholder back to
// original lives only in the Redactor and is never written anywhere.
//
// Pure text in, text out: no file, no environment. Who the user is comes in as an Identity, which
// mcppls.bundle.bundle reads from the machine.
export module mcppls.bundle.redact;

import std;
import nlohmann.json;

export namespace mcppls::bundle {

// Who a bundle must not name.
struct Identity {
    std::vector<std::string> homes;        // the home directory, '/'-separated, in each spelling known (as set, canonical)
    std::vector<std::string> users;        // the login name and the home directory's last component
    std::vector<std::string> hosts;        // the machine's names
    std::vector<std::string> workspaces;   // roots replaced by <workspace>, <workspace-2>, ...; empty: roots are kept
};

// The rules, by the id the manifest counts them under.
inline constexpr std::string_view RULE_HOME { "home-directory" };
inline constexpr std::string_view RULE_WORKSPACE { "workspace-root" };
inline constexpr std::string_view RULE_USER { "user-name" };
inline constexpr std::string_view RULE_SHORT_NAME { "short-name" };
inline constexpr std::string_view RULE_HOST { "host-name" };
inline constexpr std::string_view RULE_SECRET { "secret" };
inline constexpr std::string_view RULE_EMAIL { "email" };

// Whether a user or host name is distinctive enough to be replaced wherever it stands as a word.
// A short name ("a", "bob") or a common word or generic account ("admin", "runner", "ubuntu") is
// replaced only where it names a directory: replacing it everywhere would rewrite ordinary words,
// and a check for it everywhere would fail every bundle of that user for ever.
bool distinctive_name(std::string_view name);

// Whether a key or variable name (camelCase, snake_case, kebab-case, UPPER_CASE) names a secret:
// token, secret, password, api key, authorization, ...
bool secret_name(std::string_view name);

// A place where something the Identity names was found after redaction.
struct Residue {
    std::string rule;
    std::size_t offset { 0 };
};

class Redactor {
public:
    explicit Redactor(Identity identity);
    ~Redactor();
    Redactor(Redactor&&) noexcept;
    Redactor& operator=(Redactor&&) noexcept;

    // The text with every rule applied, counting what each replaced.
    std::string redact(std::string_view text);
    // A JSON value redacted through its text, parsed back; its structure is unchanged, since no
    // placeholder carries a quote or a backslash.
    nlohmann::json redact_json(const nlohmann::json& value);

    // What the Identity names that is still in `text`: the home directory in any spelling, the user
    // and host names under the same rules as redact(), and anything that looks like a known token.
    // At most `limit` places.
    std::vector<Residue> residue(std::string_view text, std::size_t limit = 8) const;

    // Replacements made so far, by rule id.
    const std::map<std::string, std::size_t, std::less<>>& hits() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mcppls::bundle
