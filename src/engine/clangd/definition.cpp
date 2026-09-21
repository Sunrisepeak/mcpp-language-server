module mcppls.engine.clangd.definition;

import std;
import mcppls.base.path;

namespace mcppls::engine::clangd {

namespace {

std::string_view stem_of(std::string_view path) {
    std::string_view name { base::file_name(path) };
    if (const std::size_t dot { name.find('.') }; dot != std::string_view::npos) name = name.substr(0, dot);
    return name;
}

std::size_t shared_directories(std::string_view left, std::string_view right) {
    std::size_t shared { 0 };
    std::size_t i { 0 };
    for (; i < left.size() && i < right.size() && left[i] == right[i]; ++i) {
        if (left[i] == '/') ++shared;
    }
    return shared;
}

} // namespace

DeclarationKind declaration_kind(std::string_view text, std::size_t nameEnd) {
    int parentheses { 0 };
    int brackets { 0 };
    const std::size_t end { std::min(text.size(), nameEnd + 16384) };
    for (std::size_t i { nameEnd }; i < end; ++i) {
        const char c { text[i] };
        const char next { i + 1 < end ? text[i + 1] : '\0' };
        if (c == '/' && next == '/') {
            i = text.find('\n', i);
            if (i == std::string_view::npos) return DeclarationKind::unknown;
            continue;
        }
        if (c == '/' && next == '*') {
            i = text.find("*/", i + 2);
            if (i == std::string_view::npos) return DeclarationKind::unknown;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            for (++i; i < end && text[i] != c; ++i) {
                if (text[i] == '\\') ++i;
                if (i < end && text[i] == '\n') break;
            }
            continue;
        }
        if (c == '(') ++parentheses;
        else if (c == '[') ++brackets;
        else if (c == ')' || c == ']') {
            int& depth { c == ')' ? parentheses : brackets };
            if (depth == 0) return DeclarationKind::unknown;
            --depth;
        } else if (parentheses > 0 || brackets > 0) {
            continue;
        } else if (c == ';') {
            return DeclarationKind::declaration;
        } else if (c == '{' || c == '=') {
            return DeclarationKind::definition;   // a body, a class body, an initializer, = default, = delete
        } else if (c == ':') {
            if (next == ':') {
                ++i;
                continue;
            }
            return DeclarationKind::definition;   // a constructor's initializers, a base clause
        } else if (c == '}') {
            return DeclarationKind::unknown;
        }
    }
    return DeclarationKind::unknown;
}

std::vector<std::string> units_to_search(std::string_view interfacePath, std::span<const UnitOfModule> units, std::size_t limit) {
    std::vector<const UnitOfModule*> ordered;
    for (const auto& unit : units) ordered.push_back(&unit);
    const std::string_view stem { stem_of(interfacePath) };
    const std::string directory { base::parent_path(interfacePath) };
    auto rank = [&](const UnitOfModule* unit) {
        return std::tuple { unit->partition ? 1 : 0, stem_of(unit->path) == stem ? 0 : 1,
                            -static_cast<long>(shared_directories(base::parent_path(unit->path), directory)), std::string_view { unit->path } };
    };
    std::ranges::sort(ordered, [&](const UnitOfModule* a, const UnitOfModule* b) { return rank(a) < rank(b); });
    std::vector<std::string> chosen;
    for (const auto* unit : ordered) {
        if (chosen.size() >= limit) break;
        chosen.push_back(unit->path);
    }
    return chosen;
}

} // namespace mcppls::engine::clangd
