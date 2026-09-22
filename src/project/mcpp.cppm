// mcpp as a producer (design section 14.2): `mcpp emit build-database --format json`
// when this mcpp advertises the kind (mcpp-community/mcpp#636), otherwise
// `mcpp build --configure-only`'s compile database plus scanning and probing.
export module mcppls.project.mcpp;

import std;
import mcppls.base.error;
import mcppls.project.compdb;
import mcppls.project.detect;
import mcppls.project.infer;
import mcppls.project.provider;

export namespace mcppls::project {

std::string mcpp_package_name(std::string_view manifestText);

// Dotted, numeric-segment version comparison ("2026.9.21.3" > "2026.8.8.4", and "2026.9.9" <
// "2026.9.10": a plain string compare gets that one backwards). A segment that is not a number
// compares as 0, so an unparsed or empty version is never newer than one that parses.
bool mcpp_version_less(std::string_view left, std::string_view right);

// Other mcpp executables installed on the machine besides `resolved` (the project's own pin, or
// PATH), newest first: the xlings package store and mcpp's own registry store, following the same
// `xim-x-mcpp/<version>/bin/mcpp` layout `mcppls.toolchain.discover` already reads for compilers.
// Producer negotiation (design item 1) uses this to describe a project whose pinned mcpp cannot,
// without changing which mcpp the project itself builds with.
std::vector<std::string> other_mcpp_executables(std::string_view resolved, std::string_view homeDirectory);

// The standard library's module units of an mcpp compile database (usable plan W8), for an mcpp
// older than 2026.9.15.1, which describes them itself in mcpp:std (mcpp-community/mcpp#636). A
// project can keep such an mcpp through its .xlings.json. A package that supplies `std`, such as
// openkal-llvm-runtime, is built into mcpp's std build cache outside the compile database: the
// build directory's build.ninja stages that BMI, and std-module.json beside it records the
// sources and the commands. Empty when the commands name no staged std BMI or the record is unreadable.
std::vector<CompileCommand> mcpp_standard_units(std::span<const CompileCommand> commands);
base::Result<InferredDatabase> load_mcpp(const Detection& detection, const ProviderContext& context);

} // namespace mcppls::project
