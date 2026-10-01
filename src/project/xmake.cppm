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

// One scalar the user's own `xmake f` left in `.xmake/<plat>/<arch>/xmake.conf` (X-2, plan 0.0.8 part 2):
// a string, or a true/false (`flag`), which xmake takes back as `--name=y|n`.
struct XmakeOption {
    std::string name;
    std::string value;
    bool flag { false };
    bool operator==(const XmakeOption&) const = default;
};

// The top-level `key = "string"` and `key = true/false` entries of an xmake.conf, in file order. The file is a Lua
// table; it is read as text and never executed, so nested tables (`__toolchains_linux_x86_64 = { ... }`), numbers
// and anything else are skipped.
std::vector<XmakeOption> parse_xmake_conf(std::string_view text);

// The user's configuration as this provider needs it: which file (the newest of `.xmake/*/*/xmake.conf`, the user's
// last `xmake f`), its size and time (they go into the configuration key) and what it said. Read only; nothing is written.
struct XmakeUserConfig {
    std::string path;
    std::uint64_t size { 0 };
    std::int64_t modified { 0 };
    std::vector<XmakeOption> options;
};
std::optional<XmakeUserConfig> read_xmake_user_config(std::string_view root);

// Keys xmake writes for itself or that belong to this machine's session, not to the project's choices
// (`builddir`, `host`, `theme`, `network`, `proxy_pac`, `ccache`, `policies`, `pkg_searchdirs`, `ndk_stdcxx`, `clean`, `__*`).
bool xmake_option_is_internal(std::string_view name);

// The options a retry keeps after xmake did not know one of them: plat, arch, mode, toolchain, sdk, runtimes, kind.
bool xmake_option_is_standard(std::string_view name);

// What a reduced configuration leaves out of `user`: the names that are not internal and not standard.
std::vector<std::string> xmake_left_out(std::span<const XmakeOption> user);

// D-1 (plan 0.0.9): the long option names `xmake f --help` lists (`--plat=PLAT`, `--ccache=[y|n]`, and the project's own
// `option()`s under "Command options (Project Configuration)"), sorted, without the leading dashes. An option line is indented
// by at most eight spaces; the deeper lines are descriptions and examples (`- xmake f --import=...`) and are not read.
std::vector<std::string> xmake_help_options(std::string_view text);

// The keys of `user` that are not internal, are not standard and are not in `accepted`: what xmake wrote into xmake.conf for
// itself (`proxy`, `dotnet`, `dotnet_sdkver`, ...) and `xmake f` refuses. Empty `accepted` (no help text) skips nothing.
std::vector<std::string> xmake_not_accepted(std::span<const XmakeOption> user, std::span<const std::string> accepted);

// The last `limit` lines of `output` that say `error:`, in order, trimmed (what a failed install says in xmake's own words).
std::vector<std::string> xmake_error_lines(std::string_view output, std::size_t limit = 3);

// The packages xmake says it failed to install: `error: install libtool failed!` names `libtool`.
std::vector<std::string> xmake_failed_installs(std::string_view output);

// The `.../installdir.failed/logs/install.txt` path xmake names when a package fails to install, or empty.
std::string xmake_install_log(std::string_view output);

// The reason of the needs-download outcome (D-3) and of an install that failed with the network allowed (D-2).
std::string xmake_needs_download_reason(std::span<const std::string> missing);
// `missing` may be empty (xmake named no package): the reason then speaks of the packages the project requires.
std::string xmake_install_failed_reason(std::span<const std::string> missing, std::string_view output);

// `xmake f -c [-p plat -a arch -m mode] [--name=value ...] --confirm=no --policies=package.fetch_only,network.mode:private
// --builddir=<private>/build` offline (measured: `network.mode:private` is what stops xmake's package repositories being
// pulled on a machine that has never fetched them, not `package.fetch_only` alone -- plan §3.5); online, `-y` replaces the
// two policy arguments (no fetch-only policy, so a package that is missing installs instead of only being looked for).
// `user` is the user's own configuration (X-2): `xmake f` resets every option it is not given, so the private run is given them
// all (E5: a debug project described as release otherwise); `reduced` keeps only the standard ones, for the retry.
// D-1: when `accepted` (xmake_help_options) is not empty, a key outside it and outside the standard ones is not passed on.
std::vector<std::string> xmake_configure_arguments(std::string_view buildDirectory, bool offline,
                                                   std::span<const XmakeOption> user = {}, bool reduced = false,
                                                   std::span<const std::string> accepted = {});

// xmake refused an option it does not know ("Invalid option: --x=1", exit 255 with v3.1.1).
bool xmake_unknown_option(std::string_view output);

// `xmake project -k compile_commands <private>/out`: xmake's own, dedicated compile-database
// command (it does not compile -- plan §3.5), which is why it is the only command this provider
// ever needs beyond the one configure above.
std::vector<std::string> xmake_compile_commands_arguments(std::string_view outDirectory);

// What the private configuration was last made for: the mode (xmake keeps `--policies` in its own
// configuration, so an offline configuration stays offline until `xmake f` runs again), the stamp
// of the project's xmake.lua and, X-2, of the user's xmake.conf (a new `xmake f` of theirs configures again here).
// `xmake f` runs again whenever this differs from the key it recorded after its last success; xmake itself
// re-reads a changed xmake.lua on the next command, but a mode change it cannot know about.
std::string xmake_configuration_key(bool offline, std::optional<std::pair<std::uint64_t, std::int64_t>> manifest,
                                    std::optional<std::pair<std::uint64_t, std::int64_t>> userConfig = std::nullopt);

// X-1: the project's own compile_commands.json is older than one of the inputs it was made from (an xmake.lua, the
// user's xmake.conf): what it says no longer follows them. Times are file-clock nanoseconds.
bool xmake_commands_out_of_date(std::int64_t commandsModified, std::span<const std::int64_t> inputsModified);

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
