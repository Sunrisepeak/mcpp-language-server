module mcppls.cli.settings;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.config.settings;

namespace mcppls::cli {

namespace {

namespace cmdline = mcpplibs::cmdline;
namespace settings = mcppls::config::settings;

} // namespace

cmdline::App settings_command(bool& handled, int& status) {
    cmdline::App command { "settings" };
    (void)command.description("Print the configuration registry: every setting mcppls understands, its default, and how to spell it");
    (void)command.option("format").takes_value().help("markdown (default) | json");
    (void)command.option("lang").takes_value().help("en (default) | zh-CN; markdown only");
    (void)command.action([&handled, &status](const cmdline::ParsedArgs& args) {
        handled = true;
        const std::string format { args.value("format").value_or("markdown") };
        const auto rows = settings::registry();
        if (format == "json") {
            std::println("{}", settings::registry_to_json(rows).dump(2));
        } else if (format == "markdown") {
            std::println("{}", settings::to_markdown(rows, args.value("lang").value_or("en")));
        } else {
            std::println(std::cerr, "settings: unknown format {}; use markdown or json", format);
            status = 2;
            return;
        }
        status = 0;
    });
    return command;
}

} // namespace mcppls::cli
