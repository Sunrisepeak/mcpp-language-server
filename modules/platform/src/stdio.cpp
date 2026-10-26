module mcppls.platform.stdio;

import std;
import mcpplibs.os;
import mcppls.base.error;

namespace mcppls::platform::stdio {

namespace {

// One writer at a time on each stream: standard output carries the protocol, and two messages
// interleaved are neither.
std::mutex gOutputMutex;
std::mutex gErrorMutex;

} // namespace

base::Result<std::string> read_input() {
    auto got = mcpplibs::os::stdio::read_input();
    if (!got) return base::fail("stdio-read", got.error().message());
    return std::move(*got);
}

base::Result<void> write_output(std::string_view bytes) {
    std::lock_guard lock { gOutputMutex };
    if (auto written = mcpplibs::os::stdio::write_output(bytes); !written) return base::fail("stdio-write", written.error().message());
    (void)mcpplibs::os::stdio::flush_output();
    return {};
}

base::Result<void> write_error(std::string_view bytes) {
    std::lock_guard lock { gErrorMutex };
    if (auto written = mcpplibs::os::stdio::write_error(bytes); !written) return base::fail("stdio-write", written.error().message());
    return {};
}

} // namespace mcppls::platform::stdio
