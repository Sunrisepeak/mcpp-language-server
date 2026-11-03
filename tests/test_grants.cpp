// The directories an ok-image engine is given (mcpp design 2026-11-03 part 4 §9.3): what mcppls.grants states, what an
// engine database names, the set an engine is started with, and what a later database names that it does not cover.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.engine.clangd.grants;
import mcppls.normalize.plan;
import mcpplibs.os;

namespace base = mcppls::base;
namespace os = mcpplibs::os;
namespace cld = mcppls::engine::clangd;

namespace {

std::string scratch(std::string_view name) {
    std::error_code ignored;
    const std::string root { base::join_path(base::spelled(os::dirs::temp()),
        std::format("mcppls-test-grants-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void)std::filesystem::create_directories(root, ignored);
    return root;
}

std::string made(const std::string& root, std::string_view child) {
    std::error_code ignored;
    const std::string path { base::join_path(root, child) };
    (void)std::filesystem::create_directories(path, ignored);
    return path;
}

const cld::Grant* find(std::span<const cld::Grant> grants, std::string_view path) {
    for (const auto& grant : grants)
        if (base::same_path(grant.path, path)) return &grant;
    return nullptr;
}

} // namespace

int main() {
    using namespace mcppls::testing;

    "mcppls.grants: a path is read-only, rw:<path> writable, all the runner's own, and a relative name a problem"_test = [] {
        const std::string absolute { base::join_path(base::spelled(os::dirs::temp()), "x") };
        const std::vector<std::string> members { absolute, "rw:" + absolute + "/y", "relative/z", " ", "all" };
        const auto setting = cld::parse_grant_setting(members);
        expect(setting.all);
        expect(fatal(setting.added.size() == 2u));
        expect(!setting.added[0].writable && setting.added[1].writable);
        expect(setting.problems.size() == 1u && setting.problems.front().find("relative/z") != std::string::npos);
        expect(!cld::parse_grant_setting(std::vector<std::string> {}).all) << "nothing set: the derived set";
    };

    "the directories an engine database names: each flag's, joined or apart, the command's and the compiler's installation"_test = [] {
        const std::string root { scratch("plan") };
        mcppls::normalize::EnginePlan plan;
        mcppls::normalize::EngineEntry entry;
        entry.directory = root;
        entry.file = base::join_path(root, "src/a.cppm");
        entry.arguments = { base::join_path(root, "toolchain/bin/clang++"), "-std=c++23", "-Iinclude", "-isystem",
                            base::join_path(root, "sys"), "-isystem" + base::join_path(root, "sys2"), "--sysroot=" + base::join_path(root, "sysroot"),
                            "-include", base::join_path(root, "pre/config.h"), "-include-pch", "x.pch",
                            "-fmodule-file=m=" + base::join_path(root, "bmi/m.pcm"), "-c", entry.file };
        plan.entries.push_back(entry);
        const auto named = cld::plan_directories(plan);
        const auto has = [&](std::string_view child) {
            const std::string want { base::join_path(root, child) };
            return std::ranges::any_of(named, [&](const std::string& d) { return base::same_path(d, want); });
        };
        expect(has("src") && has("toolchain")) << "the source's directory and the compiler's installation";
        expect(has("include")) << "-I joined, relative to the command's directory";
        expect(has("sys") && has("sys2")) << "-isystem apart and joined";
        expect(has("sysroot") && has("pre") && has("bmi")) << "--sysroot=, -include's directory, -fmodule-file=<name>='s";
        expect(std::ranges::none_of(named, [](const std::string& d) { return d.ends_with("-pch") || d.ends_with("x.pch"); }))
            << "-include-pch is not -include";
    };

    "the set: the project first and writable, one beneath another with as many rights left out, an absent one left out"_test = [] {
        const std::string root { scratch("set") };
        const std::string project { made(root, "project") };
        const std::string cache { made(root, "cache") };
        const std::string payload { made(root, "payload/clangd") };
        const std::string store { made(root, "store") };
        const std::string storeWritable { made(root, "store/lib/1.0") };
        cld::GrantInputs inputs;
        inputs.root = project;
        inputs.writable = { cache, base::join_path(cache, "inner") };
        inputs.payload = { payload };
        inputs.readable = { store, base::join_path(root, "absent") };
        inputs.named = { made(root, "project/include"), made(root, "store/lib") };
        inputs.added = { cld::Grant { storeWritable, true, false } };
        const auto grants = cld::engine_grants(inputs);
        expect(fatal(!grants.empty()));
        expect(base::same_path(grants.front().path, project) && grants.front().writable) << "the directory the engine runs in";
        const auto* c = find(grants, cache);
        expect(fatal(c != nullptr));
        expect(c->writable);
        const auto* p = find(grants, payload);
        expect(fatal(p != nullptr));
        expect(!p->writable && p->unchanging) << "the engine's own files, which nothing changes";
        expect(find(grants, store) != nullptr && !find(grants, store)->writable);
        expect(find(grants, base::join_path(root, "project/include")) == nullptr) << "beneath the project, writable";
        expect(find(grants, base::join_path(root, "store/lib")) == nullptr) << "beneath the store, read-only";
        expect(find(grants, base::join_path(root, "absent")) == nullptr) << "a directory that is not there is not given";
        const auto* w = find(grants, storeWritable);
        expect(fatal(w != nullptr));
        expect(w->writable) << "a writable directory beneath a read-only one is given on its own";
    };

    "no more than sixteen: read-only grants become the directory they are beneath, the deepest first"_test = [] {
        const std::string root { scratch("many") };
        cld::GrantInputs inputs;
        inputs.root = made(root, "project");
        inputs.writable = { made(root, "cache") };
        for (int i { 0 }; i < 20; ++i) inputs.named.push_back(made(root, std::format("deps/lib{}/include", i)));
        inputs.named.push_back(made(root, "other/include"));
        const auto grants = cld::engine_grants(inputs);
        expect(grants.size() <= 16u) << grants.size();
        expect(fatal(!grants.empty()));
        expect(grants.front().writable && base::same_path(grants.front().path, inputs.root)) << "the project stays first";
        expect(cld::covered(grants, base::join_path(root, "deps/lib19/include")) && cld::covered(grants, base::join_path(root, "other/include")))
            << "every directory named is still covered";
        expect(find(grants, base::join_path(root, "cache")) != nullptr && find(grants, base::join_path(root, "cache"))->writable)
            << "a writable grant is never merged away";
    };

    "too many writable grants are retained for refusal rather than replaced with unrestricted defaults"_test = [] {
        const std::string root { scratch("writable-limit") };
        cld::GrantInputs inputs;
        inputs.root = made(root, "project");
        for (int i { 0 }; i < 17; ++i)
            inputs.writable.push_back(made(root, std::format("write-{}", i)));
        const auto grants = cld::engine_grants(inputs);
        expect(grants.size() == 18u) << "an empty set would give the engine the whole file system";
        expect(cld::covered(grants, inputs.root));
        for (const auto& path : inputs.writable) {
            const auto* grant = find(grants, path);
            expect(grant != nullptr && grant->writable) << path;
        }
    };

    "a database naming a directory the engine was not given says which; one beneath a grant, or absent, does not"_test = [] {
        const std::string root { scratch("uncovered") };
        const std::vector<cld::Grant> grants { cld::Grant { made(root, "project"), true, false } };
        const std::vector<std::string> named { made(root, "project/src"), made(root, "elsewhere/include"), base::join_path(root, "gone") };
        const auto missing = cld::uncovered(grants, named);
        expect(fatal(missing.size() == 1u));
        expect(base::same_path(missing.front(), base::join_path(root, "elsewhere/include")));
        expect(cld::covered(grants, base::join_path(root, "project/src/deep")));
        expect(!cld::covered(grants, base::join_path(root, "projectile"))) << "a sibling whose name begins the same is not beneath";
    };

    "the system's roots and the runner's caches are directories that are there"_test = [] {
        for (const auto& root : cld::system_roots()) expect(std::filesystem::is_directory(root)) << root;
        for (const auto& cache : cld::runner_caches()) expect(std::filesystem::is_directory(cache)) << cache;
        if (os::host() != os::platform::windows) expect(!cld::system_roots().empty()) << "/usr at least";
    };

    return 0;
}
