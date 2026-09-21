module mcppls.project.compdb;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.platform.fs;

namespace mcppls::project {

namespace {

std::vector<std::string> split_posix(std::string_view command) {
    std::vector<std::string> words;
    std::string current;
    bool inWord { false };
    enum class Quote { none, single, dual } quote { Quote::none };
    for (std::size_t i { 0 }; i < command.size(); ++i) {
        const char c { command[i] };
        if (quote == Quote::single) {
            if (c == '\'') quote = Quote::none; else current += c;
            continue;
        }
        if (quote == Quote::dual) {
            if (c == '"') {
                quote = Quote::none;
            } else if (c == '\\' && i + 1 < command.size()
                       && (command[i + 1] == '"' || command[i + 1] == '\\' || command[i + 1] == '$' || command[i + 1] == '`')) {
                current += command[++i];
            } else {
                current += c;
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (inWord) {
                words.push_back(std::move(current));
                current.clear();
                inWord = false;
            }
            continue;
        }
        inWord = true;
        if (c == '\'') quote = Quote::single;
        else if (c == '"') quote = Quote::dual;
        else if (c == '\\' && i + 1 < command.size()) current += command[++i];
        else current += c;
    }
    if (inWord) words.push_back(std::move(current));
    return words;
}

std::vector<std::string> split_windows(std::string_view command) {
    std::vector<std::string> words;
    std::string current;
    bool inWord { false };
    bool quoted { false };
    for (std::size_t i { 0 }; i < command.size(); ++i) {
        const char c { command[i] };
        if (c == '\\') {
            std::size_t backslashes { 0 };
            while (i < command.size() && command[i] == '\\') {
                ++backslashes;
                ++i;
            }
            inWord = true;
            if (i < command.size() && command[i] == '"') {
                current.append(backslashes / 2, '\\');
                if (backslashes % 2 == 1) {
                    current += '"';
                } else {
                    quoted = !quoted;
                }
            } else {
                current.append(backslashes, '\\');
                --i;
            }
            continue;
        }
        if (c == '"') {
            inWord = true;
            if (quoted && i + 1 < command.size() && command[i + 1] == '"') {
                current += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            if (inWord) {
                words.push_back(std::move(current));
                current.clear();
                inWord = false;
            }
            continue;
        }
        inWord = true;
        current += c;
    }
    if (inWord) words.push_back(std::move(current));
    return words;
}

void expand_into(std::vector<std::string>& out, std::span<const std::string> arguments, std::string_view directory,
                 CommandSyntax syntax, int depth) {
    for (const auto& argument : arguments) {
        if (argument.size() > 1 && argument.front() == '@' && depth < 8) {
            const std::string path { base::join_path(directory, std::string_view { argument }.substr(1)) };
            if (auto content = platform::fs::read_file(path)) {
                const auto words = split_command(*content, syntax);
                expand_into(out, words, directory, syntax, depth + 1);
                continue;
            }
        }
        out.push_back(argument);
    }
}

} // namespace

std::vector<std::string> split_command(std::string_view command, CommandSyntax syntax) {
    return syntax == CommandSyntax::windows ? split_windows(command) : split_posix(command);
}

std::vector<std::string> expand_response_files(std::span<const std::string> arguments, std::string_view directory,
                                               CommandSyntax syntax) {
    std::vector<std::string> out;
    expand_into(out, arguments, directory, syntax, 0);
    return out;
}

base::Result<std::vector<CompileCommand>> parse_compile_commands(const nlohmann::json& document, CommandSyntax syntax) {
    if (!document.is_array()) return base::fail("compdb-invalid", "compile_commands.json is not an array");
    std::vector<CompileCommand> commands;
    for (const auto& entry : document) {
        if (!entry.is_object()) continue;
        CompileCommand command;
        command.directory = base::normalize_path(entry.value("directory", std::string {}));
        const std::string file { entry.value("file", std::string {}) };
        if (file.empty() || command.directory.empty()) continue;
        command.file = base::join_path(command.directory, file);
        command.output = entry.value("output", std::string {});
        if (const auto arguments = entry.find("arguments"); arguments != entry.end() && arguments->is_array()) {
            for (const auto& argument : *arguments) {
                if (argument.is_string()) command.arguments.push_back(argument.get<std::string>());
            }
        } else if (const auto text = entry.find("command"); text != entry.end() && text->is_string()) {
            command.arguments = split_command(text->get<std::string>(), syntax);
        }
        if (command.arguments.empty()) continue;
        commands.push_back(std::move(command));
    }
    return commands;
}

base::Result<std::vector<CompileCommand>> read_compile_commands(std::string_view path) {
    auto text = platform::fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
    nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
    if (document.is_discarded()) return base::fail("compdb-invalid", std::format("{} is not valid JSON", path));
    const CommandSyntax syntax { mcppls::os::FAMILY == mcppls::os::Family::windows ? CommandSyntax::windows : CommandSyntax::posix };
    return parse_compile_commands(document, syntax);
}

} // namespace mcppls::project
