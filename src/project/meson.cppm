// meson as a data source (B-6, 2026-09-27 plan §3.3; untested against a real meson -- the free
// functions below are unit-tested, and the manual check this task asked for is recorded in the
// commit that adds this file).
export module mcppls.project.meson;

import std;
import mcppls.base.error;
import mcppls.project.infer;
import mcppls.project.provider;

export namespace mcppls::project {

// `meson setup <private> --wrap-mode=nodownload` for a first configure offline (online: without
// the flag, so a missing subproject is fetched); `--reconfigure` in place of the bare setup once
// `<private>/meson-private` already exists (meson refuses a second plain `setup` into a directory
// that already has one).
std::vector<std::string> meson_setup_arguments(std::string_view privateDirectory, bool offline, bool reconfigure);

// Whether meson's output said a wrap-based subproject download was needed and disabled
// ("Automatic wrap-based subproject downloading is disabled" / "nodownload"), and which
// subprojects it named, when it named any (meson's own wording varies by version and is not
// pinned down the way cmake's and xmake's were -- plan §3.3 leaves this provider at P3, unmeasured).
std::vector<std::string> meson_missing_subprojects(std::string_view output);

// B-1: meson's own `BuildSystemProvider` (registry order: last before compile-commands -- plan
// §0.3/§3.3, P3).
class MesonProvider final : public BuildSystemProvider {
public:
    std::string_view id() const override { return "meson"; }
    std::optional<Claim> detect(std::string_view root) const override;
    std::optional<Answer> existing(const Claim& claim, const ProviderContext& context) const override;
    Answer describe(const Claim& claim, const ProviderContext& context) const override;
    std::vector<std::string> watch_inputs(const Claim& claim) const override;
};

} // namespace mcppls::project
