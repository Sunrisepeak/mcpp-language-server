// The diagnostic bundle (issue #23 fix plan F18): redaction in every spelling a path, a name or a
// secret reaches a log in, the check that finds what is left, the zip it is packed in, and a whole
// bundle written for a user whose name is in every path.
import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.os;
import mcppls.base.path;
import mcppls.base.sha256;
import mcppls.platform.dirs;
import mcppls.platform.env;
import mcppls.platform.fs;
import mcppls.platform.process;
import mcppls.bundle.redact;
import mcppls.bundle.zip;
import mcppls.bundle.writer;

namespace bundle = mcppls::bundle;
namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;
using Json = nlohmann::json;

namespace {

// name -> content, through the module's own reader, which checks each entry's size and CRC. That the
// archive is a zip any program opens is checked on the real bundle in CI (python -m zipfile -t).
std::map<std::string, std::string> unzip(std::string_view archive) {
    auto files = bundle::read_archive(archive);
    if (!files) throw std::runtime_error { files.error() };
    return std::move(*files);
}

// Raw deflate data back, by packing it as the one entry of an archive.
std::string inflate_through_archive(std::string_view data) {
    bundle::ZipEntry entry { "data", bundle::crc32(data), data.size(), 8, bundle::deflate(data) };
    const std::array<bundle::ZipEntry, 1> entries { std::move(entry) };
    return unzip(bundle::zip_archive(entries, std::chrono::system_clock::now())).at("data");
}

bool contains_folded(std::string_view text, std::string_view needle) {
    const auto fold = [](std::string_view s) {
        std::string out { s };
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    return fold(text).contains(fold(needle));
}

bundle::Identity linux_user() {
    return bundle::Identity { { "/home/alicewonder" }, { "alicewonder" }, { "devbox-alice-7" }, {} };
}

bundle::Identity windows_user() {
    return bundle::Identity { { "C:/Users/runneradmin" }, { "runneradmin" }, { "fv-az1234-567" }, {} };
}

// The child a whole bundle is written in: another HOME, another user, another cache.
constexpr std::string_view LONG_USER { "averylongusernameforbundles" };

int write_bundle_as_child(const std::string& directory, std::string_view mode) {
    const std::string home { mcppls::platform::dirs::home_directory() };
    const std::string cache { mcppls::platform::dirs::cache_directory() };
    const std::string root { base::join_path(home, "projects/hello") };
    const std::string rootCache { base::join_path(cache, "workspaces/hello-1") };
    (void)fs::create_directories(base::join_path(root, "src"));
    (void)fs::create_directories(base::join_path(cache, "logs"));
    std::string backslashed { home };
    std::ranges::replace(backslashed, '/', '\\');
    const std::string lines { std::format(
        "mcppls 2026-09-26T01:02:03Z [info] log file {0}/.cache/mcppls/logs/server.log\n"
        "mcppls 2026-09-26T01:02:03Z [info] root {1}\n"
        "mcppls 2026-09-26T01:02:04Z [warning] clangd ({1}): E[01:02:04.000] Failed to build module hello; due to Failed to compile {1}/src/hello.cppm\n"
        "mcppls 2026-09-26T01:02:05Z [info] uri file://{0}/projects/hello/src/main.cpp as {2}\\\\projects\\\\hello\n"
        "mcppls 2026-09-26T01:02:06Z [info] {3} opened main.cpp, mail {3}@example.org, GITHUB_TOKEN=ghp_abcdefghijklmnopqrstuvwxyz0123456789\n",
        home, root, backslashed, LONG_USER) };
    (void)fs::write_file(base::join_path(cache, "logs/server-20260926-010203-ab12.log"), lines);
    (void)fs::write_file(base::join_path(cache, "logs/server-20260926-010203-ab12.log.1"), lines);
    const std::string incident { base::join_path(rootCache, "incidents/20260926T010207.000Z-spin") };
    (void)fs::create_directories(incident);
    const Json incidentJson {
        { "kind", "spin" }, { "root", root }, { "files", Json::array({ base::join_path(root, "src/main.cpp") }) },
        { "excerpts", Json::array({ Json { { "file", base::join_path(root, "src/main.cpp") }, { "line", 3 }, { "buffer", "import hello.greet;" },
                                           { "disk", "import hello." } } }) },
    };
    (void)fs::write_file(base::join_path(incident, "incident.json"), incidentJson.dump(2));
    (void)fs::write_file(base::join_path(incident, "clangd.log"), lines);
    (void)fs::create_directories(base::join_path(rootCache, "contexts/default/cdb"));
    const Json database = Json::array({ Json { { "directory", root }, { "file", base::join_path(root, "src/main.cpp") },
                                               { "arguments", Json::array({ "clang++", "-DAPI_KEY=\"s3cr3t-value\"", "-DVERSION=3", "-c", base::join_path(root, "src/main.cpp") }) } } });
    (void)fs::write_file(base::join_path(rootCache, "contexts/default/cdb/compile_commands.json"), database.dump(1));
    const Json report {
        { "generatedAt", "2026-09-26T01:02:08Z" },
        { "server", Json { { "name", "mcppls" }, { "version", "test" }, { "logFile", base::join_path(cache, "logs/server-20260926-010203-ab12.log") } } },
        { "client", Json { { "name", "test client" }, { "version", "1" } } },
        { "payload", Json { { "directory", "" }, { "clangdVersion", "23.1.0" } } },
        { "roots", Json::array({ Json { { "root", root }, { "cacheDirectory", rootCache }, { "plan", Json { { "entries", 1 }, { "openSources", Json::array({ base::join_path(root, "src/main.cpp") }) } } } } }) },
        { "logTail", Json::array({ std::format("root {}", root) }) },
    };
    bundle::BundleInput input;
    input.report = report;
    input.client = Json { { "extension", Json { { "version", "0.0.5" } } }, { "otherCppExtensions", Json::array() } };
    input.clientLog = std::format("[10:00:00] Starting {}/.vscode/extensions/mcppls/payload/bin/mcppls serve\n", home);
    bundle::BundleOptions options;
    options.output = base::join_path(directory, mode == "excerpts" ? "with-excerpts.zip" : mode == "cap" ? "capped.zip" : "without-excerpts.zip");
    options.sourceExcerpts = mode != "none";
    if (mode == "cap") {
        // An older session's log that compresses badly and does not fit a bundle capped at 512 KiB.
        std::string noise;
        std::uint32_t state { 7 };
        while (noise.size() < 3 * 1024 * 1024) {
            state = state * 1103515245U + 12345U;
            noise += std::format("{:08x}", state);
            if (noise.size() % 64 == 0) noise += '\n';
        }
        (void)fs::write_file(base::join_path(cache, "logs/server-20260925-010203-cd34.log"), noise);
        options.sizeCap = 512 * 1024;
    }
    auto written = bundle::write_bundle(input, options);
    if (!written) {
        std::println("{}", Json { { "error", written.error().message }, { "residue", written.error().residue } }.dump());
        return 1;
    }
    std::println("{}", Json { { "path", written->path }, { "bytes", written->bytes }, { "redactions", written->redactions }, { "home", home } }.dump());
    return 0;
}

} // namespace

int main() {
    {
        const auto arguments = mcppls::platform::env::arguments();
        if (arguments.size() == 4 && arguments[1] == "bundle-child") std::_Exit(write_bundle_as_child(arguments[2], arguments[3]));
    }
    using namespace mcppls::testing;

    "a home directory is ~ in every spelling of a POSIX path"_test = [] {
        bundle::Redactor redactor { linux_user() };
        expect(redactor.redact("/home/alicewonder/src/main.cpp") == "~/src/main.cpp");
        expect(redactor.redact("file:///home/alicewonder/x.cppm") == "file://~/x.cppm");
        expect(redactor.redact(R"({"p":"\/home\/alicewonder\/x"})") == R"({"p":"~\/x"})");
        expect(redactor.redact("/HOME/ALICEWONDER/x") == "~/x") << "case-insensitive, as macOS and Windows file systems are";
        expect(redactor.redact("see /home/alicewonder.") == "see ~.") << "a sentence ending after the path";
        expect(redactor.redact("/home/alicewonder") == "~");
    };

    "a longer name, or the tail of another path, is not the home directory"_test = [] {
        bundle::Redactor redactor { linux_user() };
        expect(redactor.redact("/home/alicewonderland/x") == "/home/<user-2>/x") << redactor.redact("/home/alicewonderland/x");
        expect(redactor.redact("/data/home/alicewonder/x") == "/data/home/<user>/x") << redactor.redact("/data/home/alicewonder/x");
        expect(redactor.redact("/home/alicewonder.old/x") == "/home/<user-3>/x") << redactor.redact("/home/alicewonder.old/x");
        expect(redactor.redact("/ab/home/alicewonder/x") == "/ab/home/<user>/x") << redactor.redact("/ab/home/alicewonder/x");
    };

    "a path glued to a compiler option is still a path"_test = [] {
        bundle::Redactor posix { linux_user() };
        expect(posix.redact("-I/home/alicewonder/include -L/home/alicewonder/lib") == "-I~/include -L~/lib");
        expect(posix.redact("-I/home/bob/include") == "-I/home/<user-2>/include");
        bundle::Redactor windows { windows_user() };
        expect(windows.redact(R"(-IC:\Users\runneradmin\inc /IC:\Users\runneradmin\inc)") == R"(-I~\inc /I~\inc)");
        auto hidden = linux_user();
        hidden.workspaces = { "/tmp/work/proj" };
        bundle::Redactor workspace { hidden };
        expect(workspace.redact(R"(["-I/tmp/work/proj/include", "-fmodule-file=a=/tmp/work/proj/a.pcm"])") == R"(["-I<workspace>/include", "-fmodule-file=a=<workspace>/a.pcm"])");
        // macOS gives /var, /tmp and /etc a second spelling under /private: the same directory either way.
        expect(workspace.redact("/private/tmp/work/proj/src/a.cpp") == "<workspace>/src/a.cpp");
        auto mac = linux_user();
        mac.workspaces = { "/private/var/folders/s6/T/proj" };
        bundle::Redactor macos { mac };
        expect(macos.redact("/var/folders/s6/T/proj/src/a.cpp and /private/var/folders/s6/T/proj/b.cpp") == "<workspace>/src/a.cpp and <workspace>/b.cpp");
        expect(macos.redact("/var/folders/s6/T/project2/c.cpp") == "/var/folders/s6/T/project2/c.cpp") << "another directory";
    };

    "a Windows profile is ~ with either separator, escaped, encoded, from WSL and by its 8.3 name"_test = [] {
        bundle::Redactor redactor { windows_user() };
        expect(redactor.redact(R"(C:\Users\runneradmin\AppData\Local\Temp)") == R"(~\AppData\Local\Temp)");
        expect(redactor.redact(R"("C:\\Users\\runneradmin\\x")") == R"("~\\x")");
        expect(redactor.redact(R"("C:\\\\Users\\\\runneradmin\\\\x")") == R"("~\\\\x")") << "JSON inside JSON";
        expect(redactor.redact("c:/users/RUNNERADMIN/x") == "~/x");
        expect(redactor.redact("file:///c%3A/Users/runneradmin/x") == "file:///~/x");
        expect(redactor.redact("file:///c%3a/Users/runneradmin/x") == "file:///~/x");
        expect(redactor.redact(R"(C:\Users\RUNNER~1\AppData\Local\Temp)") == R"(~\AppData\Local\Temp)");
        expect(redactor.redact("TEMP is RUNNER~1") == "TEMP is <user>");
        expect(redactor.redact("/mnt/c/Users/runneradmin/x") == "~/x");
        expect(redactor.redact(R"(D:\a\_temp\x)") == R"(D:\a\_temp\x)");
        expect(redactor.redact("runneradmin started it") == "<user> started it");
    };

    "a profile with a space in its name"_test = [] {
        bundle::Redactor redactor { bundle::Identity { { "C:/Users/John Doe" }, { "John Doe" }, {}, {} } };
        expect(redactor.redact(R"(C:\Users\John Doe\src)") == R"(~\src)");
        expect(redactor.redact("file:///c%3A/Users/John%20Doe/src") == "file:///~/src");
        expect(redactor.redact(R"(C:\Users\JOHNDO~1\src)") == R"(~\src)");
        expect(redactor.redact(R"(C:\Users\Jane Roe\src)") == R"(C:\Users\<user-2>\src)") << redactor.redact(R"(C:\Users\Jane Roe\src)");
    };

    "other people's profile directories get their own placeholder, the same one every time"_test = [] {
        bundle::Redactor redactor { linux_user() };
        const std::string first { redactor.redact("/home/bob/a /Users/carol/b C:\\Users\\dave\\c /mnt/d/Users/erin/d") };
        expect(first == "/home/<user-2>/a /Users/<user-3>/b C:\\Users\\<user-4>\\c /mnt/d/Users/<user-5>/d") << first;
        expect(redactor.redact("/home/bob/z") == "/home/<user-2>/z") << "the same original, the same placeholder";
        expect(redactor.redact(R"(C:\Users\Public\x /Users/Shared/y)") == R"(C:\Users\Public\x /Users/Shared/y)");
        expect(redactor.redact("https://github.com/users/frank") == "https://github.com/users/frank") << "a URL path is not a profile";
    };

    "a common or short user name is replaced only where it names a directory"_test = [] {
        bundle::Redactor redactor { bundle::Identity { { "/home/runner" }, { "runner" }, { "ubuntu" }, {} } };
        expect(redactor.redact("/home/runner/work/x") == "~/work/x");
        expect(redactor.redact("/opt/runner/x") == "/opt/<user>/x");
        expect(redactor.redact("the runner is idle on ubuntu") == "the runner is idle on ubuntu");
        expect(redactor.residue("the runner is idle on ubuntu").empty()) << "a bundle of a user called runner can still be written";
        bundle::Redactor root { bundle::Identity { { "/root" }, { "root" }, {}, {} } };
        const std::string redacted { root.redact("/root/.cache/mcppls and /usr/share/rootcerts and /opt/root-6/bin") };
        expect(redacted == "~/.cache/mcppls and /usr/share/rootcerts and /opt/root-6/bin") << redacted;
        expect(root.residue(redacted).empty()) << "a container's root user can export a bundle";
        expect(!bundle::distinctive_name("a"));
        expect(!bundle::distinctive_name("bob"));
        expect(!bundle::distinctive_name("admin"));
        expect(!bundle::distinctive_name("Runner"));
        expect(bundle::distinctive_name("runneradmin"));
        expect(bundle::distinctive_name("speak"));
    };

    "a distinctive name is replaced as a word, not inside another word, and not as a JSON key"_test = [] {
        bundle::Redactor redactor { linux_user() };
        expect(redactor.redact("Alicewonder, alicewonder_dev and alicewonder.log") == "<user>, <user>_dev and <user>.log");
        expect(redactor.redact("alicewonderful") == "alicewonderful");
        expect(redactor.redact(R"({"alicewonder": "alicewonder"})") == R"({"alicewonder": "<user>"})");
        expect(redactor.redact("built on devbox-alice-7 and devbox-alice-7.local") == "built on <host> and <host>.local");
    };

    "project roots are hidden when asked, before the home they are under"_test = [] {
        auto identity = linux_user();
        identity.workspaces = { "/home/alicewonder/proj", "/srv/other" };
        bundle::Redactor redactor { identity };
        expect(redactor.redact("/home/alicewonder/proj/src/a.cpp and /srv/other/b.cpp and /home/alicewonder/c") == "<workspace>/src/a.cpp and <workspace-2>/b.cpp and ~/c");
    };

    "secrets are replaced by their shape and by their names"_test = [] {
        bundle::Redactor redactor { linux_user() };
        expect(redactor.redact("token ghp_abcdefghijklmnopqrstuvwxyz0123456789 end") == "token <redacted> end");
        expect(redactor.redact("github_pat_11ABCDEFG0123456789_abcdefghijklmnop") == "<redacted>");
        expect(redactor.redact("key sk-proj-abcdefghijklmnopqrstuvwx") == "key <redacted>");
        expect(redactor.redact("AKIAABCDEFGHIJKLMNOP") == "<redacted>");
        expect(redactor.redact("desk-abcdefghijklmnopqrstuvwxyz") == "desk-abcdefghijklmnopqrstuvwxyz") << "sk- inside a word is no key";
        expect(redactor.redact(R"({"apiKey": "abc123", "authToken":"x", "password" : "hunter2"})")
               == R"({"apiKey": "<redacted>", "authToken":"<redacted>", "password" : "<redacted>"})");
        expect(redactor.redact(R"({"semanticTokens": {"full": true}, "tokenTypes": ["keyword"]})")
               == R"({"semanticTokens": {"full": true}, "tokenTypes": ["keyword"]})");
        expect(redactor.redact("export GITHUB_TOKEN=abcdef MCPP_HOME=/opt/mcpp") == "export GITHUB_TOKEN=<redacted> MCPP_HOME=/opt/mcpp");
        expect(redactor.redact("--api-key=xyz --verbose") == "--api-key=<redacted> --verbose");
        expect(redactor.redact(R"(["-DAPI_KEY=\"s3cr3t\"", "-DVERSION=3"])") == R"(["-DAPI_KEY=<redacted>", "-DVERSION=3"])")
            << redactor.redact(R"(["-DAPI_KEY=\"s3cr3t\"", "-DVERSION=3"])");
        expect(redactor.redact("clang++ -DSECRET_SALT=abc -c x.cpp") == "clang++ -DSECRET_SALT=<redacted> -c x.cpp");
        expect(redactor.redact("Authorization: Bearer abcdefghijklmnop") == "Authorization: <redacted>");
        expect(redactor.redact("sent Bearer abcdefghijklmnop") == "sent Bearer <redacted>");
        expect(redactor.redact("https://bob:pa55@proxy.example.com:8080/x") == "https://<redacted>@proxy.example.com:8080/x");
        expect(redactor.redact("mail a.b+c@example.org.") == "mail <redacted>.");
        expect(redactor.redact("mcpp@2026.9.24.1 and gcc@16.1.0") == "mcpp@2026.9.24.1 and gcc@16.1.0");
        expect(redactor.redact("git@github.com:owner/repo.git") == "git@github.com:owner/repo.git");
        expect(redactor.redact("PASSWORD=null TOKEN=") == "PASSWORD=null TOKEN=") << "nothing to hide";
        expect(bundle::secret_name("x-api-key"));
        expect(bundle::secret_name("APIKey"));
        expect(bundle::secret_name("client_secret"));
        expect(!bundle::secret_name("semanticTokens"));
        expect(!bundle::secret_name("author"));
        expect(!bundle::secret_name("keyword"));
    };

    "what each rule replaced is counted"_test = [] {
        bundle::Redactor redactor { windows_user() };
        (void)redactor.redact(R"(C:\Users\runneradmin\x ghp_abcdefghijklmnopqrstuvwxyz0123456789 fv-az1234-567 runneradmin)");
        const auto& hits = redactor.hits();
        expect(hits.contains("home-directory") && hits.at("home-directory") == 1);
        expect(hits.contains("secret") && hits.at("secret") == 1);
        expect(hits.contains("host-name") && hits.at("host-name") == 1);
        expect(hits.contains("user-name") && hits.at("user-name") == 1);
    };

    "the residue check finds the identity in the text before redaction, and nothing after"_test = [] {
        for (const auto& identity : { linux_user(), windows_user() }) {
            bundle::Redactor redactor { identity };
            const std::string home { identity.homes.front() };
            std::string backslashed { home };
            std::ranges::replace(backslashed, '/', '\\');
            const std::string raw { std::format("{0}/a {1}\\b file:///{0}/c {2} built on {3} {0}", home, backslashed, identity.users.front(), identity.hosts.front()) };
            const auto before = redactor.residue(raw);
            expect(!before.empty());
            expect(std::ranges::any_of(before, [](const bundle::Residue& residue) { return residue.rule == "home-directory"; }));
            const std::string redacted { redactor.redact(raw) };
            expect(redactor.residue(redacted).empty()) << redacted;
            expect(!contains_folded(redacted, identity.users.front())) << redacted;
        }
        bundle::Redactor redactor { linux_user() };
        expect(redactor.residue("ghp_abcdefghijklmnopqrstuvwxyz0123456789").size() == 1);
        // The check runs every detector redact() does, so what one of them would catch is never shipped unseen ...
        const auto email = redactor.residue("contact: jane.doe@corp.example");
        expect(email.size() == 1u && email.front().rule == "email");
        expect(!redactor.residue(R"({"api_key": "s3cr3t-value"})").empty());
        expect(!redactor.residue("Authorization: Bearer abcdef0123456789").empty());
        // ... and the placeholders redact() leaves are not taken for what they replaced.
        const std::string redacted { redactor.redact(R"({"api_key": "s3cr3t-value", "contact": "jane.doe@corp.example"} Authorization: Bearer abcdef0123456789)") };
        expect(redactor.residue(redacted).empty()) << redacted;
    };

    "a report is redacted as JSON and keeps its structure"_test = [] {
        bundle::Redactor redactor { windows_user() };
        const Json report { { "root", "C:/Users/runneradmin/proj" }, { "count", 3 }, { "log", Json::array({ R"(C:\Users\runneradmin\x)" }) } };
        const Json redacted = redactor.redact_json(report);
        expect(redacted["root"] == "~/proj");
        expect(redacted["count"] == 3);
        expect(redacted["log"][0] == R"(~\x)") << redacted.dump();
    };

    "CRC-32 and deflate round trip"_test = [] {
        expect(bundle::crc32("123456789") == 0xCBF43926U);
        expect(bundle::crc32("") == 0U);
        std::string log;
        for (int i { 0 }; i < 4000; ++i) log += std::format("mcppls 2026-09-26T01:02:{:02}Z [info] clangd (/home/x/proj): V[01:02:03] line {}\n", i % 60, i);
        std::string noise;
        std::uint32_t state { 12345 };
        for (int i { 0 }; i < 70000; ++i) {
            state = state * 1103515245U + 12345U;
            noise += static_cast<char>(state >> 24);
        }
        for (const std::string_view data : { std::string_view {}, std::string_view { "a" }, std::string_view { "abcabcabcabcabcabcabc" },
                                             std::string_view { log }, std::string_view { noise }, std::string_view { std::string(70000, 'z') } }) {
            expect(inflate_through_archive(data) == data) << data.size();
        }
        expect(bundle::deflate(log).size() < log.size() / 3) << "a log compresses";
    };

    "a zip archive holds every entry with its name, size and CRC"_test = [] {
        std::vector<bundle::ZipEntry> entries;
        entries.push_back(bundle::make_entry("manifest.json", R"({"format":1})"));
        entries.push_back(bundle::make_entry("logs/server-1.log", std::string(10000, 'x')));
        entries.push_back(bundle::make_entry("dumps/random.dmp", std::string { "\x01\x02\x03", 3 }));
        expect(entries[1].method == 8);
        expect(entries[2].method == 0) << "what deflate does not shrink is stored";
        const std::string archive { bundle::zip_archive(entries, std::chrono::system_clock::now()) };
        const auto files = unzip(archive);
        expect(files.size() == 3);
        expect(files.at("manifest.json") == R"({"format":1})");
        expect(files.at("logs/server-1.log") == std::string(10000, 'x'));
        expect(files.at("dumps/random.dmp") == std::string { "\x01\x02\x03", 3 });
    };

    // The fixture of the plan's F18 acceptance: a user whose long name is in every path, a bundle
    // written by a process that sees that user as its own, and not one file of it naming them.
    "a bundle of a user whose name is everywhere names them nowhere"_test = [] {
        const auto arguments = mcppls::platform::env::arguments();
        std::string self { arguments.front() };
        if (!base::is_absolute_path(self)) self = base::join_path(fs::current_directory(), self);
        const std::string directory { base::join_path(mcppls::platform::dirs::temp_directory(),
                                                      std::format("mcppls-test-bundle-{}", std::chrono::steady_clock::now().time_since_epoch().count())) };
        const bool windows { mcppls::os::FAMILY == mcppls::os::Family::windows };
        const std::string home { base::join_path(directory, windows ? std::format("Users/{}", LONG_USER) : std::format("home/{}", LONG_USER)) };
        (void)fs::create_directories(home);
        std::vector<std::string> environment;
        for (const auto& variable : mcppls::platform::env::variables()) {
            const std::string name { variable.substr(0, variable.find('=', 1)) };
            static constexpr std::array<std::string_view, 9> REPLACED { "HOME", "USERPROFILE", "HOMEDRIVE", "HOMEPATH", "USER", "USERNAME", "LOGNAME",
                                                                        "MCPPLS_CACHE_DIR", "XDG_CACHE_HOME" };
            if (std::ranges::none_of(REPLACED, [&](std::string_view replaced) { return base::path_key(name, true) == base::path_key(replaced, true); })) {
                environment.push_back(variable);
            }
        }
        environment.push_back(std::format("{}={}", windows ? "USERPROFILE" : "HOME", home));
        environment.push_back(std::format("USER={}", LONG_USER));
        environment.push_back(std::format("USERNAME={}", LONG_USER));
        environment.push_back(std::format("MCPPLS_CACHE_DIR={}", base::join_path(home, ".cache/mcppls")));
        for (const bool excerpts : { true, false }) {
            auto run = mcppls::platform::run(mcppls::platform::SpawnOptions { .program = self, .arguments = { "bundle-child", directory, excerpts ? "excerpts" : "none" },
                                                                             .environment = environment },
                                             mcppls::platform::RunBounds { .hard = std::chrono::seconds { 60 } }, "");
            expect(fatal(run.has_value()));
            expect(fatal(run->exitCode == 0)) << run->output << run->error;
            const Json result = Json::parse(run->output);
            const auto archive = fs::read_file(result.value("path", std::string {}));
            expect(fatal(archive.has_value()));
            expect(archive->size() == result.value("bytes", std::size_t { 0 }));
            expect(archive->size() < bundle::DEFAULT_SIZE_CAP);
            const auto files = unzip(*archive);
            const Json manifest = Json::parse(files.at("manifest.json"));
            // What the manifest says is inside is what is inside, digest for digest.
            expect(manifest["files"].size() + 1 == files.size()) << manifest.dump(2);
            for (const auto& file : manifest["files"]) {
                const std::string name { file.value("path", std::string {}) };
                expect(fatal(files.contains(name))) << name;
                expect(base::sha256_hex(files.at(name)) == file.value("sha256", std::string {})) << name;
                expect(files.at(name).size() == file.value("bytes", std::size_t { 0 })) << name;
            }
            for (const std::string_view expected : { "report.json", "environment.json", "logs/client.log", "logs/server-20260926-010203-ab12.log",
                                                      "logs/server-20260926-010203-ab12.log.1", "incidents/root-1/20260926T010207.000Z-spin/incident.json",
                                                      "incidents/root-1/20260926T010207.000Z-spin/clangd.log", "engine/root-1/compile_commands.json",
                                                      "engine/root-1/plan.json" }) {
                expect(files.contains(std::string { expected })) << expected;
            }
            // Nobody named, in any file: not the user, not the home in any spelling.
            std::string backslashed { home };
            std::ranges::replace(backslashed, '/', '\\');
            for (const auto& [name, content] : files) {
                expect(!contains_folded(content, LONG_USER)) << name << " names the user";
                expect(!contains_folded(content, home) && !contains_folded(content, backslashed)) << name << " names the home directory";
                expect(!content.contains("ghp_abcdefghijklmnopqrstuvwxyz") && !content.contains("s3cr3t-value")) << name << " keeps a secret";
                expect(!content.contains("@example.org")) << name << " keeps an address";
            }
            expect(manifest["redaction"]["rules"].value("home-directory", 0) > 0) << manifest.dump(2);
            expect(manifest["redaction"]["residueCheck"] == "passed");
            const bool excerptsKept { files.at("incidents/root-1/20260926T010207.000Z-spin/incident.json").contains("\"excerpts\"") };
            expect(excerptsKept == excerpts) << "source excerpts only when asked";
            expect(files.at("engine/root-1/compile_commands.json").contains("-DVERSION=3"));
        }
        // Over its cap, a bundle leaves out the oldest log first, says so, and keeps what matters most.
        {
            auto run = mcppls::platform::run(mcppls::platform::SpawnOptions { .program = self, .arguments = { "bundle-child", directory, "cap" }, .environment = environment },
                                             mcppls::platform::RunBounds { .hard = std::chrono::seconds { 60 } }, "");
            expect(fatal(run.has_value() && run->exitCode == 0)) << (run ? run->output + run->error : std::string {});
            const Json result = Json::parse(run->output);
            const auto archive = fs::read_file(result.value("path", std::string {}));
            expect(fatal(archive.has_value()));
            expect(archive->size() <= 512 * 1024) << archive->size();
            const auto files = unzip(*archive);
            const Json manifest = Json::parse(files.at("manifest.json"));
            expect(!files.contains("logs/server-20260925-010203-cd34.log"));
            expect(manifest["omitted"].dump().contains("logs/server-20260925-010203-cd34.log")) << manifest["omitted"].dump();
            for (const std::string_view kept : { "report.json", "environment.json", "incidents/root-1/20260926T010207.000Z-spin/incident.json",
                                                  "logs/server-20260926-010203-ab12.log" }) {
                expect(files.contains(std::string { kept })) << kept;
            }
        }
        fs::remove_all(directory);
    };

    return report();
}
