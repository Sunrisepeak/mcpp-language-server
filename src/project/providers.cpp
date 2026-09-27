module mcppls.project.providers;

import std;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.project.compdb;
import mcppls.project.infer;
import mcppls.project.provider;
import mcppls.project.mcpp;
import mcppls.project.cmake;
import mcppls.project.xmake;
import mcppls.project.meson;

namespace mcppls::project {

std::optional<Claim> CompileCommandsProvider::detect(std::string_view root) const {
    for (std::string_view candidate : { "compile_commands.json", "build/compile_commands.json" }) {
        if (const std::string commands { base::join_path(std::string { root }, candidate) }; platform::fs::is_regular_file(commands)) {
            Claim claim;
            claim.provider = "compile-commands";
            claim.confidence = 50;   // the last resort of a "real" provider: whatever wrote this did not say
            claim.root = std::string { root };
            claim.compileCommands = commands;
            return claim;
        }
    }
    return std::nullopt;
}

std::optional<Answer> CompileCommandsProvider::existing(const Claim& claim, const ProviderContext& context) const {
    if (claim.compileCommands.empty()) return std::nullopt;
    return from_result(database_from_compile_commands_file(claim.compileCommands, claim.root, "mcppls", context.scanner, context.prober));
}

Answer CompileCommandsProvider::describe(const Claim&, const ProviderContext&) const {
    // There is nothing to run: a bare compile_commands.json is either there (existing() above) or
    // it is not, in which case inference is the fallback, not this provider trying harder.
    return Answer { .code = "compile-commands-no-database", .reason = "no compile_commands.json was found" };
}

std::vector<std::string> CompileCommandsProvider::watch_inputs(const Claim& claim) const {
    return { claim.compileCommands };
}

namespace {

std::vector<std::unique_ptr<BuildSystemProvider>>& provider_storage() {
    static std::vector<std::unique_ptr<BuildSystemProvider>> providers { [] {
        std::vector<std::unique_ptr<BuildSystemProvider>> made;
        made.push_back(std::make_unique<McppProvider>());
        made.push_back(std::make_unique<CmakeProvider>());
        made.push_back(std::make_unique<XmakeProvider>());
        made.push_back(std::make_unique<MesonProvider>());
        made.push_back(std::make_unique<CompileCommandsProvider>());
        return made;
    }() };
    return providers;
}

std::vector<BuildSystemProvider*>& provider_pointers() {
    static std::vector<BuildSystemProvider*> pointers { [] {
        std::vector<BuildSystemProvider*> made;
        for (const auto& provider : provider_storage()) made.push_back(provider.get());
        return made;
    }() };
    return pointers;
}

} // namespace

std::span<BuildSystemProvider* const> registered_providers() {
    return provider_pointers();
}

} // namespace mcppls::project
