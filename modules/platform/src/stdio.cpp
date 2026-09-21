module mcppls.platform.stdio;

import std;
import openkal.types;
import openkal.stream;
import mcppls.base.error;

namespace mcppls::platform::stdio {

namespace {

std::mutex gOutputMutex;
std::mutex gErrorMutex;

base::Result<void> write_all(kal_stream stream, std::string_view bytes) {
    std::size_t done { 0 };
    while (done < bytes.size()) {
        const kal_intptr written { kal_stream_write(stream, bytes.data() + done, bytes.size() - done) };
        if (written <= 0) return base::fail("stdio-write", std::format("write failed ({})", -written));
        done += static_cast<std::size_t>(written);
    }
    kal_stream_flush(stream);
    return {};
}

} // namespace

base::Result<std::string> read_input() {
    std::array<char, 65536> buffer {};
    const kal_intptr got { kal_stream_read(kal_stdin(), buffer.data(), buffer.size()) };
    if (got < 0) {
        if (static_cast<int>(-got) == kal_err_closed) return std::string {};
        return base::fail("stdio-read", std::format("read failed ({})", -got));
    }
    return std::string { buffer.data(), static_cast<std::size_t>(got) };
}

base::Result<void> write_output(std::string_view bytes) {
    std::lock_guard lock { gOutputMutex };
    return write_all(kal_stdout(), bytes);
}

base::Result<void> write_error(std::string_view bytes) {
    std::lock_guard lock { gErrorMutex };
    return write_all(kal_stderr(), bytes);
}

} // namespace mcppls::platform::stdio
