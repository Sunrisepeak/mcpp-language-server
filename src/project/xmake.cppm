// xmake as a data source (B-5, 2026-09-27 plan §3.5): its own compile-database command, which does
// not compile, run in a private directory so its implicit configuration and module dependency scan
// (both side effects of that command, measured against xmake 3.1.1) never touch the project.
export module mcppls.project.xmake;

import std;
import mcppls.base.error;
import mcppls.project.infer;
import mcppls.project.provider;

export namespace mcppls::project {

// `XMAKE_CONFIGDIR=<private>/config`, `XMAKE_THEME=plain` (plan §3.5): the configuration directory
// xmake would otherwise write into the project's own `.xmake/`, and a theme with no interactive
// prompt or color codes to strip out of a captured log. A free function so tests can check it
// without spawning xmake.
std::vector<std::pair<std::string, std::string>> xmake_environment(std::string_view configDirectory);

// `xmake f -c --confirm=no --policies=package.fetch_only,network.mode:private --builddir=<private>/build`
// offline (measured: `network.mode:private` is what stops xmake's package repositories being pulled
// on a machine that has never fetched them, not `package.fetch_only` alone -- plan §3.5); online,
// `xmake f -c -y --builddir=<private>/build` (no fetch-only policy, so a package that is missing
// installs instead of only being looked for).
std::vector<std::string> xmake_configure_arguments(std::string_view buildDirectory, bool offline);

// `xmake project -k compile_commands <private>/out`: xmake's own, dedicated compile-database
// command (it does not compile -- plan §3.5), which is why it is the only command this provider
// ever needs beyond the one configure above.
std::vector<std::string> xmake_compile_commands_arguments(std::string_view outDirectory);

// Every package name `xmake f`'s output named as not found ("The packages(xxhash, fmt) not found",
// or the multi-line "note: install or modify (m) these packages" xmake also prints), in first-seen
// order; empty when `output` does not match either shape.
std::vector<std::string> xmake_missing_packages(std::string_view output);

// B-1: xmake's own `BuildSystemProvider` (registry order: after CMake, before meson -- plan §3.3,
// "xmake 提到 P1" ahead of meson's P3).
class XmakeProvider final : public BuildSystemProvider {
public:
    std::string_view id() const override { return "xmake"; }
    std::optional<Claim> detect(std::string_view root) const override;
    std::optional<Answer> existing(const Claim& claim, const ProviderContext& context) const override;
    Answer describe(const Claim& claim, const ProviderContext& context) const override;
    std::vector<std::string> watch_inputs(const Claim& claim) const override;
};

} // namespace mcppls::project
