module mcppls.config.settings;

import std;
import nlohmann.json;
import mcpplibs.cmdline;
import mcppls.base.text;
import mcppls.platform.env;

namespace mcppls::config::settings {

using Json = nlohmann::json;
namespace cmdline = mcpplibs::cmdline;

namespace {

// A category is grouped and headed by this table, in this order, in both the generated docs and
// `mcppls settings --format markdown`; a row's `category` is one of these keys, never displayed
// directly. Keeping the key and the two headings together is what makes adding a category (rather
// than misspelling an existing one) the only way a row's table silently stops rendering.
struct CategoryHeading {
    std::string_view key;
    std::string_view en;
    std::string_view zh;
};

constexpr std::array<CategoryHeading, 6> CATEGORIES { {
    { "build", "Project and build tools", "项目与构建工具" },
    { "engines", "Engines", "引擎" },
    { "editor", "Editor experience", "编辑器体验" },
    { "diagnostics", "Diagnostics and logging", "诊断与日志" },
    { "ai", "AI review", "AI 评审" },
    { "paths", "Paths", "路径" },
} };

bool is_zh(std::string_view lang) { return lang.starts_with("zh"); }

// The registry itself (0.0.6 plan §9 T1). One row per configurable behaviour; see `Setting` in
// `settings.cppm` for what each field means. Ordered as the generated docs list it: by category in
// `CATEGORIES`' order, each category's own rows in the order below.
const std::vector<Setting>& shipped_registry() {
    static const std::vector<Setting> rows {
        // ---- Project and build tools ------------------------------------------------------
        Setting {
            .key = "buildTool", .kind = Kind::enumeration, .values = { "offline", "online", "off" }, .defaultValue = "offline",
            .commandLine = "--build-tool", .surface = Surface::server, .applies = Applies::reload, .category = "build", .since = "0.0.1",
            .summary = "How the project's build tool may be run. `offline`: run it without the network -- if it then cannot describe "
                       "the build without downloading something, the status says what is missing and offers to run it in your terminal. "
                       "`online`: let it reach the network, with ten minutes instead of one. `off`: never run it; the build system is "
                       "still detected and its own generated files are still read (see `buildDiscovery` for turning that off too).",
            .summaryZh = "项目构建工具的运行方式。`offline`：不联网运行——如果构建工具因此无法在不下载东西的情况下描述构建，状态栏会说明缺什么，"
                         "并提议在终端里运行它。`online`：允许联网，超时时间从一分钟延长到十分钟。`off`：从不运行构建工具；仍会探测构建系统、"
                         "仍读取它已有的产物（要连探测也关掉，见 `buildDiscovery`）。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "toolEnvironment", .kind = Kind::enumeration, .values = { "auto", "editor" }, .defaultValue = "auto",
            .commandLine = "--tool-environment", .surface = Surface::server, .applies = Applies::restart, .category = "build", .since = "0.0.1",
            .summary = "Which environment build tools are started in. `auto` reads your login shell's environment once, in the "
                       "background, on POSIX -- an editor started from a desktop entry or a Dock icon carries none of your shell "
                       "configuration, so without this the build tool it finds may not be the one your terminal finds. On Windows the "
                       "editor's environment already matches the terminal's. `editor` always uses the editor process's environment.",
            .summaryZh = "构建工具在哪个环境中启动。`auto` 会在后台读取一次你登录 shell 的环境（仅限 POSIX 系统）——从桌面项或 Dock 图标启动的"
                         "编辑器不带任何 shell 配置，没有这个选项，它找到的构建工具可能就不是你终端里找到的那个。在 Windows 上，编辑器的环境本就"
                         "和终端一致。`editor` 始终使用编辑器进程自身的环境。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "producerTimeout", .kind = Kind::seconds, .defaultValue = "0", .commandLine = "--producer-timeout",
            .surface = Surface::server, .applies = Applies::reload, .category = "build", .since = "0.0.1",
            .summary = "How long a build tool may take to describe the project. `0`, the default, uses the design's own bound (a "
                       "minute offline, ten minutes once `buildTool` is `online`); set it to watch that bound work, or longer for a "
                       "genuinely slower build.",
            .summaryZh = "构建工具描述项目最多可以花多长时间。默认 `0` 使用设计本身的限制（离线一分钟，`buildTool` 为 `online` 时十分钟）；"
                         "调短可以观察限制是否生效，构建确实慢就调长。",
        },
        Setting {
            .key = "untrusted", .kind = Kind::boolean, .defaultValue = "false", .commandLine = "--untrusted", .surface = Surface::server,
            .applies = Applies::restart, .category = "build", .since = "0.0.1",
            .summary = "Run no build tool and no compiler; an untrusted workspace is also read as though `buildDiscovery` were `off`.",
            .summaryZh = "不运行任何构建工具，也不运行编译器；一个不受信任的工作区也等同于 `buildDiscovery` 为 `off`。",
        },
        Setting {
            .key = "discoverCompilers", .kind = Kind::boolean, .defaultValue = "true", .commandLine = "--no-discover",
            .commandLineNegated = true, .surface = Surface::server, .applies = Applies::reload, .category = "build", .since = "0.0.1",
            .summary = "Look for a compiler on the machine for a source the build description does not cover. Off: such a source "
                       "uses the semantic kit instead.",
            .summaryZh = "为构建描述没有覆盖到的源码在本机查找编译器。关闭后，这类源码改用语义工具包。",
        },
        Setting {
            .key = "buildDiscovery", .kind = Kind::enumeration, .values = { "auto", "off" }, .defaultValue = "auto",
            .commandLine = "--build-discovery", .surface = Surface::server, .applies = Applies::reload, .category = "build",
            .since = "0.0.6",
            .summary = "Whether the project's build system is detected at all (0.0.6 plan §3.7 B-7). `off`: nothing is read or run "
                       "implicitly -- only an explicitly configured `database`, else sources are scanned. `buildTool` still governs "
                       "whether a detected build tool may be *run*; this governs whether it is looked for in the first place.",
            .summaryZh = "是否探测项目的构建系统（0.0.6 计划 §3.7 B-7）。`off`：不隐式读取或执行任何东西——只用明确配置的 `database`，否则"
                         "扫描源码。`buildTool` 管的是探测到的构建工具能不能*执行*；这个开关管的是要不要去探测它。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "buildDiscovery.providers", .kind = Kind::list,
            .values = { "mcpp", "cmake", "xmake", "meson", "compile-commands" },
            .defaultValue = "mcpp,cmake,xmake,meson,compile-commands", .commandLine = "--build-discovery-providers",
            .surface = Surface::server, .applies = Applies::reload, .category = "build", .since = "0.0.6",
            .summary = "Which build system providers `buildDiscovery` may use; leave one out to stop mcppls from detecting it (for "
                       "example, to use only a CMake build directory that already exists and never let xmake run).",
            .summaryZh = "`buildDiscovery` 可以使用哪些构建系统提供者；从中去掉某个提供者即停用它的探测（例如只想用已有的 CMake 构建目录，"
                         "不要 xmake）。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "buildDiscovery.askBeforeDownload", .kind = Kind::boolean, .defaultValue = "true", .surface = Surface::server,
            .applies = Applies::immediately, .category = "build", .since = "0.0.6",
            .summary = "When the build tool needs a download to finish describing the project, a client may offer to fetch it. "
                       "Off: the status says a download is needed, and nothing asks.",
            .summaryZh = "当构建工具需要下载才能完成描述项目时，客户端可以提议去获取它。关闭后，状态栏说明需要下载，但不会再询问。",
            .clientConfigurable = true,
        },
        // ---- Engines ------------------------------------------------------------------------
        Setting {
            .key = "engine", .kind = Kind::enumeration, .values = { "clangd", "none" }, .defaultValue = "clangd",
            .commandLine = "--engine", .surface = Surface::server, .applies = Applies::restart, .category = "engines", .since = "0.0.1",
            .summary = "The core semantic engine. mcppls's own module engine always runs beside it; `none` means module-level "
                       "features only.",
            .summaryZh = "核心引擎。无论如何，mcppls 自己的模块引擎都会运行；`none` 表示只提供模块相关功能。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "compiler", .kind = Kind::string, .defaultValue = "", .commandLine = "--compiler", .surface = Surface::server,
            .applies = Applies::reload, .category = "engines", .since = "0.0.1",
            .summary = "Use this compiler for module semantics instead of what was detected: an absolute path, a name on `PATH`, or "
                       "`kit` to force the bundled semantic kit. Empty means discovered automatically.",
            .summaryZh = "为模块语义使用这个编译器，而不是检测到的那个：可以是绝对路径、`PATH` 上的名字，或 `kit`（强制使用内置的语义工具"
                         "包）。空表示自动检测。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "semanticKit", .kind = Kind::enumeration, .values = { "auto", "off" }, .defaultValue = "auto",
            .commandLine = "--semantic-kit", .surface = Surface::server, .applies = Applies::reload, .category = "engines",
            .since = "0.0.1",
            .summary = "Whether the bundled standard library kit may be used at all: `auto`, when no compiler is found; `off`, "
                       "never (without a compiler, only module-level features remain).",
            .summaryZh = "内置的标准库工具包是否可以被使用：`auto` 在没有找到编译器时使用；`off` 从不使用（没有编译器时只剩模块相关功能）。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "requestTimeout", .kind = Kind::seconds, .defaultValue = "60", .commandLine = "--request-timeout",
            .surface = Surface::server, .applies = Applies::restart, .category = "engines", .since = "0.0.1",
            .summary = "How long an engine request may take before it is answered without the engine. A request a person waits "
                       "for (hover, definition, completion and the like) waits at most 30s in all, including while clangd starts or "
                       "prepares its modules, and is then answered by mcppls's own engine.",
            .summaryZh = "一个引擎请求最多等待多久，超时后不经该引擎就给出答复。用户在等的请求（悬停、跳转、补全等）总共最多等 30 秒，"
                         "clangd 启动或准备模块期间也算在内，之后由 mcppls 自己的引擎答复。",
        },
        Setting {
            .key = "MCPPLS_ENGINE_ARGUMENTS", .kind = Kind::string, .defaultValue = "", .surface = Surface::environment,
            .applies = Applies::restart, .category = "engines", .since = "0.0.1",
            .summary = "Extra arguments appended to clangd's own command line, for troubleshooting "
                       "(e.g. `-j=8 --background-index-priority=background`).",
            .summaryZh = "追加到 clangd 自身命令行末尾的额外参数，用于排查问题（例如 `-j=8 --background-index-priority=background`）。",
        },
        // ---- Editor experience ----------------------------------------------------------------
        Setting {
            .key = "semanticTokens.modules", .kind = Kind::boolean, .defaultValue = "true", .surface = Surface::server,
            .applies = Applies::restart, .category = "editor", .since = "0.0.4",
            .summary = "Color `import`, `module`, `export` and module names from the server's semantic tokens. Off: only the "
                       "grammar's colors.",
            .summaryZh = "用服务端的语义 token 给 `import`、`module`、`export` 和模块名上色。关闭后只用语法文件的颜色。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "semanticTokens.moduleType", .kind = Kind::boolean, .defaultValue = "false", .surface = Surface::server,
            .applies = Applies::restart, .category = "editor", .since = "0.0.4",
            .summary = "A client declares it knows the custom `module` semantic token type and the `partition` modifier (design "
                       "2026-09-25 K/§7); off is every client but this one, since none else advertises it. Not a package.json "
                       "setting: VS Code's own extension always declares it, fixed, because it contributes that token type itself.",
            .summaryZh = "客户端声明自己认得自定义的 `module` 语义 token 类型和 `partition` 修饰符（设计 2026-09-25 K/§7）；除本仓库的 "
                         "VS Code 扩展外都关闭，因为没有别的客户端会声明它。不是 package.json 里的设置：VS Code 扩展自己贡献了这个 token "
                         "类型，因此固定声明为开。",
        },
        Setting {
            .key = "completion.triggerOnSpace", .kind = Kind::boolean, .defaultValue = "true", .surface = Surface::server,
            .applies = Applies::restart, .category = "editor", .since = "0.0.5",
            .summary = "Show the module list as soon as a space is typed after `import` or `export import`. A space anywhere else "
                       "never reaches the server. A client that says nothing gets this only when it identifies itself as VS Code or "
                       "a fork of it; every other client opts in with `initializationOptions.completion.triggerOnSpace: true`.",
            .summaryZh = "在 `import` 或 `export import` 后输入空格时立即弹出模块列表；其他位置的空格不会发给服务端。什么都不说的客户端"
                         "只有在自证是 VS Code 或其分支时才会得到这个行为；其他客户端需要用 "
                         "`initializationOptions.completion.triggerOnSpace: true` 主动开启。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "index.primeImplementationUnits", .kind = Kind::enumeration, .values = { "auto", "off" }, .defaultValue = "auto",
            .commandLine = "--prime-implementation-units", .surface = Surface::server, .applies = Applies::immediately,
            .category = "editor", .since = "0.0.6",
            .summary = "mcppls opens a module's implementation units in clangd in the background (0.0.6 plan §2.6, §9 T5), so "
                       "go-to-definition reaches a definition that only an implementation unit has. `off`: only what a client opens "
                       "itself is ever indexed for this.",
            .summaryZh = "mcppls 在后台把一个模块的实现单元打开给 clangd（0.0.6 计划 §2.6、§9 T5），这样跳到定义能到达只存在于实现单元里"
                         "的定义。`off`：只有客户端自己打开的文件才会被索引。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "detectConflicts", .kind = Kind::boolean, .defaultValue = "true", .surface = Surface::client,
            .applies = Applies::immediately, .category = "editor", .since = "0.0.1",
            .summary = "Offer once to turn off another C++ extension's language features in this workspace, and say so when one "
                       "becomes active later. VS Code only: no other client arbitrates between language servers.",
            .summaryZh = "在此工作区中提议关闭另一个 C++ 扩展的语言功能（只提议一次），之后又有冲突扩展启用时会提示。仅限 VS Code：其他"
                         "客户端不会在多个语言服务端之间做取舍。",
            .clientConfigurable = true,
        },
        // ---- Diagnostics and logging ----------------------------------------------------------
        Setting {
            .key = "logLevel", .kind = Kind::enumeration, .values = { "debug", "info", "warning", "error" }, .defaultValue = "info",
            .commandLine = "--log-level", .surface = Surface::server, .applies = Applies::restart, .category = "diagnostics",
            .since = "0.0.1",
            .summary = "The server's own log level.",
            .summaryZh = "服务端自身的日志级别。",
        },
        Setting {
            .key = "disableWorkaround", .kind = Kind::list, .defaultValue = "", .commandLine = "--disable-workaround",
            .commandLineRepeatable = true, .surface = Surface::server, .applies = Applies::restart, .category = "diagnostics",
            .since = "0.0.4",
            .summary = "Turn off a registered clangd workaround (`WA-CLANGD-<n>`, see the SKILL.md upstream-defects register), to "
                       "see whether it is still needed; repeatable. `mcppls report` lists every registered workaround under "
                       "`engines[].details.workarounds`.",
            .summaryZh = "关掉一个针对 clangd 缺陷登记的规避措施（`WA-CLANGD-<n>`，见 SKILL.md 的上游缺陷登记），用来确认它是否还有"
                         "必要；可重复。`mcppls report` 在 `engines[].details.workarounds` 下列出所有登记过的规避措施。",
        },
        Setting {
            .key = "trace.server", .kind = Kind::enumeration, .values = { "off", "messages", "verbose" }, .defaultValue = "off",
            .surface = Surface::client, .applies = Applies::immediately, .category = "diagnostics", .since = "0.0.1",
            .summary = "Log the LSP traffic to the C++ Modules output channel (at Trace level); `verbose` adds the server's debug "
                       "log (at Debug level, by also passing `--log-level debug`). Set the channel's own log level to see them.",
            .summaryZh = "把 LSP 通信记录到 C++ Modules 输出通道（Trace 级别）；`verbose` 还会打开服务端的 debug 日志（通过附加 "
                         "`--log-level debug`，Debug 级别）。要看到它们，需把该输出通道的日志级别调到对应级别。",
            .clientConfigurable = true,
        },
        Setting {
            .key = "MCPPLS_LOG_LEVEL", .kind = Kind::enumeration, .values = { "debug", "info", "warning", "error" }, .defaultValue = "",
            .surface = Surface::environment, .applies = Applies::restart, .category = "diagnostics", .since = "0.0.1",
            .summary = "Overrides the log level the VS Code extension starts the server with, ahead of `trace.server`.",
            .summaryZh = "覆盖 VS Code 扩展启动服务端时使用的日志级别，优先于 `trace.server`。",
        },
        // ---- AI review --------------------------------------------------------------------
        Setting {
            .key = "ai.enabled", .kind = Kind::boolean, .defaultValue = "false", .surface = Surface::client,
            .applies = Applies::immediately, .category = "ai", .since = "0.0.1",
            .summary = "Show the AI-era features: Review Changes reviews the workspace's changes against `HEAD` with mcppls's "
                       "rules and shows the findings, with their evidence, as problems. Nothing is sent to a model unless this is "
                       "on and a model source is separately configured.",
            .summaryZh = "显示 AI 时代的功能：Review Changes 用 mcppls 的规则对照 `HEAD` 审查工作区的变更，并把发现连同证据以问题的形式"
                         "展示。除非这里开启并且另行配置了模型来源，否则不会向任何模型发送内容。",
            .clientConfigurable = true,
        },
        // ---- Paths --------------------------------------------------------------------------
        Setting {
            .key = "database", .kind = Kind::path, .defaultValue = "", .commandLine = "--database", .surface = Surface::server,
            .applies = Applies::reload, .category = "paths", .since = "0.0.1",
            .summary = "A workspace's own S1 build database, relative to its root, used instead of detecting one.",
            .summaryZh = "工作区自己的 S1 构建数据库，相对于其根目录，用它代替探测。",
        },
        Setting {
            .key = "mcpp", .kind = Kind::path, .defaultValue = "", .commandLine = "--mcpp", .surface = Surface::server,
            .applies = Applies::reload, .category = "paths", .since = "0.0.1",
            .summary = "The `mcpp` executable for mcpp projects; empty means found on `PATH`.",
            .summaryZh = "mcpp 项目所用的 `mcpp` 可执行文件；空表示在 `PATH` 上查找。",
        },
        Setting {
            .key = "payload", .kind = Kind::path, .defaultValue = "", .commandLine = "--payload", .surface = Surface::server,
            .applies = Applies::restart, .category = "paths", .since = "0.0.1",
            .summary = "Payload directory with clangd and the semantic kit; overridden per-file by `clangd` and `kit` below.",
            .summaryZh = "包含 clangd 和语义工具包的 payload 目录；下面的 `clangd` 和 `kit` 可以分别覆盖其中一项。",
        },
        Setting {
            .key = "clangd", .kind = Kind::path, .defaultValue = "", .commandLine = "--clangd", .surface = Surface::server,
            .applies = Applies::restart, .category = "paths", .since = "0.0.1",
            .summary = "clangd executable, overriding the one the payload carries.",
            .summaryZh = "clangd 可执行文件，覆盖 payload 自带的那一份。",
        },
        Setting {
            .key = "kit", .kind = Kind::path, .defaultValue = "", .commandLine = "--kit", .surface = Surface::server,
            .applies = Applies::restart, .category = "paths", .since = "0.0.1",
            .summary = "Semantic kit directory, overriding the one the payload carries.",
            .summaryZh = "语义工具包目录，覆盖 payload 自带的那一份。",
        },
        Setting {
            .key = "MCPPLS_CACHE_DIR", .kind = Kind::path, .defaultValue = "", .surface = Surface::environment,
            .applies = Applies::restart, .category = "paths", .since = "0.0.1",
            .summary = "Overrides the whole cache directory mcppls otherwise picks under the user's cache home (workspace models, "
                       "toolchain probes, logs, diagnostic bundles).",
            .summaryZh = "覆盖 mcppls 原本在用户缓存目录下选定的整个缓存目录（工作区模型、工具链探测结果、日志、诊断包）。",
        },
    };
    return rows;
}

// A value's string form as read out of `object`, per the row's `Kind` (T1: `initializationOptions`
// and `didChangeConfiguration` may write a boolean, a number, a string or an array of strings,
// depending on the row). Null when `object`'s JSON type does not fit the row's kind at all.
std::optional<std::string> json_to_text(const Setting& row, const Json& object) {
    switch (row.kind) {
    case Kind::boolean:
        return object.is_boolean() ? std::optional { std::string { object.get<bool>() ? "true" : "false" } } : std::nullopt;
    case Kind::seconds:
        if (object.is_number_integer()) return std::to_string(object.get<long long>());
        if (object.is_string()) return object.get<std::string>();
        return std::nullopt;
    case Kind::list: {
        if (object.is_string()) return object.get<std::string>();
        if (!object.is_array()) return std::nullopt;
        std::vector<std::string> parts;
        for (const auto& item : object) {
            if (!item.is_string()) return std::nullopt;
            parts.push_back(item.get<std::string>());
        }
        return base::join(parts, ",");
    }
    case Kind::enumeration:
    case Kind::string:
    case Kind::path:
        return object.is_string() ? std::optional { object.get<std::string>() } : std::nullopt;
    }
    return std::nullopt;
}

// Validates `text` (already in the row's own string form) against its `Kind`'s vocabulary,
// returning the canonical stored text, or null with a `Problem` appended to `problems` when it is
// outside that vocabulary -- the caller then keeps the row at whatever it already was (T1: an
// unknown value never silently takes effect).
std::optional<std::string> validate(const Setting& row, std::string_view text, std::vector<Problem>& problems) {
    switch (row.kind) {
    case Kind::boolean:
        if (text == "true" || text == "false") return std::string { text };
        problems.push_back({ row.key, std::format("{} is not true or false; keeping the default", text) });
        return std::nullopt;
    case Kind::enumeration:
        if (std::ranges::find(row.values, text) != row.values.end()) return std::string { text };
        problems.push_back(
            { row.key, std::format("{} is not one of {}; keeping the default", text, base::join(row.values, ", ")) });
        return std::nullopt;
    case Kind::seconds: {
        // Full consumption, no sign: `stoll` alone would silently accept "10 minutes please".
        if (!text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
            try {
                return std::to_string(std::stoll(std::string { text }));
            } catch (...) {
            }
        }
        problems.push_back({ row.key, std::format("{} is not a non-negative number of seconds; keeping the default", text) });
        return std::nullopt;
    }
    case Kind::list: {
        std::vector<std::string> members;
        for (auto piece : base::split(text, ',')) {
            const auto trimmed = base::trim(piece);
            if (trimmed.empty()) continue;
            if (!row.values.empty() && std::ranges::find(row.values, trimmed) == row.values.end()) {
                problems.push_back({ row.key,
                                     std::format("{} is not one of {}; keeping the default", trimmed, base::join(row.values, ", ")) });
                return std::nullopt;
            }
            members.emplace_back(trimmed);
        }
        return base::join(members, ",");
    }
    case Kind::string:
    case Kind::path:
        return std::string { text };
    }
    return std::nullopt;
}

// An object's own key `dottedKey` when it has one literally (covers a plain key and the dotted-key
// form, `{"semanticTokens.modules": false}`), else the same path walked as nested objects
// (`{"semanticTokens": {"modules": false}}`) -- T1 accepts either from a client.
const Json* find_dotted_or_nested(const Json& root, std::string_view dottedKey) {
    if (!root.is_object()) return nullptr;
    if (auto it = root.find(std::string { dottedKey }); it != root.end()) return &*it;
    const Json* cursor { &root };
    std::size_t start { 0 };
    while (true) {
        const auto dot = dottedKey.find('.', start);
        const std::string_view segment { dottedKey.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start) };
        if (!cursor->is_object()) return nullptr;
        auto it = cursor->find(std::string { segment });
        if (it == cursor->end()) return nullptr;
        if (dot == std::string_view::npos) return &*it;
        cursor = &*it;
        start = dot + 1;
    }
}

// `object`, or its nested `mcppls` object when it has one: both `initializationOptions` and
// `didChangeConfiguration.settings` may or may not carry that wrapper (T1).
const Json& unwrap_mcppls(const Json& object) {
    if (object.is_object()) {
        if (auto it = object.find("mcppls"); it != object.end() && it->is_object()) return *it;
    }
    return object;
}

const Json* find_setting_json(const Json& scope, const Setting& row) {
    if (const Json* found = find_dotted_or_nested(scope, row.key)) return found;
    for (const auto& alias : row.aliases) {
        if (const Json* found = find_dotted_or_nested(scope, alias)) return found;
    }
    return nullptr;
}

Json typed_json(const Setting& row, const std::string& text) {
    switch (row.kind) {
    case Kind::boolean:
        return text == "true";
    case Kind::seconds:
        try {
            return std::stoll(text);
        } catch (...) {
            return text;
        }
    case Kind::list: {
        Json array = Json::array();
        for (auto piece : base::split(text, ',')) {
            const auto trimmed = base::trim(piece);
            if (!trimmed.empty()) array.push_back(std::string { trimmed });
        }
        return array;
    }
    case Kind::enumeration:
    case Kind::string:
    case Kind::path:
        return text;
    }
    return text;
}

} // namespace

std::string_view to_string(Kind kind) {
    switch (kind) {
    case Kind::boolean: return "boolean";
    case Kind::enumeration: return "enumeration";
    case Kind::string: return "string";
    case Kind::path: return "path";
    case Kind::seconds: return "seconds";
    case Kind::list: return "list";
    }
    return "?";
}

std::string_view to_string(Surface surface) {
    switch (surface) {
    case Surface::server: return "server";
    case Surface::client: return "client";
    case Surface::environment: return "environment";
    }
    return "?";
}

std::string_view to_string(Applies applies) {
    switch (applies) {
    case Applies::restart: return "restart";
    case Applies::reload: return "reload";
    case Applies::immediately: return "immediately";
    }
    return "?";
}

std::string_view to_string(Origin origin) {
    switch (origin) {
    case Origin::defaulted: return "default";
    case Origin::environment: return "environment";
    case Origin::commandLine: return "command-line";
    case Origin::client: return "client";
    case Origin::clientUpdated: return "client-updated";
    }
    return "?";
}

std::span<const Setting> registry() { return shipped_registry(); }

const Setting* find(std::span<const Setting> rows, std::string_view key) {
    for (const auto& row : rows) {
        if (row.key == key) return &row;
        if (std::ranges::find(row.aliases, key) != row.aliases.end()) return &row;
    }
    return nullptr;
}

Settings::Settings(std::span<const Setting> rows) : rows_(rows) {
    for (const auto& row : rows_) {
        if (row.surface == Surface::environment) {
            if (auto value = platform::env::get(row.key)) {
                values_[row.key] = { *value, Origin::environment };
                continue;
            }
        }
        values_[row.key] = { row.defaultValue, Origin::defaulted };
    }
}

std::span<const Setting> Settings::rows() const { return rows_; }

const Value& Settings::value(std::string_view key) const {
    static const Value fallback {};
    const auto it = values_.find(key);
    return it != values_.end() ? it->second : fallback;
}

std::string Settings::string_value(std::string_view key) const { return value(key).text; }

bool Settings::bool_value(std::string_view key) const { return value(key).text == "true"; }

std::chrono::seconds Settings::seconds_value(std::string_view key) const {
    try {
        return std::chrono::seconds { std::stoll(value(key).text) };
    } catch (...) {
        return std::chrono::seconds { 0 };
    }
}

std::vector<std::string> Settings::list_value(std::string_view key) const {
    std::vector<std::string> members;
    for (auto piece : base::split(value(key).text, ',')) {
        const auto trimmed = base::trim(piece);
        if (!trimmed.empty()) members.emplace_back(trimmed);
    }
    return members;
}

Origin Settings::origin(std::string_view key) const { return value(key).origin; }

const std::vector<Problem>& Settings::problems() const { return problems_; }

void Settings::apply_command_line(const cmdline::ParsedArgs& args) {
    for (const auto& row : rows_) {
        if (row.surface == Surface::environment || row.commandLine.empty()) continue;
        // Every command-line spelling here is "--name"; cmdline itself is asked about "name".
        const std::string flag { row.commandLine.substr(2) };
        if (row.kind == Kind::boolean) {
            if (!args.is_flag_set(flag)) continue;
            values_[row.key] = { row.commandLineNegated ? "false" : "true", Origin::commandLine };
            continue;
        }
        if (row.kind == Kind::list && row.commandLineRepeatable) {
            const auto given = args.option_or_empty(flag).values;
            if (given.empty()) continue;
            if (auto validated = validate(row, base::join(given, ","), problems_)) {
                values_[row.key] = { *validated, Origin::commandLine };
            }
            continue;
        }
        if (auto raw = args.value(flag)) {
            if (auto validated = validate(row, *raw, problems_)) values_[row.key] = { *validated, Origin::commandLine };
        }
    }
}

void Settings::apply_initialization_options(const Json& initializationOptionsOrParams) {
    const Json* init { &initializationOptionsOrParams };
    if (initializationOptionsOrParams.is_object()) {
        if (auto it = initializationOptionsOrParams.find("initializationOptions");
            it != initializationOptionsOrParams.end() && it->is_object()) {
            init = &*it;
        }
    }
    if (!init->is_object()) return;
    const Json& scope { unwrap_mcppls(*init) };
    for (const auto& row : rows_) {
        if (row.surface == Surface::environment) continue;
        if (values_[row.key].origin == Origin::commandLine) continue;
        const Json* found { find_setting_json(scope, row) };
        if (found == nullptr) continue;
        auto text { json_to_text(row, *found) };
        if (!text) {
            problems_.push_back({ row.key, std::format("initializationOptions carries {} as the wrong kind of value; keeping {}",
                                                        row.key, values_[row.key].text) });
            continue;
        }
        if (auto validated = validate(row, *text, problems_)) values_[row.key] = { *validated, Origin::client };
    }
}

ChangeResult Settings::apply_configuration_change(const Json& params) {
    ChangeResult result;
    const Json* settingsObject { &params };
    if (params.is_object()) {
        if (auto it = params.find("settings"); it != params.end()) settingsObject = &*it;
    }
    if (!settingsObject->is_object()) return result;
    const Json& scope { unwrap_mcppls(*settingsObject) };
    for (const auto& row : rows_) {
        if (row.surface == Surface::environment) continue;
        auto it = values_.find(row.key);
        if (it == values_.end() || it->second.origin == Origin::commandLine) continue;
        const Json* found { find_setting_json(scope, row) };
        if (found == nullptr) continue;
        auto text { json_to_text(row, *found) };
        if (!text) {
            problems_.push_back(
                { row.key, std::format("didChangeConfiguration carries {} as the wrong kind of value; keeping {}", row.key, it->second.text) });
            continue;
        }
        auto validated { validate(row, *text, problems_) };
        const std::string newValue { validated.value_or(row.defaultValue) };
        const bool changed { newValue != it->second.text };
        it->second = { newValue, Origin::clientUpdated };
        if (!changed) continue;
        result.changedKeys.push_back(row.key);
        if (row.applies == Applies::restart) result.restartKeys.push_back(row.key);
        else if (row.applies == Applies::reload) result.reloadKeys.push_back(row.key);
    }
    return result;
}

Json Settings::to_json() const {
    Json result = Json::object();
    for (const auto& row : rows_) {
        const auto& current = value(row.key);
        result[row.key] = Json { { "value", typed_json(row, current.text) }, { "origin", std::string { to_string(current.origin) } } };
    }
    Json problems = Json::array();
    for (const auto& problem : problems_) problems.push_back(Json { { "key", problem.key }, { "message", problem.message } });
    result["problems"] = std::move(problems);
    return result;
}

namespace {

std::string setting_cell(const Setting& row) {
    return row.surface == Surface::environment ? std::format("`{}`", row.key) : std::format("`mcppls.{}`", row.key);
}

std::string backticked_join(std::span<const std::string> values, std::string_view separator) {
    std::vector<std::string> quoted;
    quoted.reserve(values.size());
    for (const auto& value : values) quoted.push_back(std::format("`{}`", value));
    return base::join(quoted, separator);
}

std::string values_cell(const Setting& row, bool zh) {
    switch (row.kind) {
    case Kind::boolean: return "`true`, `false`";
    case Kind::enumeration: return backticked_join(row.values, ", ");
    case Kind::list:
        if (row.values.empty()) return zh ? "`WA-CLANGD-<n>`（可重复）" : "`WA-CLANGD-<n>` (repeatable)";
        return backticked_join(row.values, ", ") + (row.commandLineRepeatable ? "" : (zh ? "（逗号分隔）" : " (comma-separated)"));
    case Kind::seconds: return zh ? "非负整数（秒）" : "a non-negative number of seconds";
    case Kind::string: return zh ? "字符串" : "a string";
    case Kind::path: return zh ? "路径" : "a path";
    }
    return "?";
}

std::string default_cell(const Setting& row, bool zh) {
    switch (row.kind) {
    case Kind::boolean:
    case Kind::seconds: return std::format("`{}`", row.defaultValue);
    case Kind::enumeration:
    case Kind::string:
    case Kind::path: return row.defaultValue.empty() ? (zh ? "（空）" : "*(empty)*") : std::format("`{}`", row.defaultValue);
    case Kind::list: {
        if (row.defaultValue.empty()) return zh ? "（无）" : "*(none)*";
        std::vector<std::string> members;
        for (auto piece : base::split(row.defaultValue, ',')) members.emplace_back(piece);
        return backticked_join(members, ", ");
    }
    }
    return "?";
}

std::string command_cell(const Setting& row, bool zh) {
    if (row.commandLine.empty()) return "—";
    std::string cell { std::format("`{}`", row.commandLine) };
    if (row.kind == Kind::list && row.commandLineRepeatable) cell += zh ? "（可重复）" : " (repeatable)";
    return cell;
}

std::string applies_cell(Applies applies, bool zh) {
    if (!zh) return std::string { to_string(applies) };
    switch (applies) {
    case Applies::restart: return "重启";
    case Applies::reload: return "重新加载模型";
    case Applies::immediately: return "立即生效";
    }
    return "?";
}

} // namespace

std::string to_markdown(std::span<const Setting> rows, std::string_view lang) {
    const bool zh { is_zh(lang) };
    const std::string_view headSetting { zh ? "设置" : "Setting" };
    const std::string_view headValues { zh ? "取值" : "Values" };
    const std::string_view headDefault { zh ? "默认值" : "Default" };
    const std::string_view headCommand { zh ? "命令行" : "Command line" };
    const std::string_view headApplies { zh ? "生效方式" : "Applies" };
    const std::string_view headWhat { zh ? "作用" : "What it does" };

    std::vector<std::string> blocks;
    for (const auto& info : CATEGORIES) {
        std::vector<const Setting*> members;
        for (const auto& row : rows) {
            if (row.category == info.key) members.push_back(&row);
        }
        if (members.empty()) continue;
        std::string block { std::format("### {}\n\n", zh ? info.zh : info.en) };
        block += std::format("| {} | {} | {} | {} | {} | {} |\n", headSetting, headValues, headDefault, headCommand, headApplies, headWhat);
        block += "|---|---|---|---|---|---|\n";
        for (const auto* row : members) {
            block += std::format("| {} | {} | {} | {} | {} | {} |\n", setting_cell(*row), values_cell(*row, zh), default_cell(*row, zh),
                                 command_cell(*row, zh), applies_cell(row->applies, zh), zh ? row->summaryZh : row->summary);
        }
        block.pop_back();   // the loop above leaves one trailing '\n'; blocks are joined by "\n\n" instead
        blocks.push_back(std::move(block));
    }
    return base::join(blocks, "\n\n");
}

Json registry_to_json(std::span<const Setting> rows) {
    Json array = Json::array();
    for (const auto& row : rows) {
        array.push_back(Json {
            { "key", row.key },
            { "kind", std::string { to_string(row.kind) } },
            { "values", row.values },
            { "default", row.defaultValue },
            { "commandLine", row.commandLine },
            { "commandLineNegated", row.commandLineNegated },
            { "commandLineRepeatable", row.commandLineRepeatable },
            { "surface", std::string { to_string(row.surface) } },
            { "applies", std::string { to_string(row.applies) } },
            { "category", row.category },
            { "since", row.since },
            { "summary", row.summary },
            { "summaryZh", row.summaryZh },
            { "aliases", row.aliases },
            { "clientConfigurable", row.clientConfigurable },
        });
    }
    return array;
}

} // namespace mcppls::config::settings
