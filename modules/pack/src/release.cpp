module mcppls.pack.release;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.platform.fs;

namespace mcppls::pack::release {
namespace fs = mcppls::platform::fs;
namespace {

// Files the release step itself produces after this checks or renders -- never "absent", because
// a first pass runs before they exist and a final pass runs after.
const std::set<std::string> SELF { "MANIFEST.md", "SHA256SUMS" };

std::string apply(std::string_view pattern, std::string_view platform, std::string_view version) {
    std::string out { pattern };
    out = base::replace_all(out, "{platform}", platform);
    out = base::replace_all(out, "{version}", version);
    return out;
}

} // namespace

base::Result<Manifest> load_manifest(std::string_view path) {
    auto text = fs::read_file(path);
    if (!text) return std::unexpected { text.error() };
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
            for (const auto& platform : manifest.platforms) out.push_back({ apply(asset.pattern, platform, version), asset });
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
        const bool present { onDisk.contains(file.name) || SELF.contains(file.name) };
        out += std::format("| `{}`{} | {} | {} |\n", file.name, present ? "" : " *(not in this release)*",
                            file.asset.what, file.asset.install);
    }
    out += "\n## Not in a release\n\n";
    for (const auto& reason : manifest.notInARelease) out += std::format("- {}\n", reason);
    out += std::format("\nGenerated from `packaging/release.manifest.json` for {}.\n", version);
    return out;
}

base::Result<CheckReport> check(const Manifest& manifest, std::string_view version, std::string_view directory,
                                bool renderAlso) {
    if (!fs::is_directory(directory)) {
        return base::fail("release-dir", std::format("{} is not a directory", directory));
    }
    // `list_directory` returns each entry already joined onto `directory`; what belongs in
    // `onDisk` (compared against the manifest's bare file names) is the name alone.
    std::set<std::string> onDisk;
    for (const auto& path : fs::list_directory(directory)) {
        if (fs::is_regular_file(path)) onDisk.insert(std::string { base::file_name(path) });
    }

    const auto declared = expected_files(manifest, version);
    std::set<std::string> declaredNames;
    for (const auto& file : declared) declaredNames.insert(file.name);

    if (renderAlso) {
        const std::string text { render(manifest, version, declared, onDisk) };
        if (auto written = fs::write_file_atomic(base::join_path(directory, "MANIFEST.md"), text); !written) {
            return std::unexpected { written.error() };
        }
        onDisk.insert("MANIFEST.md");
    }

    CheckReport report {};
    for (const auto& file : declared) {
        if (onDisk.contains(file.name)) continue;
        if (file.asset.required && !SELF.contains(file.name)) {
            report.problems.push_back(std::format("missing (required): {} — {}", file.name, file.asset.what));
        } else if (!file.asset.required) {
            report.notes.push_back(std::format("absent (optional): {}", file.name));
        }
    }
    // `onDisk` is a std::set, so this iterates in the same sorted order Python's `sorted()` gave.
    for (const auto& name : onDisk) {
        if (!declaredNames.contains(name)) {
            report.problems.push_back(std::format("undeclared: {} — add it to packaging/release.manifest.json or do not ship it", name));
        }
    }
    for (const auto& name : onDisk) {
        if (!declaredNames.contains(name)) continue;
        const auto stamp = fs::stamp(base::join_path(directory, name));
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
