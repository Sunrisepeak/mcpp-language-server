// Starting a program when the system has no `execveat` with a directory descriptor, which is what
// termux's PRoot answers (ENOSYS). The vendored openkal-linux (vendor/README.md) then starts the
// program with `execve` and an absolute name computed before the duplication. This test makes a
// system that has the call behave as one that does not, through the flag the library sets itself
// when it meets ENOSYS, and starts a program and a `#!` script through that path.
//
// The end-to-end proof is the `proot` CI job: mcppls itself, under a real PRoot.
import std;
import mcppls.os;
import mcppls.testing;
import mcppls.base.path;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.platform.stdio;

// Not openkal's interface: the flag of vendor/openkal-linux/src/process.cpp, exposed for this test.
extern "C" void okl_execveat_unavailable(int unavailable);

namespace platform = mcppls::platform;
namespace base = mcppls::base;
namespace fs = platform::fs;

namespace {

// The hook exists only in the Linux package; other systems have no such fallback to force.
void set_unavailable(int unavailable) {
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::linux) okl_execveat_unavailable(unavailable);
}

std::string self_path() {
    const auto arguments = platform::env::arguments();
    std::string path { arguments.empty() ? std::string {} : arguments.front() };
    if (!base::is_absolute_path(path)) path = base::join_path(fs::current_directory(), path);
    return base::normalize_path(path);
}

std::string run(const platform::SpawnOptions& options, int* status = nullptr) {
    auto process = platform::Process::spawn(options);
    if (!process) return "spawn-failed: " + process.error().message;
    std::string collected;
    while (true) {
        auto chunk = process->read_output();
        if (!chunk || chunk->empty()) break;
        collected += *chunk;
    }
    auto code = process->wait();
    if (status) *status = code ? *code : -1;
    return collected;
}

} // namespace

int main() {
    const auto arguments = platform::env::arguments();
    if (arguments.size() >= 2 && arguments[1] == "--child") {
        (void)platform::stdio::write_output(std::format("child:{}", arguments.size() > 2 ? arguments[2] : ""));
        std::_Exit(7);
    }

    using namespace mcppls::testing;
    if constexpr (mcppls::os::FAMILY != mcppls::os::Family::linux) return report();

    const std::string self { self_path() };
    const std::string root { base::join_path(platform::dirs::temp_directory(),
        std::format("mcppls-test-fallback-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
    expect(fs::create_directories(root).has_value());
    const std::string script { base::join_path(root, "script.sh") };
    expect(fs::write_file(script, "#!/bin/sh\necho \"script:$1\"\nexit 5\n").has_value());
    expect(platform::fs::make_executable(std::array<std::string, 1> { script }).has_value());

    "a program starts with execveat"_test = [&] {
        set_unavailable(0);
        int status { 0 };
        const std::string out { run({ .program = self, .arguments = { "--child", "a" }, .pipeInput = false }, &status) };
        expect(out == "child:a") << out;
        expect(status == 7) << status;
    };

    "a program starts when execveat is unavailable"_test = [&] {
        set_unavailable(1);
        int status { 0 };
        const std::string out { run({ .program = self, .arguments = { "--child", "b" }, .pipeInput = false }, &status) };
        expect(out == "child:b") << out;
        expect(status == 7) << status;
    };

    "a script with an interpreter starts when execveat is unavailable"_test = [&] {
        set_unavailable(1);
        int status { 0 };
        const std::string out { run({ .program = script, .arguments = { "x" }, .pipeInput = false }, &status) };
        expect(out == "script:x\n") << out;
        expect(status == 5) << status;
    };

    "the work directory is honoured on the fallback path"_test = [&] {
        set_unavailable(1);
        const std::string out { run({ .program = script, .arguments = { "y" }, .workDirectory = root, .pipeInput = false }) };
        expect(out == "script:y\n") << out;
    };

    "a missing program is still reported as missing on the fallback path"_test = [&] {
        set_unavailable(1);
        auto process = platform::Process::spawn({ .program = base::join_path(root, "absent"), .pipeInput = false });
        expect(!process.has_value());
        if (!process) expect(!process.error().message.contains("not supported")) << process.error().message;
    };

    set_unavailable(0);
    fs::remove_all(root);
    return report();
}
