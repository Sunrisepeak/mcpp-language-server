// The conformance case (tests/cts/README.md): a fixture of fixtures/ run by mcppls-conformance as
// a client of the server (mcppls.harness). An object of a topic's file is one case and states
// where it runs (`on`), what it holds (`machine`, for a fixture whose checks bound how long an
// answer takes) and, by its name, how the runner drives it: NAME@polling declares no dynamic
// registration of watched files, NAME@plain no `experimental.cxxModules`, NAME@vscode, @neovim and
// @zed what that editor sends, NAME@warm runs a second session on the cache the first one left,
// and NAME@linked reaches its workspace through a symbolic link -- where every macOS temporary
// directory is (/var is /private/var): mcpp names files by their resolved path, and an unsaved
// edit must still reach the modules that import it, and a root the client names that way is
// still the server's own. Every failed check is a failure of the case, named by its id.
export module mcppls.cts;

import std;
export import mcpplibs.mtest;
export import mcppls.harness;

export namespace mcppls::cts {

using harness::client;
using harness::on;
using harness::run;

namespace detail {

template <class... A>
constexpr bool ends_with_body() {
    if constexpr (sizeof...(A) == 0)
        return false;
    else
        return std::invocable<std::remove_cvref_t<std::tuple_element_t<sizeof...(A) - 1, std::tuple<A...>>>&>;
}

// The sessions the name `fixture@variant` asks for.
inline void sessions(std::string_view id) {
    const auto at = id.find('@');
    const auto fixture = std::string(id.substr(0, at));
    const auto variant = at == std::string_view::npos ? std::string_view{} : id.substr(at + 1);
    if (variant == "linked") {
        run({.fixture = fixture, .linked = true});
        return;
    }
    if (variant == "warm") {
        const auto home = mcpplibs::mtest::temp_dir();
        run({.fixture = fixture, .home = home});
        run({.fixture = fixture, .warm = true, .home = home});
        return;
    }
    run({.fixture = fixture, .as = harness::client_of(variant)});
}

}  // namespace detail

// A fixture as a case. Its body, unless it states one, runs the sessions its name names; a failing
// case runs once more with the server's log (diagnose).
class Fixture : public mcpplibs::mtest::CTS {
public:
    template <class... A>
    requires(!detail::ends_with_body<A...>())
    Fixture(mcpplibs::mtest::name_here name, A&&... args)
        : CTS(name, server_log(), std::forward<A>(args)...,
              [id = std::string(name.name)] { detail::sessions(id); }) {}
    template <class... A>
    requires(detail::ends_with_body<A...>())
    Fixture(mcpplibs::mtest::name_here name, A&&... args) : CTS(name, server_log(), std::forward<A>(args)...) {}

private:
    static mcpplibs::mtest::diagnose server_log() {
        return {.env = {{"MCPPLS_CTS_VERBOSE", "1"}, {"MCPPLS_CTS_TIMEOUT", "60"}}};
    }
};

}  // namespace mcppls::cts
