module mcppls.platform.preopen;

import std;
import openkal.fs;
import mcppls.base.path;
import mcppls.base.text;

namespace mcppls::platform {

namespace {

struct Preopen {
    kal_dir directory {};
    std::string name;
};

const std::vector<Preopen>& preopens() {
    static const std::vector<Preopen> table { [] {
        std::vector<Preopen> result;
        const kal_uintptr count { kal_fs_preopen_count() };
        for (kal_uintptr i { 0 }; i < count; ++i) {
            kal_dir dir {};
            std::array<char, 1024> name {};
            kal_uintptr length { 0 };
            if (kal_fs_preopen(i, &dir, name.data(), name.size(), &length) != kal_ok) continue;
            if (length > name.size()) continue;
            result.push_back(Preopen { dir, std::string { name.data(), static_cast<std::size_t>(length) } });
        }
        return result;
    }() };
    return table;
}

bool is_separator(char c) { return c == '/' || c == '\\'; }

} // namespace

std::optional<ResolvedName> resolve_name(std::string_view absolutePath) {
    const std::string path { base::normalize_path(absolutePath) };
    std::optional<ResolvedName> best;
    for (const auto& preopen : preopens()) {
        std::string prefix { base::normalize_path(preopen.name) };
        if (prefix.empty()) continue;
        const bool prefixIsRoot { is_separator(prefix.back()) };
        bool matches { false };
        if (base::NATIVE_PATH_STYLE == base::PathStyle::windows) {
            matches = path.size() >= prefix.size() && base::iequals_ascii(std::string_view { path }.substr(0, prefix.size()), prefix);
        } else {
            matches = path.starts_with(prefix);
        }
        if (!matches) continue;
        if (!prefixIsRoot && path.size() > prefix.size() && !is_separator(path[prefix.size()])) continue;
        if (best && best->preopenName.size() >= prefix.size()) continue;
        std::string_view rest { std::string_view { path }.substr(prefix.size()) };
        while (!rest.empty() && is_separator(rest.front())) rest.remove_prefix(1);
        best = ResolvedName { preopen.directory, prefix, std::string { rest } };
    }
    return best;
}

} // namespace mcppls::platform
