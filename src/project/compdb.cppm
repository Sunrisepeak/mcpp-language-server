// JSON Compilation Databases (compile_commands.json) and command-line splitting.
export module mcppls.project.compdb;

import std;
import nlohmann.json;
import mcppls.base.error;

export namespace mcppls::project {

struct CompileCommand {
    std::string directory;
    std::string file;                   // absolute
    std::string output;
    std::vector<std::string> arguments;
};

enum class CommandSyntax { posix, windows };

// POSIX shell words, or the Windows CommandLineToArgvW rules.
std::vector<std::string> split_command(std::string_view command, CommandSyntax syntax);
// Replaces @file arguments with the file's words, recursively (bounded depth).
std::vector<std::string> expand_response_files(std::span<const std::string> arguments, std::string_view directory,
                                               CommandSyntax syntax);

base::Result<std::vector<CompileCommand>> parse_compile_commands(const nlohmann::json& document, CommandSyntax syntax);
base::Result<std::vector<CompileCommand>> read_compile_commands(std::string_view path);

} // namespace mcppls::project
