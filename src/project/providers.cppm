// B-1 (2026-09-27 plan §3.2, §3.6, task 1): the registry every `BuildSystemProvider` is asked
// through, in the fixed order that gives conformance fixtures their precedence -- mcpp, CMake,
// xmake, meson, then a bare compile_commands.json, inferred being the fallback none of them are.
// This is the one module that may depend on every provider's own (mcpp.cppm, cmake.cppm, ...); none
// of them, nor mcppls.project.detect (which only needs the `BuildSystemProvider` interface itself,
// from mcppls.project.provider), depends back on this one.
export module mcppls.project.providers;

import std;
import mcppls.base.error;
import mcppls.project.compdb;
import mcppls.project.infer;
import mcppls.project.provider;
import mcppls.project.mcpp;
import mcppls.project.cmake;
import mcppls.project.xmake;
import mcppls.project.meson;

export namespace mcppls::project {

// The one provider with no build tool of its own: a `compile_commands.json` (or
// `build/compile_commands.json`) already on disk is both its claim and its whole answer --
// `describe()` never runs anything (plan §3.3 table: its `describe` column is "--").
class CompileCommandsProvider final : public BuildSystemProvider {
public:
    std::string_view id() const override { return "compile-commands"; }
    std::optional<Claim> detect(std::string_view root) const override;
    std::optional<Answer> existing(const Claim& claim, const ProviderContext& context) const override;
    Answer describe(const Claim& claim, const ProviderContext& context) const override;
    std::vector<std::string> watch_inputs(const Claim& claim) const override;
};

// mcpp, cmake, xmake, meson, compile-commands, in that order (B-1 task 1: "keep the current
// precedence... exactly"; xmake and meson are new between cmake and a bare compile_commands.json).
std::span<BuildSystemProvider* const> registered_providers();

} // namespace mcppls::project
