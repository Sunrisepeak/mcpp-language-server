// Whether this process runs under a system-call translator, which changes what a program may do
// (termux's PRoot answers ENOSYS to calls a kernel would accept, and rewrites registers a kernel
// would not), and so belongs in a report of why something did not work.
export module mcppls.platform.sandbox;

import std;

export namespace mcppls::platform {

// What the classification reads, gathered by `sandbox()` and given by a test.
struct SandboxFacts {
    std::string selfStatus;                  // /proc/self/status
    std::string tracerName;                  // /proc/<TracerPid>/comm, when there is a tracer and it is readable
    std::vector<std::string> environment;    // "NAME=value"
};

// "proot" when the process is traced by PRoot (the tracer is named proot*), else empty. PROOT_*
// variables decide only when the tracer cannot be read (a hidden /proc); in a process nothing traces
// they mean nothing. Only PRoot is named: a debugger also traces, and is not a sandbox.
std::string classify_sandbox(const SandboxFacts& facts);

// The process's TracerPid, or 0 when there is none or the text has no such line.
std::int64_t tracer_pid(std::string_view selfStatus);

// The answer for this process, computed once. Empty on a system with no /proc (macOS, Windows)
// and when nothing is detected.
const std::string& sandbox();

} // namespace mcppls::platform
