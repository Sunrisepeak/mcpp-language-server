# Darwin child-pipe writes

Register: [UP-O1](https://github.com/Sunrisepeak/mcpp-language-server/issues/24#issuecomment-6028338032). Compensation: WA-PLATFORM-001 in
modules/platform/src/process.cpp. This is a platform runtime issue and belongs
outside the clangd workaround registry.

The Intel runner executes the actual cross-built dev/release binaries. Both
failed by signal 13 in the existing blocked-peer shutdown test (CI run
37552260398, jobs 112572688643/112572688644). openkal-macos 0.12.0's env.cpp
explicitly records that its pipe writes can trigger SIGPIPE; its C-library
layer does not install a kernel signal disposition.

Each parent-owned child-input write descriptor now receives F_SETNOSIGPIPE
before spawn. Darwin's existing native syscall convention is used because the
program supplies its own C ABI. Failure closes both input ends and the work
handle, and rejects spawn. The change is local to that descriptor. Constants
and semantics: [Apple XNU fcntl.h](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/fcntl.h).

Evidence: Linux process tests pass, 2 binaries; the main test has 23 cases and
86 assertions. Intel and arm64 process tests cross-compile (2 binaries each). Fixed-byte
native CI remains pending. The new regression explicitly
waits for peer exit, writes to its still-open parent end and requires a returned
process-write error. Existing blocked-writer shutdown remains the lifecycle
canary. No native success is inferred from cross-linking.

Remove WA-PLATFORM-001 only when selected openkal-macos reliably reports closed
pipe writes on both macOS architectures with default parent SIGPIPE handling.
Other arbitrary openkal streams do not gain a blanket protection from this fix.

Native follow-up: Intel dev CI run 37553665184 / job 112577351043 at
1fa6975 passes the blocked-peer and closed-peer write canaries and both process
binaries (main: 23 cases, 80 assertions). The job fails later in a separate
pack platform fixture, which has now been corrected. Native release proof
is still pending because that fixture ran before its process tests.
