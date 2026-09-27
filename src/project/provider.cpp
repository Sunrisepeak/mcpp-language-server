module mcppls.project.provider;

import std;
import mcppls.os;
import mcppls.base.error;
import mcppls.spec.discovery;
import mcppls.platform.env;
import mcppls.platform.fs;

namespace mcppls::project {

std::optional<std::string> find_tool(std::string_view name, std::span<const std::string> fallbacks) {
    if (auto found = platform::env::find_executable(name)) return found;
    for (const auto& fallback : fallbacks) {
        const std::string candidate { fallback + std::string { mcppls::os::EXECUTABLE_SUFFIX } };
        if (platform::fs::is_regular_file(candidate)) return candidate;
    }
    return std::nullopt;
}

base::Result<InferredDatabase> to_result(Answer answer) {
    if ((answer.outcome == Outcome::ok || answer.outcome == Outcome::partial) && answer.database) {
        return base::Result<InferredDatabase> { std::move(*answer.database) };
    }
    if (!answer.code.empty()) return base::fail(answer.code, answer.reason);
    switch (answer.outcome) {
    case Outcome::needs_download: return base::fail(std::string { spec::NEEDS_DOWNLOAD }, answer.reason);
    case Outcome::timed_out: return base::fail("discovery-timeout", answer.reason);
    default: return base::fail("provider-failed", answer.reason);
    }
}

Answer from_result(base::Result<InferredDatabase> result) {
    if (result) return Answer { .outcome = Outcome::ok, .database = std::move(*result) };
    Answer answer;
    answer.code = result.error().code;
    answer.reason = result.error().message;
    answer.outcome = answer.code == spec::NEEDS_DOWNLOAD ? Outcome::needs_download : Outcome::failed;
    return answer;
}

} // namespace mcppls::project
