module mcppls.ai.mcp.daemon;

import std;
import nlohmann.json;
import mcppls.os;
import mcppls.base.error;
import mcppls.base.log;
import mcppls.base.path;
import mcppls.base.text;
import mcppls.base.version;
import mcppls.platform.dirs;
import mcppls.platform.fs;
import mcppls.platform.net;
import mcppls.platform.process;
import mcppls.platform.stdio;
import mcppls.project.model;
import mcppls.orchestrator.kernel;
import mcppls.orchestrator.workspace;
import mcppls.ai.mcp.server;

namespace mcppls::ai::mcp {

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace log = base::log;
namespace net = platform::net;

constexpr std::string_view MARK { "mcppls-daemon" };

// The connections a daemon has open, shared with the threads that read them.
struct Connections {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<net::Connection>> open;
    std::atomic<bool> stopping { false };

    std::shared_ptr<net::Connection> find(std::uint64_t id) {
        const std::lock_guard lock { mutex };
        const auto found = open.find(id);
        return found == open.end() ? nullptr : found->second;
    }
};

bool send_line(net::Connection& connection, const Json& message) { return static_cast<bool>(connection.write(message.dump() + "\n")); }

// The first line of a connection, and what came after it in the same reads.
base::Result<std::pair<std::string, std::string>> read_first_line(net::Connection& connection) {
    std::string buffer;
    while (buffer.find('\n') == std::string::npos) {
        auto chunk = connection.read();
        if (!chunk) return std::unexpected { chunk.error() };
        if (chunk->empty()) return base::fail("daemon-closed", "the daemon closed the connection");
        buffer += *chunk;
        if (buffer.size() > 65536) return base::fail("daemon-protocol", "no line from the daemon");
    }
    const std::size_t end { buffer.find('\n') };
    return std::pair { buffer.substr(0, end), buffer.substr(end + 1) };
}

base::Result<std::pair<net::Connection, std::string>> open_session_now(const DaemonInfo& daemon, std::string_view session) {
    auto connection = net::Connection::connect_local(daemon.port);
    if (!connection) return std::unexpected { connection.error() };
    if (!send_line(*connection, Json { { "token", daemon.token }, { "session", std::string { session } } })) {
        return base::fail("daemon-write", "cannot write to the daemon");
    }
    auto greeting = read_first_line(*connection);
    if (!greeting) return std::unexpected { greeting.error() };
    const Json answer = Json::parse(greeting->first, nullptr, false);
    if (answer.is_discarded() || !answer.value("ok", false)) {
        return base::fail("daemon-refused", answer.is_object() ? answer.value("error", std::string { "refused" }) : std::string { "refused" });
    }
    return std::pair { std::move(*connection), std::move(greeting->second) };
}

// A daemon that accepts but never answers (a stale port now used by something else) must not hold an entry forever.
base::Result<std::pair<net::Connection, std::string>> open_session(const DaemonInfo& daemon, std::string_view session) {
    auto opening = std::make_shared<std::promise<base::Result<std::pair<net::Connection, std::string>>>>();
    auto opened = opening->get_future();
    std::thread { [opening, daemon, name = std::string { session }] { opening->set_value(open_session_now(daemon, name)); } }.detach();
    if (opened.wait_for(std::chrono::seconds { 30 }) != std::future_status::ready) return base::fail("daemon-timeout", "the daemon did not answer the connection in time");
    return opened.get();
}

} // namespace

std::string discovery_path(std::string_view root) {
    const std::string canonical { platform::fs::exists(root) ? platform::fs::canonical_path(root) : base::normalize_path(root) };
    return base::join_path(platform::dirs::cache_directory(), base::join_path("workspaces", base::join_path(project::workspace_key(canonical), "daemon.json")));
}

std::optional<DaemonInfo> find_daemon(std::string_view root) {
    auto text = platform::fs::read_file(discovery_path(root));
    if (!text) return std::nullopt;
    const Json info = Json::parse(*text, nullptr, false);
    if (info.is_discarded() || !info.is_object()) return std::nullopt;
    DaemonInfo daemon { info.value("port", 0), info.value("token", std::string {}), info.value("version", std::string {}), info.value("root", std::string {}) };
    // A daemon of another version keeps serving its own entries; this one does not use it (design 11).
    if (daemon.port <= 0 || daemon.token.empty() || daemon.version != base::VERSION) return std::nullopt;
    return daemon;
}

int run_daemon(const DaemonOptions& options) {
    const std::string discovery { discovery_path(options.server.kernel.root.empty() ? platform::fs::current_directory() : options.server.kernel.root) };
    // Nobody reads a daemon's standard error, from its first line on: its log is a file beside its
    // discovery file. A line written to a channel nobody drains blocks once the channel is full.
    (void)platform::fs::create_directories(base::parent_path(discovery));
    const std::string logPath { base::join_path(base::parent_path(discovery), "daemon.log") };
    auto logFile = std::make_shared<std::ofstream>(std::filesystem::path { logPath }, std::ios::app);
    log::set_sink([logFile](std::string_view line) {
        if (*logFile) {
            *logFile << line;
            logFile->flush();
        }
    });
    auto kernel = orchestrator::Kernel::start(options.server.kernel);
    const std::string root { kernel->root() };

    auto listener = net::Listener::listen_local();
    if (!listener) {
        log::error("daemon for {}: {}", root, listener.error().message);
        kernel->shut_down();
        return 2;
    }
    const std::string token { net::random_token() };
    (void)platform::fs::create_directories(base::parent_path(discovery));
    const Json info { { "port", listener->port() }, { "token", token }, { "version", std::string { base::VERSION } }, { "root", root } };
    if (auto written = platform::fs::write_file_atomic(discovery, info.dump()); !written) {
        log::error("daemon for {}: cannot write {}: {}", root, discovery, written.error().message);
        kernel->shut_down();
        return 2;
    }
    log::info("daemon for {} on 127.0.0.1:{}", root, listener->port());

    auto connections = std::make_shared<Connections>();
    orchestrator::Kernel* handle { kernel.get() };
    std::thread { [listening = std::make_shared<net::Listener>(std::move(*listener)), connections, handle, token] {
        std::uint64_t nextId { 1 };
        while (!connections->stopping) {
            auto accepted = listening->accept();
            if (!accepted) return;
            auto connection = std::make_shared<net::Connection>(std::move(*accepted));
            const std::uint64_t id { nextId++ };
            {
                const std::lock_guard lock { connections->mutex };
                connections->open[id] = connection;
            }
            std::thread { [connection, connections, handle, token, id] {
                std::string buffer;
                bool greeted { false };
                bool reading { true };
                while (reading && !connections->stopping) {
                    auto chunk = connection->read();
                    if (!chunk || chunk->empty()) break;
                    buffer += *chunk;
                    for (auto& line : take_lines(buffer)) {
                        Json message = Json::parse(line, nullptr, false);
                        if (!greeted) {
                            const std::string session { message.is_object() ? message.value("session", std::string { "mcp" }) : std::string {} };
                            if (message.is_discarded() || !message.is_object() || message.value("token", std::string {}) != token || (session != "mcp" && session != "control")) {
                                (void)send_line(*connection, Json { { "ok", false }, { "error", "a connection opens with the daemon's token and a session of mcp or control" } });
                                reading = false;
                                break;
                            }
                            greeted = true;
                            (void)send_line(*connection, Json { { "ok", true }, { "version", std::string { base::VERSION } } });
                            handle->post_external(Json { { std::string { MARK }, Json { { "connection", id }, { "event", "open" }, { "session", session } } } });
                            continue;
                        }
                        if (message.is_discarded()) message = Json { { "mcppls-parse-error", line.substr(0, 200) } };
                        handle->post_external(Json { { std::string { MARK }, Json { { "connection", id }, { "event", "message" } } }, { "message", std::move(message) } });
                    }
                }
                if (!connections->stopping) handle->post_external(Json { { std::string { MARK }, Json { { "connection", id }, { "event", "close" } } } });
            } }.detach();
        }
    } }.detach();

    std::map<std::uint64_t, std::unique_ptr<Session>> sessions;
    std::set<std::uint64_t> controls;
    auto lastActive = Clock::now();
    const auto started = Clock::now();
    bool stop { false };
    while (!stop) {
        auto message = kernel->next_external(std::chrono::seconds { 15 });
        if (!message) {
            if (sessions.empty() && controls.empty() && Clock::now() - lastActive > options.idle) {
                log::info("daemon for {}: idle for {} minutes, exiting", root, options.idle.count());
                break;
            }
            continue;
        }
        lastActive = Clock::now();
        const Json mark = message->value(std::string { MARK }, Json::object());
        const std::uint64_t id { mark.value("connection", std::uint64_t { 0 }) };
        const std::string event { mark.value("event", std::string {}) };
        auto connection = connections->find(id);
        if (event == "open" && connection) {
            if (mark.value("session", std::string {}) == "control") {
                controls.insert(id);
            } else {
                sessions[id] = std::make_unique<Session>(*kernel, options.server.toolTimeout,
                                                         [connection](const Json& reply) { (void)send_line(*connection, reply); }, options.server.model);
            }
        } else if (event == "message" && connection) {
            const Json inner = message->value("message", Json {});
            if (const auto session = sessions.find(id); session != sessions.end()) {
                if (inner.contains("mcppls-parse-error")) (void)send_line(*connection, Json { { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", Json { { "code", -32700 }, { "message", "a line is not JSON" } } } });
                else session->second->handle(inner);
            } else if (controls.contains(id)) {
                const std::string method { inner.value("method", std::string {}) };
                if (method == "status") {
                    (void)send_line(*connection, Json { { "root", root }, { "version", std::string { base::VERSION } }, { "sessions", sessions.size() },
                                                        { "uptimeSeconds", std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count() },
                                                        { "status", kernel->status() } });
                } else if (method == "stop") {
                    (void)send_line(*connection, Json { { "ok", true } });
                    stop = true;
                } else {
                    (void)send_line(*connection, Json { { "error", std::format("unknown control method {}", method) } });
                }
            }
        } else if (event == "close") {
            sessions.erase(id);
            controls.erase(id);
            if (connection) connection->close();
            const std::lock_guard lock { connections->mutex };
            connections->open.erase(id);
        }
    }
    connections->stopping = true;
    {
        const std::lock_guard lock { connections->mutex };
        for (auto& [id, connection] : connections->open) connection->close();
    }
    // Only this daemon's own discovery file: a newer daemon may have replaced it.
    if (auto current = platform::fs::read_file(discovery); current && Json::parse(*current, nullptr, false).value("token", std::string {}) == token) {
        platform::fs::remove_all(discovery);
    }
    sessions.clear();
    kernel->shut_down();
    log::info("daemon for {} stopped", root);
    return 0;
}

base::Result<DaemonInfo> start_daemon(std::string_view executable, std::string_view root, std::span<const std::string> arguments, std::chrono::milliseconds timeout) {
    const std::string discovery { discovery_path(root) };
    const std::string before { platform::fs::read_file(discovery).value_or(std::string {}) };
    platform::SpawnOptions spawn;
    spawn.program = std::string { executable };
    spawn.arguments = { "daemon", "run", "--root", std::string { root } };
    spawn.arguments.insert(spawn.arguments.end(), arguments.begin(), arguments.end());
    spawn.workDirectory = std::string { root };
    // A daemon holding its starter's standard output would keep an agent waiting for that output to end.
    // POSIX: fresh channels, closed once the daemon is up. Windows: no streams at all, since a start that
    // places streams hands the child every inheritable handle, the starter's own streams among them.
    if constexpr (mcppls::os::FAMILY == mcppls::os::Family::windows) {
        spawn.noStreams = true;
    } else {
        spawn.pipeInput = true;
        spawn.pipeOutput = true;
        spawn.pipeError = true;
    }
    spawn.detached = true;
    auto process = platform::Process::spawn(spawn);
    if (!process) return std::unexpected { process.error() };
    const auto until = Clock::now() + timeout;
    while (Clock::now() < until) {
        const std::string current { platform::fs::read_file(discovery).value_or(std::string {}) };
        if (!current.empty() && current != before) {
            if (auto daemon = find_daemon(root)) {
                process->detach();
                return *daemon;
            }
        }
        if (auto exited = process->wait_for(std::chrono::milliseconds { 100 }); exited && exited->has_value()) {
            return base::fail("daemon-exited", std::format("the daemon exited with status {} before it was ready", **exited));
        }
    }
    process->detach();
    return base::fail("daemon-timeout", "the daemon did not say where it listens in time");
}

base::Result<Json> control(const DaemonInfo& daemon, std::string_view method) {
    auto opened = open_session(daemon, "control");
    if (!opened) return std::unexpected { opened.error() };
    auto& [connection, rest] = *opened;
    if (!send_line(connection, Json { { "method", std::string { method } } })) return base::fail("daemon-write", "cannot write to the daemon");
    std::string buffer { rest };
    while (buffer.find('\n') == std::string::npos) {
        auto chunk = connection.read();
        if (!chunk || chunk->empty()) break;
        buffer += *chunk;
    }
    const Json answer = Json::parse(buffer.substr(0, buffer.find('\n')), nullptr, false);
    connection.close();
    if (answer.is_discarded()) return base::fail("daemon-protocol", "the daemon did not answer");
    return answer;
}

int relay_mcp(const DaemonInfo& daemon) {
    auto opened = open_session(daemon, "mcp");
    if (!opened) {
        log::error("cannot reach the daemon on 127.0.0.1:{}: {}", daemon.port, opened.error().message);
        return 2;
    }
    auto connection = std::make_shared<net::Connection>(std::move(opened->first));
    if (!opened->second.empty()) (void)platform::stdio::write_output(opened->second);
    // The agent's messages to the daemon, as they come; the end of them ends this side of the connection.
    std::thread { [connection] {
        while (true) {
            auto chunk = platform::stdio::read_input();
            if (!chunk || chunk->empty()) break;
            if (!connection->write(*chunk)) break;
        }
        connection->shutdown_write();
    } }.detach();
    while (true) {
        auto chunk = connection->read();
        if (!chunk || chunk->empty()) break;
        if (!platform::stdio::write_output(*chunk)) break;
    }
    connection->close();
    return 0;
}

} // namespace mcppls::ai::mcp
