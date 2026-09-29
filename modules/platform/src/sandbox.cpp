module mcppls.platform.sandbox;

import std;
import mcppls.os;
import mcppls.platform.env;
import mcppls.platform.fs;

namespace mcppls::platform {

namespace {

std::string trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string { text };
}

} // namespace

std::int64_t tracer_pid(std::string_view selfStatus) {
    constexpr std::string_view key { "TracerPid:" };
    std::size_t at { 0 };
    while (at < selfStatus.size()) {
        const std::size_t end { std::min(selfStatus.find('\n', at), selfStatus.size()) };
        const std::string_view line { selfStatus.substr(at, end - at) };
        if (line.starts_with(key)) {
            const std::string value { trimmed(line.substr(key.size())) };
            std::int64_t pid { 0 };
            const auto [rest, error] = std::from_chars(value.data(), value.data() + value.size(), pid);
            return error == std::errc {} && rest == value.data() + value.size() && pid > 0 ? pid : 0;
        }
        at = end + 1;
    }
    return 0;
}

std::string classify_sandbox(const SandboxFacts& facts) {
    const std::string tracer { trimmed(facts.tracerName) };
    if (tracer_pid(facts.selfStatus) != 0) {
        if (tracer.starts_with("proot")) return "proot";
        if (!tracer.empty()) return {};   // some other tracer (a debugger), named
    } else if (facts.selfStatus.contains("TracerPid:")) {
        return {};   // not traced at all: a PROOT_* variable that leaked into this environment means nothing
    }
    // The status or the tracer's name could not be read (a hidden /proc): the variables are what is left.
    for (const std::string& variable : facts.environment) {
        if (variable.starts_with("PROOT_")) return "proot";
    }
    return {};
}

const std::string& sandbox() {
    static const std::string answer { [] {
        SandboxFacts facts;
        facts.environment = env::variables();
        if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) {
            if (auto status = fs::read_file("/proc/self/status")) {
                facts.selfStatus = std::move(*status);
                if (const std::int64_t tracer { tracer_pid(facts.selfStatus) }; tracer != 0) {
                    if (auto name = fs::read_file(std::format("/proc/{}/comm", tracer))) facts.tracerName = std::move(*name);
                }
            }
        }
        return classify_sandbox(facts);
    }() };
    return answer;
}

} // namespace mcppls::platform
