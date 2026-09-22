// mcppls-devtools: the one entry for work on this repository (docs/93-devtools.md).
//
//   mcpp run -p devtools -- <command> [...]        provisions what the command needs, then runs it
//
// What goes where (tooling architecture §5.1): mcpp builds, tests and runs; the server's own
// command line answers what a user debugging their machine needs (`mcppls cache`, `check`,
// `report`); this program does what only someone working on this repository needs. It does not
// wrap mcpp's verbs and does not repeat the server's diagnostics.
//
// Each command lives in its own module and contributes one `cmdline::App`; this file only puts
// them together, so adding a command is one module and one line here.
import std;
import mcpplibs.cmdline;
import mcppls.base.log;
import mcppls.devtools.packaging;
import mcppls.devtools.kit;
import mcppls.devtools.version;
import mcppls.devtools.measure;
import mcppls.devtools.release;
import mcppls.devtools.check;
import mcppls.devtools.bench;
import mcppls.devtools.stress;

namespace devtools = mcppls::devtools;

int main(int argc, char* argv[]) {
    mcppls::base::log::set_level(mcppls::base::log::Level::info);
    mcpplibs::cmdline::App app { "mcppls-devtools" };
    (void) app.description("Development, test, packaging and release tools for mcpp-language-server");

    int status { 0 };
    bool handled { false };
    (void) app.subcommand(devtools::payload_command(handled, status));
    (void) app.subcommand(devtools::extension_command(handled, status));
    (void) app.subcommand(devtools::uninstall_command(handled, status));
    (void) app.subcommand(devtools::kit_command(handled, status));
    (void) app.subcommand(devtools::version_command(handled, status));
    (void) app.subcommand(devtools::measure_command(handled, status));
    (void) app.subcommand(devtools::release_command(handled, status));
    (void) app.subcommand(devtools::check_command(handled, status));
    (void) app.subcommand(devtools::bench_command(handled, status));
    (void) app.subcommand(devtools::stress_command(handled, status));

    const int parsed { app.run(argc, argv) };
    if (parsed != 0) return parsed;
    const std::vector<std::string> given { argv + 1, argv + argc };
    const bool askedForHelp { std::ranges::any_of(given, [](const std::string& argument) {
        return argument == "--help" || argument == "-h" || argument == "--version";
    }) };
    if (!handled && !askedForHelp) {
        std::println(std::cerr, "mcppls-devtools: say what to do (--help lists the commands)");
        return 2;
    }
    return status;
}
