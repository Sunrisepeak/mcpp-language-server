// Building an S1 model where the build does not provide one: from a
// compile_commands.json plus scanning and probing, or from sources alone.
export module mcppls.project.infer;

import std;
import mcppls.spec.database;
import mcppls.toolchain.probe;
import mcppls.project.compdb;
import mcppls.project.scan;

export namespace mcppls::project {

using Scanner = std::function<ScanResult(std::string_view path)>;
using FactsMap = std::map<std::string, toolchain::ToolchainFacts, std::less<>>;
// Probes a driver with its relevant arguments; nullopt when it cannot be probed.
using Prober = std::function<std::optional<toolchain::ToolchainFacts>(std::string_view driver, std::span<const std::string> relevant)>;

struct InferredDatabase {
    spec::Database database;
    FactsMap facts;
    std::vector<std::string> problems;
    std::vector<std::string> watch;   // extra paths to watch, from a discovery command
    // Facts a person may want to know that do not reduce any feature: (code, message).
    std::vector<std::pair<std::string, std::string>> notices;
};

// Completes a database a producer wrote: probes each set's compiler when the
// set names no toolchain the facts know, and scans units without a role.
InferredDatabase enrich_database(spec::Database database, const Scanner& scanner, const Prober& prober);

// One set per distinct toolchain; sets see each other. Units carry scanned roles.
InferredDatabase database_from_commands(std::span<const CompileCommand> commands, std::string_view familyName,
                                        const Scanner& scanner, const Prober& prober);

struct InferOptions {
    std::optional<toolchain::ToolchainFacts> facts;   // a discovered compiler, or nullopt for kit semantics
    std::string languageStandard { "c++26" };         // inferred_language_standard(facts), for sources nothing describes
    std::string engineDriver { "clang++" };           // written as argv[0] when there is no compiler
};

// The standard sources nothing describes are read with (C++26 alignment): the newest one the compiler that reads them
// takes -- C++26 for the semantic kit (clang 23, libc++ 23), GCC 14 and later and Clang 17 and later (as `c++2c` before
// Clang 20) -- and C++23 for older ones, the oldest standard `import std` has.
std::string inferred_language_standard(const std::optional<toolchain::ToolchainFacts>& facts);

// Every C++ source under `root` (build output and dot directories skipped).
InferredDatabase infer_database(std::string_view root, const InferOptions& options, const Scanner& scanner);

// Scans a file from disk.
Scanner file_scanner();

} // namespace mcppls::project
