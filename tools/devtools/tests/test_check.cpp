// The repository invariants (tooling architecture §7): os-surface, layers, versions and scripts
// are exercised against the REAL checkout (found the same way tests/test_spec.cpp finds it), which
// doubles as an acceptance check that this tree currently satisfies its own rules; `binary` is
// exercised against small synthetic files, since it only ever reads bytes.
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.fs;
import mcppls.platform.dirs;
import mcppls.devtools.check;

namespace check = mcppls::devtools::check;
namespace fs = mcppls::platform::fs;
namespace base = mcppls::base;

namespace {

std::string repository_root() {
    std::string directory { fs::current_directory() };
    while (true) {
        if (fs::is_regular_file(base::join_path(directory, "tools/devtools/scripts.allow"))) return directory;
        const std::string parent { base::parent_path(directory) };
        if (parent == directory) return fs::current_directory();
        directory = parent;
    }
}

std::string scratch(std::string_view name) {
    const std::string directory { base::join_path(
        mcppls::platform::dirs::temp_directory(),
        std::format("mcppls-test-check-{}-{}", name, std::chrono::steady_clock::now().time_since_epoch().count())) };
    (void) fs::create_directories(directory);
    return directory;
}

void print_problems(const check::Report& report) {
    for (const auto& p : report.problems) std::println(std::cerr, "  unexpected: {}", p);
}

} // namespace

int main() {
    using namespace mcppls::testing;
    const std::string root { repository_root() };

    "os-surface: this tree's three os packages declare exactly the six constants"_test = [&] {
        auto report = check::os_surface(root);
        expect(report.has_value());
        if (!report) return;
        print_problems(*report);
        expect(report->ok);
    };

    "layers: this tree's members depend only on strictly lower layers"_test = [&] {
        auto report = check::layers(root);
        expect(report.has_value());
        if (!report) return;
        print_problems(*report);
        expect(report->ok);
    };

    "versions: this tree's version sites agree and every third-party dependency uses .workspace"_test = [&] {
        auto report = check::versions(root);
        expect(report.has_value());
        if (!report) return;
        print_problems(*report);
        expect(report->ok);
    };

    "scripts: this tree's tracked scripts are all in the allowlist"_test = [&] {
        auto report = check::scripts(root);
        expect(report.has_value());
        if (!report) return;
        print_problems(*report);
        expect(report->ok);
    };

    "binary: a file with neither forbidden symbol passes"_test = [&] {
        const std::string path { base::join_path(scratch("clean"), "server") };
        (void) fs::write_file(path, "just some bytes, nothing archive- or mbedtls-shaped here");
        auto report = check::binary(path);
        expect(report.has_value());
        if (report) expect(report->ok);
    };

    "binary: a file carrying archive_read_ or mbedtls_ fails, naming which"_test = [&] {
        const std::string path { base::join_path(scratch("dirty"), "server") };
        (void) fs::write_file(path, std::string { "prefix" } + '\0' + "archive_read_open_filename" + '\0' + "mbedtls_ssl_init");
        auto report = check::binary(path);
        expect(report.has_value());
        if (!report) return;
        expect(!report->ok);
        expect(report->problems.size() == 2);
    };

    "binary: a missing file is an error, not a false pass"_test = [&] {
        auto report = check::binary(base::join_path(scratch("missing"), "no-such-file"));
        expect(!report.has_value());
    };

    "docs: the commands --help lists are read, and a description's continuation lines are not"_test = [&] {
        const std::string help { "Tools\n\nSUBCOMMANDS:\n    payload      Assemble a payload.\n"
                                 "  payload --verify DIR   an example line\n    kit          Build the kit\n\nOPTIONS:\n" };
        const auto commands = check::commands_in_help(help);
        expect(commands == std::vector<std::string> { "payload", "kit" });
    };

    "docs: a command missing from the document and one the program lacks both fail"_test = [&] {
        const std::string root { scratch("docs") };
        expect(fs::create_directories(base::join_path(root, "docs")).has_value());
        expect(fs::write_file(base::join_path(root, "docs/93-devtools.md"),
                              "Run `mcpp run -p devtools -- payload`, then `mcppls-devtools gone`.\n").has_value());
        auto report = check::docs(root, { "payload", "kit" });
        expect(fatal(report.has_value()));
        expect(!report->ok);
        expect(report->problems.size() == 2u);
        auto good = check::docs(root, { "payload", "gone" });
        expect(fatal(good.has_value()));
        expect(good->ok);
    };

    return report();
}
