// cl.exe and clang-cl dialect arguments translated into GNU-style arguments for
// the engine's clang++ driver (design section 14.4, rows P6 and P7). The engine
// does not use clang's cl driver mode: there a module interface cannot be named
// by anything but its extension, and MSVC projects name interfaces `.ixx`.
export module mcppls.normalize.msvc;

import std;
import mcppls.toolchain.probe;

export namespace mcppls::normalize {

struct MsvcInput {
    std::span<const std::string> arguments;   // full command, argv[0] first, response files expanded
    std::string source;
    std::string workDirectory;
    const toolchain::ToolchainFacts* facts { nullptr };
    bool importable { false };
    bool noAlignedAllocationWithMsvcStl { true };   // the core engine's trait (overall design 5.4)
};

std::vector<std::string> translate_msvc(const MsvcInput& input);

} // namespace mcppls::normalize
