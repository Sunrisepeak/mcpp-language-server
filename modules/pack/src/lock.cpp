module mcppls.pack.lock;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.text;
import mcppls.pack.fetch;
import mcppls.platform.fs;

namespace mcppls::pack::lock {
namespace fs = mcppls::platform::fs;
namespace {

std::string joined_names(const std::map<std::string, fetch::Entry>& entries) {
    std::vector<std::string> names;
    for (const auto& [name, unused] : entries) names.push_back(name);
    return base::join(names, ", ");
}

std::string joined_names(const std::map<std::string, Platform>& platforms) {
    std::vector<std::string> names;
    for (const auto& [name, unused] : platforms) names.push_back(name);
    return base::join(names, ", ");
}

} // namespace

base::Result<Lock> load(std::string_view path) {
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*text);
    } catch (const std::exception& error) {
        return base::fail("lock-parse", std::format("cannot parse {}: {}", path, error.what()));
    }
    if (!document.is_object()) return base::fail("lock-parse", std::format("{} is not a JSON object", path));

    Lock lockData {};
    lockData.clangdVersion = document.value("clangd-version", std::string {});
    lockData.libcxxVersion = document.value("libcxx-version", std::string {});

    const auto entries = document.find("entries");
    if (entries == document.end() || !entries->is_object()) {
        return base::fail("lock-parse", std::format("{} has no \"entries\" object", path));
    }
    for (const auto& item : entries->items()) {
        const auto& value = item.value();
        fetch::Entry parsed {};
        parsed.file = value.value("file", std::string {});
        parsed.url = value.value("url", std::string {});
        parsed.sha256 = value.value("sha256", std::string {});
        if (const auto size = value.find("size"); size != value.end() && size->is_number_unsigned()) {
            parsed.size = size->get<std::uint64_t>();
        }
        if (const auto from = value.find("license-from"); from != value.end() && from->is_object()) {
            lockData.licenseFrom.emplace(item.key(), LicenseFrom { from->value("entry", std::string {}), from->value("member", std::string {}) });
        }
        lockData.entries.emplace(item.key(), std::move(parsed));
    }

    const auto platforms = document.find("platforms");
    if (platforms == document.end() || !platforms->is_object()) {
        return base::fail("lock-parse", std::format("{} has no \"platforms\" object", path));
    }
    for (const auto& item : platforms->items()) {
        const auto& value = item.value();
        Platform parsed {};
        parsed.clangd = value.value("clangd", std::string {});
        parsed.serverTarget = value.value("server-target", std::string {});
        const auto kit = value.find("kit");
        if (kit != value.end() && kit->is_object()) {
            parsed.kit.recipe = kit->value("recipe", std::string {});
            parsed.kit.source = kit->value("source", std::string {});
            parsed.kit.target = kit->value("target", std::string {});
        }
        lockData.platforms.emplace(item.key(), std::move(parsed));
    }
    return lockData;
}

base::Result<fetch::Entry> entry(const Lock& lockData, std::string_view name) {
    const auto found = lockData.entries.find(std::string { name });
    if (found == lockData.entries.end()) {
        return base::fail("lock-entry", std::format("unknown lock entry {}; known: {}", name, joined_names(lockData.entries)));
    }
    return found->second;
}

base::Result<Platform> platform(const Lock& lockData, std::string_view platformName) {
    const auto found = lockData.platforms.find(std::string { platformName });
    if (found == lockData.platforms.end()) {
        return base::fail("lock-platform", std::format("unknown platform {}; known: {}", platformName, joined_names(lockData.platforms)));
    }
    return found->second;
}

std::vector<std::string> platform_names(const Lock& lockData) {
    std::vector<std::string> names;
    for (const auto& [name, unused] : lockData.platforms) names.push_back(name);
    return names;
}

} // namespace mcppls::pack::lock
