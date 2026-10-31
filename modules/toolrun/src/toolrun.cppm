// One way to run the user's external programs: the build tool, the compiler probes, CMake, git.
//
// Every one of them runs here so that four things are true of all of them at once (design 4.2):
// the program leads a unit of its own and the bound ends the unit, not just the program; the read
// of its streams ends even when something it started still holds them; an implicit run does not
// reach the network; and what happened is written down --- command, environment source, offline
// or not, how long, how it ended, the last of its standard error.
export module mcppls.toolrun;

import std;
import nlohmann.json;
import mcppls.base.error;
import mcpplibs.os;

export namespace mcppls::toolrun {

enum class Network {
    offline,   // the server started this by itself: it must not reach the network (design P3)
    allowed,   // the user asked for it, or allowed it
};

struct Request {
    std::string program;
    std::vector<std::string> arguments;
    std::string workDirectory;
    std::string purpose;                                   // producer, protocol, toolchain, configure, git, ...
    std::string root;                                      // the workspace it belongs to, for the report
    Network network { Network::offline };
    mcpplibs::os::process::bounds bounds {};
    std::chrono::milliseconds soft { 0 };                  // 0: nothing is reported while it runs
    std::string input;
    // nullopt takes the tool environment (mcppls.toolenv), which is what a user's build
    // tool should see. A caller that has its own environment gives it here.
    std::optional<std::vector<std::string>> environment;
    std::chrono::milliseconds environmentWait { 0 };       // how long to wait for the tool environment
    std::function<void(std::chrono::milliseconds)> onSoftDeadline;   // still running after `soft`
};

struct Record {
    std::uint64_t id { 0 };
    std::string program;
    std::vector<std::string> arguments;
    std::string workDirectory;
    std::string purpose;
    std::string root;
    std::string environmentSource;                         // login-shell | editor | given
    bool offline { false };
    std::int64_t durationMs { 0 };
    int exitCode { -1 };
    bool timedOut { false };
    bool unitEnded { false };                              // the bound ended the unit
    bool outputHeldOpen { false };
    bool outputTruncated { false };
    bool errorTruncated { false };
    bool startFailed { false };
    // The program itself said it reached the network. mcpp reports this in the envelope's
    // `effects` from 2026.9.16.1; an offline run that reports it is a defect worth seeing.
    bool networkObserved { false };
    std::string errorTail;                                 // the last lines of standard error
    std::string failure;                                   // why it could not be started at all
};

// Runs the program. `record` receives what was written down, its id included.
base::Result<mcpplibs::os::process::bounded_output> run(const Request& request, Record* record = nullptr);

// The program said it reached the network. Amends the record and tells the recorder again.
void observed_network(std::uint64_t id);

// How many runs are kept at all.
inline constexpr std::size_t KEPT { 50 };

// The most recent runs, newest last, at most `limit` of them.
std::vector<Record> recent(std::size_t limit);

// The record as the journal and the report carry it. The arguments are kept: a build tool's
// arguments are part of what a bug report needs, and they carry no credentials.
nlohmann::json to_json(const Record& record);

// Where a record goes besides the ring above (the workspace journal).
void set_recorder(std::function<void(const Record&)> recorder);

// A run's ending as one number, as reports have shown it: the status the program returned, or -1 when
// its environment ended it (a signal, a console interrupt, the run's own bound).
inline int exit_code(const mcpplibs::os::process::exit_status& status) { return status.by_environment ? -1 : status.code; }

// The program the server would start for `name`, in this spelling: a name that says where it is
// (mcpplibs.os `fs::names_a_place`) only when it is absolute, and a bare name only from the
// absolute directories of `pathList` (PATH when omitted). A relative entry of PATH names a
// different directory from each working directory -- the workspace's, for a language server -- and
// a program found through it would be the workspace's own. On Windows a bare name is found by
// PATHEXT's extensions (mcpplibs.os `env::find_executable`).
std::optional<std::string> find_program(std::string_view name);

// The translator this server runs under, which changes how programs are started (PRoot under termux:
// mcpplibs.os `linux::translator`); empty when none, and in an ok image. Asked once.
const std::string& sandbox();
std::optional<std::string> find_program(std::string_view name, std::string_view pathList);

// How a language server is started, as an editor starts it: the program itself, or, for an image
// (`.okb`, `.oke`), the platform's runner and then the image. The runner is the one the image's
// payload names for this platform (payload.json's `platforms.<platform>.runner`, or `server.runner`
// before payload version 5, with the image under the payload's `bin/`), else
// `okrun` beside the image (`okrun.exe` on Windows). A harness given `--server bin/mcppls.okb`
// therefore starts what the payload's editor starts.
base::Result<std::vector<std::string>> server_command(std::string_view server);

} // namespace mcppls::toolrun
