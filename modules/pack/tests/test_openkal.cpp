// mcppls.pack.openkal: the openkal kit, from what mcpp's build database says of each openkal target -- which
// directories are shared and which a target reads alone, what a unit reads with, and the libc++ the headers are.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.openkal;
import mcpplibs.os;

namespace os = mcpplibs::os;
namespace base = mcppls::base;
namespace openkal = mcppls::pack::openkal;

namespace {

std::string scratch_dir() {
    const auto root = std::filesystem::current_path() / ".test-scratch" / std::format("mcppls-openkal-{}", std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

void put(const std::string& path, std::string_view content) {
    std::filesystem::create_directories(std::filesystem::path { path }.parent_path());
    std::ofstream out { path, std::ios::binary };
    out << content;
}

// A package of the store as mcpp lays it out: <store>/mcpplibs-x-<name>/<version>/<name>-<version>.
std::string package(const std::string& store, std::string_view name, std::string_view version) {
    return base::join_path(store, std::format("mcpplibs-x-{}/{}/{}-{}", name, version, name, version));
}

// The std module's command and a unit's, as `mcpp emit build-database --spec compile-commands` writes them.
nlohmann::json database(const std::string& store, const std::string& project, std::string_view arch) {
    const std::string runtime { package(store, "openkal-llvm-runtime", "0.16.6") };
    const std::string musl { package(store, "openkal-musl", "0.22.6") };
    std::vector<std::string> std_arguments { "clang++", "-std=c++23", std::format("--target={}-unknown-none-elf", arch), "-nostdinc",
                                             "-I", runtime + "/llvm/libcxx/modules" };
    for (const std::string& directory : { runtime + "/port/include", runtime + "/llvm/libcxx/include", runtime + "/llvm/libcxx/src", runtime + "/llvm/libc",
                                          musl + "/musl/include", std::format("{}/musl/arch/{}", musl, arch), musl + "/musl/arch/generic" }) {
        std_arguments.insert(std_arguments.end(), { "-isystem", directory });
    }
    std_arguments.insert(std_arguments.end(), { "--precompile", runtime + "/llvm-generated/std.cppm" });
    const std::vector<std::string> unit_arguments { "clang++", "-I" + project + "/include", "-std=c++23", "-O0", "-g",
                                                    std::format("--target={}-unknown-none-elf", arch), "--no-default-config", "-nostdlibinc",
                                                    "-nostdinc++", "-femulated-tls", "-D_XOPEN_SOURCE=700", "-D", "__OPENKAL__", "-fno-short-wchar",
                                                    "-c", project + "/src/main.cpp", "-o", "main.o" };
    return nlohmann::json::array({
        { { "directory", project }, { "file", runtime + "/llvm-generated/std.cppm" }, { "arguments", std_arguments } },
        { { "directory", project }, { "file", "src/main.cpp" }, { "arguments", unit_arguments } },
    });
}

} // namespace

int main() {
    using namespace mcppls::testing;
    const std::string work { scratch_dir() };
    const std::string store { base::join_path(work, "xpkgs") };
    const std::string project { base::join_path(work, "project") };

    "the command of a target: the std module's -isystem directories and a unit of the project"_test = [&] {
        auto command = openkal::command_of("x86_64-openkal", database(store, project, "x86_64"), project);
        expect(fatal(command.has_value())) << (command ? std::string {} : command.error().message);
        expect(command->stdModule.ends_with("llvm-generated/std.cppm"));
        expect(command->includes.size() == 7u) << "the -isystem directories of the std module's command";
        expect(command->stdIncludes.size() == 1u && command->stdIncludes.front().ends_with("libcxx/modules"));
        expect(std::ranges::find(command->arguments, "-D_XOPEN_SOURCE=700") != command->arguments.end());
        expect(std::ranges::find(command->arguments, std::string { "clang++" }) == command->arguments.end()) << "not the compiler";
        expect(!openkal::command_of("x86_64-openkal", database(store, project, "x86_64"), base::join_path(work, "elsewhere")))
            << "no unit of the project";
        expect(!openkal::command_of("x86_64-openkal", nlohmann::json::array(), project));
    };

    "a directory both targets read is shared; one target's goes under include/<target>; a run-time's own sources stay out"_test = [&] {
        std::vector<openkal::TargetCommand> commands;
        for (std::string_view arch : { "x86_64", "aarch64" }) {
            auto command = openkal::command_of(std::format("{}-openkal", arch), database(store, project, arch), project);
            expect(fatal(command.has_value()));
            commands.push_back(std::move(*command));
        }
        auto layout = openkal::plan(commands);
        expect(fatal(layout.has_value())) << (layout ? std::string {} : layout.error().message);
        expect(layout->targets.size() == 2u);
        const auto& x86 = layout->targets.front();
        expect(x86.name == "x86_64-openkal" && x86.triple == "x86_64-unknown-none-elf");
        expect((x86.includes == std::vector<std::string> { "include/openkal-llvm-runtime/port/include", "include/openkal-llvm-runtime/llvm/libcxx/include",
                                                           "include/openkal-musl/musl/include", "include/x86_64-openkal/openkal-musl/musl/arch/x86_64",
                                                           "include/openkal-musl/musl/arch/generic" }))
            << "the build's order, the shared directories once and the architecture's own under its target";
        expect(layout->targets.back().includes[3] == "include/aarch64-openkal/openkal-musl/musl/arch/aarch64");
        expect(std::ranges::none_of(layout->copies, [](const openkal::Copy& copy) { return copy.source.ends_with("/src") || copy.source.ends_with("llvm/libc"); }));
        expect(layout->copies.size() == 6u) << "four shared and one of each architecture";
        expect((x86.arguments == std::vector<std::string> { "-nostdlibinc", "-nostdinc++", "-D_XOPEN_SOURCE=700", "-D__OPENKAL__", "-fno-short-wchar" }))
            << "what decides how a unit reads, and not the project's own -I, -O or -g";
        expect(!openkal::plan({})) << "no target";
    };

    "the libc++ version is the one the headers' __config states"_test = [&] {
        const std::string headers { base::join_path(work, "headers") };
        put(base::join_path(headers, "v1/__config"), "#ifndef _LIBCPP___CONFIG\n#define _LIBCPP___CONFIG\n#  define _LIBCPP_VERSION 220108\n#endif\n");
        put(base::join_path(headers, "port/__config"), "#include_next <__config>\n");
        const std::vector<std::string> includes { base::join_path(headers, "port"), base::join_path(headers, "v1") };
        expect(openkal::libcxx_version(includes) == std::optional<std::string> { "22.1.8" });
        expect(!openkal::libcxx_version(std::vector<std::string> { base::join_path(headers, "port") }).has_value());
    };

    std::error_code ignored;
    std::filesystem::remove_all(work, ignored);
    return report();
}
