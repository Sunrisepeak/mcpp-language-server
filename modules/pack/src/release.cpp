module mcppls.pack.release;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcpplibs.os;

namespace os = mcpplibs::os;
namespace mcppls::pack::release {
namespace {

// Files the release step itself produces after this checks or renders -- never "absent", because
// a first pass runs before they exist and a final pass runs after.
const std::set<std::string> SELF { "MANIFEST.md", "SHA256SUMS" };

std::string apply(std::string_view pattern, std::string_view platform, std::string_view version, std::string_view kit = {}) {
    std::string out { pattern };
    out = base::replace_all(out, "{platform}", platform);
    out = base::replace_all(out, "{version}", version);
    out = base::replace_all(out, "{kit}", kit);
    return out;
}

// `name` against a declared name with at most one `*`, which stands for one or more characters.
bool matches(std::string_view declared, std::string_view name) {
    const auto star = declared.find('*');
    if (star == std::string_view::npos) return declared == name;
    const std::string_view head { declared.substr(0, star) };
    const std::string_view tail { declared.substr(star + 1) };
    return name.size() > head.size() + tail.size() && name.starts_with(head) && name.ends_with(tail);
}

} // namespace

base::Result<Manifest> load_manifest(std::string_view path) {
    auto text = os::fs::read(path);
    if (!text) return base::fail("read-file", text.error());
    Manifest manifest {};
    try {
        const auto json = nlohmann::json::parse(*text);
        manifest.releaseName = json.at("release-name").get<std::string>();
        for (const auto& platform : json.at("platforms")) manifest.platforms.push_back(platform.get<std::string>());
        for (const auto& raw : json.at("assets")) {
            Asset asset {};
            asset.id = raw.value("id", std::string {});
            asset.pattern = raw.at("pattern").get<std::string>();
            asset.perPlatform = raw.value("per-platform", false);
            asset.required = raw.value("required", false);
            asset.what = raw.value("what", std::string {});
            asset.install = raw.value("install", std::string {});
            manifest.assets.push_back(std::move(asset));
        }
        if (json.contains("not-in-a-release")) {
            for (const auto& reason : json.at("not-in-a-release")) manifest.notInARelease.push_back(reason.get<std::string>());
        }
    } catch (const std::exception& error) {
        return base::fail("release-manifest", std::format("{} is not a valid release manifest: {}", path, error.what()));
    }
    return manifest;
}

std::vector<Expected> expected_files(const Manifest& manifest, std::string_view version) {
    std::vector<Expected> out;
    for (const auto& asset : manifest.assets) {
        if (asset.perPlatform) {
            for (const auto& platform : manifest.platforms) {
                const auto kit = manifest.kits.find(platform);
                out.push_back({ apply(asset.pattern, platform, version, kit == manifest.kits.end() ? platform : kit->second), asset });
            }
        } else {
            out.push_back({ apply(asset.pattern, "", version), asset });
        }
    }
    return out;
}

std::string render(const Manifest& manifest, std::string_view version, const std::vector<Expected>& files,
                   const std::set<std::string>& onDisk) {
    std::string out;
    out += std::format("# {}\n\n", apply(manifest.releaseName, "", version));
    out += "| File | What it is | How to install it |\n";
    out += "|---|---|---|\n";
    for (const auto& file : files) {
        if (file.name == "MANIFEST.md") continue;
        // A name with a `*` is shown as the file it matched.
        std::string shown { file.name };
        for (const auto& name : onDisk) {
            if (file.name.contains('*') && matches(file.name, name)) shown = name;
        }
        const bool present { onDisk.contains(shown) || SELF.contains(file.name) };
        out += std::format("| `{}`{} | {} | {} |\n", shown, present ? "" : " *(not in this release)*",
                            file.asset.what, file.asset.install);
    }
    out += "\n## Not in a release\n\n";
    for (const auto& reason : manifest.notInARelease) out += std::format("- {}\n", reason);
    out += std::format("\nGenerated from `packaging/release.manifest.json` for {}.\n", version);
    return out;
}

base::Result<CheckReport> check(const Manifest& manifest, std::string_view version, std::string_view directory,
                                bool renderAlso) {
    std::error_code ignored;
    if (!std::filesystem::is_directory(directory, ignored)) {
        return base::fail("release-dir", std::format("{} is not a directory", directory));
    }
    // `list_directory` returns each entry already joined onto `directory`; what belongs in
    // `onDisk` (compared against the manifest's bare file names) is the name alone.
    std::set<std::string> onDisk;
    for (const auto& path : base::children(directory)) {
        if (std::filesystem::is_regular_file(path, ignored)) onDisk.insert(std::string { base::file_name(path) });
    }

    const auto declared = expected_files(manifest, version);

    if (renderAlso) {
        const std::string text { render(manifest, version, declared, onDisk) };
        if (auto written = os::fs::write_atomic(base::join_path(directory, "MANIFEST.md"), text); !written) {
            return base::fail("write-file", written.error());
        }
        onDisk.insert("MANIFEST.md");
    }

    CheckReport report {};
    const auto declares = [&](std::string_view name) {
        return std::ranges::any_of(declared, [&](const Expected& file) { return matches(file.name, name); });
    };
    for (const auto& file : declared) {
        const bool present { std::ranges::any_of(onDisk, [&](const std::string& name) { return matches(file.name, name); }) };
        if (present) continue;
        if (file.asset.required && !SELF.contains(file.name)) {
            report.problems.push_back(std::format("missing (required): {} — {}", file.name, file.asset.what));
        } else if (!file.asset.required) {
            report.notes.push_back(std::format("absent (optional): {}", file.name));
        }
    }
    // `onDisk` is a std::set, so this iterates in the same sorted order Python's `sorted()` gave.
    for (const auto& name : onDisk) {
        if (!declares(name)) {
            report.problems.push_back(std::format("undeclared: {} — add it to packaging/release.manifest.json or do not ship it", name));
            continue;
        }
        const auto stamp = base::file_stamp(base::join_path(directory, name));
        const std::uint64_t size { stamp ? stamp->size : 0 };
        if (size == 0) {
            report.problems.push_back(std::format("empty: {}", name));
        } else {
            report.notes.push_back(std::format("ok: {} ({:.1f} MB)", name, static_cast<double>(size) / 1'000'000.0));
        }
    }
    report.ok = report.problems.empty();
    return report;
}

} // namespace mcppls::pack::release
