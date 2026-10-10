import std;
import mcppls.testing;
import mcppls.engine;
import mcppls.engine.clangd;
import mcppls.engine.clangd.workarounds;
import mcppls.normalize.plan;
import mcppls.engine.clangd.process;
import mcppls.engine.payload;
import mcppls.orchestrator.workspace;
import mcppls.cli.options;

namespace cld = mcppls::engine::clangd;

int main() {
    using namespace mcppls::testing;

    "every workaround is registered once, says why, and says when it can go"_test = [] {
        std::set<std::string_view> ids;
        for (const auto& workaround : cld::workarounds()) {
            expect(ids.insert(workaround.id).second) << workaround.id;
            expect(workaround.id.starts_with("WA-CLANGD-")) << workaround.id;
            expect(!workaround.title.empty() && !workaround.upstream.empty() && !workaround.evidence.empty()) << workaround.id;
            expect(!workaround.added.empty() && !workaround.removeWhen.empty()) << workaround.id;
        }
        // Workarounds added from 0.0.4 on carry a canary: the check that fails once the defect is gone.
        const auto* trailingDot = cld::find_workaround(cld::TRAILING_DOT_MODULE_NAME);
        expect(fatal(trailingDot != nullptr));
        expect(!trailingDot->canary.empty());
        expect(cld::find_workaround("WA-CLANGD-999") == nullptr);
    };

    "a workaround applies to its line until the fix, and to every version the suite has not seen"_test = [] {
        expect(cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "23.1.0"));
        expect(!cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "23.1.1"));
        expect(!cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "23.1.12"));
        expect(cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "22.1.8")) << "another line gets every workaround";
        expect(cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "23.10.0")) << "23.10 is not the 23.1 line";
        expect(cld::needs(cld::MSVC_STL_ALIGNED_ALLOCATION, "")) << "an unknown version gets every workaround";
        for (const auto version : { "23.1.0", "23.1.2", "22.1.8", "24.0.0" }) expect(cld::needs(cld::TRAILING_DOT_MODULE_NAME, version)) << version;
        expect(!cld::needs("WA-CLANGD-999", "23.1.0"));
        const auto active = cld::active_workarounds("23.1.1");
        expect(std::ranges::find(active, cld::TRAILING_DOT_MODULE_NAME) != active.end());
        expect(std::ranges::find(active, cld::MSVC_STL_ALIGNED_ALLOCATION) == active.end());
    };

    "the traits are the registry's"_test = [] {
        const auto pinned = cld::traits_for_version("23.1.0");
        expect(pinned.tested && pinned.hangsOnTrailingDotModuleName && pinned.hangsOnUnresolvedImports);
        const auto bundled = cld::traits_for_version("23.1.0-mcppls.0");
        expect(bundled.tested && bundled.kitStdlibVersion == "23.1.0") << "the bundled maintained engine is the tested one, its kit the LLVM base";
        expect(!cld::traits_for_version("23.1.0-mcppls.7").tested) << "another maintained build is not";
        expect(pinned.needsModulePreparation && pinned.needsModuleHints && pinned.msvcStlNeedsNoAlignedAllocation);
        expect(!cld::traits_for_version("23.1.1").msvcStlNeedsNoAlignedAllocation);
        expect(cld::traits_for_version("23.1.1").hangsOnTrailingDotModuleName);
    };

    "every line clangd 23.1 spins on gets a ';' right after its dot"_test = [] {
        // The table of .agents/docs/2026-09-25-import-hang-status-highlight.md §1: each hangs clangd 23.1.0.
        const std::vector<std::pair<std::string_view, std::string_view>> hazards {
            { "import hello.\n", "import hello.;\n" },
            { "import hello.", "import hello.;" },
            { "import hello.\nint x;\n", "import hello.;\nint x;\n" },
            { "import a.b.\n", "import a.b.;\n" },
            { "export import hello.\n", "export import hello.;\n" },
            { "export module a.\n", "export module a.;\n" },
            { "module a.\n", "module a.;\n" },
            { "module;\nexport module m.\n", "module;\nexport module m.;\n" },
            { "import hello. \n", "import hello.; \n" },
            { "\xEF\xBB\xBFimport hello.\n", "\xEF\xBB\xBFimport hello.;\n" },
            { "import hello.\t\n", "import hello.;\t\n" },
            { "import hello.// c\n", "import hello.;// c\n" },
            { "import hello.\n;\n", "import hello.;\n;\n" },
            { "  import hello.\r\n", "  import hello.;\r\n" },
            { "import hello./* c */\n", "import hello.;/* c */\n" },
        };
        for (const auto& [text, expected] : hazards) {
            const auto sanitized = cld::sanitize_module_names(text);
            expect(sanitized.changed()) << text;
            expect(sanitized.text == expected) << text << " became " << sanitized.text;
        }
    };

    "everything else is left as it is"_test = [] {
        for (const std::string_view text : {
                 "import hello.greet;\n", "import hello.greet\n", "import hello;\n", "import hello\n", "import hello.;\n", "import hello. ;\n",
                 "import hello.;// c\n", "import hello:\n", "import :part;\n", "module;\n", "module :private;\n", "import <vector>;\n",
                 "auto x = a.\nb;\n", "// import hello.\n", "/*\nimport hello.\n*/\n", "int import_ = 1; // import x.\n", "important.\n",
                 "std::string s = \"import a.\";\n", "exported module a.\n", "", "\n" }) {
            const auto sanitized = cld::sanitize_module_names(text);
            expect(!sanitized.changed()) << text;
            expect(sanitized.text.empty()) << "nothing is copied when nothing changes: " << text;
        }
    };

    "insertions are recorded in UTF-16 and map positions back"_test = [] {
        const auto sanitized = cld::sanitize_module_names("import std;\nimport hello.\n\nexport module mé.\n");
        expect(fatal(sanitized.insertions.size() == 2u));
        expect(sanitized.insertions[0].line == 1 && sanitized.insertions[0].character == 13);
        expect(sanitized.insertions[1].line == 3 && sanitized.insertions[1].character == 17) << "é is one UTF-16 unit";
        const std::span<const cld::Insertion> insertions { sanitized.insertions };
        // Before and at the insertion nothing moves; after it, one unit back; other lines never move.
        expect(cld::to_original(insertions, { 1, 7 }).character == 7);
        expect(cld::to_original(insertions, { 1, 13 }).character == 13);
        expect(cld::to_original(insertions, { 1, 14 }).character == 13);
        expect(cld::to_original(insertions, { 0, 20 }).character == 20);
        expect(cld::to_original(insertions, { 3, 18 }).character == 17);
    };

    "every workaround says what it takes for granted"_test = [] {
        for (const auto& workaround : cld::workarounds()) expect(!workaround.premise.empty()) << workaround.id;
        const auto pinned = cld::traits_for_version("23.1.0");
        expect(pinned.misplacesDirectiveSemicolon && pinned.readsImportsFromDisk);
        const std::vector<std::string> off { std::string { cld::DIRECTIVE_SEMICOLON_POSITION }, std::string { cld::UNSAVED_IMPORT_NOT_FOUND } };
        const auto disabled = cld::traits_for_version("23.1.0", off);
        expect(!disabled.misplacesDirectiveSemicolon && !disabled.readsImportsFromDisk) << "each can be turned off";
    };

    "only a verified diagnostic range capability retires WA-CLANGD-006"_test = [] {
        const std::vector<std::string> fixed { "module-directive-diagnostic-ranges" };
        const std::vector<std::string> other { "semantic-tokens-range" };
        for (const auto version : { "23.1.0", "23.1.0-mcppls.0", "23.1.1" }) {
            expect(cld::traits_for_version(version).misplacesDirectiveSemicolon) << "the version does not prove the fix";
            expect(cld::traits_for_version(version, {}, other).misplacesDirectiveSemicolon);
            const auto verified = cld::traits_for_version(version, {}, fixed);
            expect(!verified.misplacesDirectiveSemicolon);
            expect(verified.readsImportsFromDisk && verified.hangsOnTrailingDotModuleName) << "other compensations remain active";
        }
    };

    "directive recovery and const-view correctness retire WA-CLANGD-001 and 010 only when declared"_test = [] {
        const auto plain = cld::traits_for_version("23.1.0-mcppls.0");
        expect(plain.hangsOnTrailingDotModuleName && plain.flagsNonConstViewsConst) << "the version does not prove the fixes";
        const std::vector<std::string> recovery { "module-directive-recovery" };
        const auto recovered = cld::traits_for_version("23.1.0-mcppls.0", {}, recovery);
        expect(!recovered.hangsOnTrailingDotModuleName && recovered.flagsNonConstViewsConst) << "each capability retires its own";
        const std::vector<std::string> both { "module-directive-recovery", "const-correctness-views" };
        const auto fixed = cld::traits_for_version("23.1.0-mcppls.0", {}, both);
        expect(!fixed.hangsOnTrailingDotModuleName && !fixed.flagsNonConstViewsConst);
        expect(fixed.misplacesDirectiveSemicolon && fixed.readsImportsFromDisk) << "other compensations remain active";
    };

    "the adapter receives capabilities only from a verified payload identity"_test = [] {
        mcppls::engine::PayloadPaths payload;
        payload.clangdVersion = "23.1.0-mcppls.0";
        const mcppls::orchestrator::SessionOptions options;
        const auto active = [&](bool corrupt) {
            return mcppls::cli::engine_factories(options, payload, corrupt).core()->traits().misplacesDirectiveSemicolon;
        };
        expect(active(false)) << "an older payload of the same fork version keeps the workaround";
        payload.engineIdentity.emplace();
        payload.engineIdentity->features = { "module-directive-diagnostic-ranges" };
        expect(!active(false)) << "the verified capability reaches the adapter";
        expect(active(true)) << "a corrupt payload cannot retire a workaround";
        payload.engineIdentity.reset();
        expect(active(false)) << "a custom engine has no bundled identity";
    };

    "a directive missing its ';' is reported where it is (WA-CLANGD-006)"_test = [] {
        // clangd reports `import hello` + blank line + code at the code (line 2).
        auto moved = cld::directive_missing_semicolon("import std;\nimport hello\n\nauto main() -> int { return 0; }\n", 3);
        expect(fatal(moved.has_value()));
        expect(moved->line == 1 && moved->startCharacter == 11 && moved->endCharacter == 12) << moved->line << ":" << moved->startCharacter;
        // `export module m` with no ';', trailing blanks and a comment after it.
        moved = cld::directive_missing_semicolon("export module mé   // the module\n\n\nexport int f();\n", 3);
        expect(fatal(moved.has_value()));
        expect(moved->line == 0 && moved->endCharacter == 16) << "UTF-16 columns, blanks and the comment left out: " << moved->endCharacter;
        // Only the nearest non-blank line counts, and only a directive without its ';'.
        expect(!cld::directive_missing_semicolon("import hello;\nint x\nint y;\n", 2).has_value());
        expect(!cld::directive_missing_semicolon("import hello;\n\nauto main() {}\n", 2).has_value());
        expect(!cld::directive_missing_semicolon("import hello\n", 0).has_value()) << "on the directive's own line it is right already";
        expect(!cld::directive_missing_semicolon("", 3).has_value());
        expect(cld::directive_missing_semicolon("module a\n// only a comment\nint x;\n", 2).has_value()) << "a comment line is looked past";
    };

    "the module of a 'not found' is read back (WA-CLANGD-007)"_test = [] {
        expect(cld::module_not_found_name("Module 'hello.greet' not found") == std::optional<std::string> { "hello.greet" });
        expect(cld::module_not_found_name("module 'a:part' not found") == std::optional<std::string> { "a:part" });
        expect(!cld::module_not_found_name("Module 'x' not found here").has_value());
        expect(!cld::module_not_found_name("Header 'x' not found").has_value());
        expect(!cld::module_not_found_name("Module '' not found").has_value());
    };

    "a view without a const begin() is not told to be const (WA-CLANGD-010)"_test = [] {
        // What clangd 23.1.0 said on issue #37's code (vulkan-rt) and its reductions, libstdc++ 16 and the kit's libc++.
        for (const std::string_view message : {
                 // libstdc++: `auto v = in | std::views::filter(keep);` then `v | std::views::transform(...)`
                 "Variable 'device_local_heaps' of type 'typename __invoke_result<const std::ranges::views::_Filter &, const "
                 "vk::ArrayWrapper1D<vk::MemoryHeap, 16> &, const (lambda at /p/device.cpp:399:58) &>::type' (aka "
                 "'std::ranges::filter_view<std::ranges::ref_view<const vk::ArrayWrapper1D<vk::MemoryHeap, 16>>, (lambda at "
                 "/p/device.cpp:399:58)>') can be declared 'const' (fix available)",
                 // libc++ prints the bare name after `aka`
                 "Variable 'v' of type 'invoke_result_t<std::ranges::__pipeable<std::__bind_back_t<std::ranges::views::__filter::__fn, "
                 "std::tuple<bool (*)(const H &)>>>, const std::array<H, 4> &>' (aka 'filter_view<ref_view<const std::array<H, 4>>, bool "
                 "(*)(const H &)>') can be declared 'const' (fix available)",
                 // filter_view spelled as the type, with no `aka`
                 "Variable 'v' of type 'std::ranges::filter_view<std::ranges::ref_view<const std::vector<int>>, (lambda at /p/c.cpp:29:35)>' "
                 "can be declared 'const'",
                 // a transform over a filter: its const begin() needs a const-iterable filter
                 "Variable 'v' of type 'typename __invoke_result<const std::ranges::views::_Transform &, std::ranges::filter_view<std::ranges::"
                 "ref_view<const std::array<H, 4>>, bool (*)(const H &)>, unsigned long H::*const &>::type' (aka 'std::ranges::transform_view<"
                 "std::ranges::filter_view<std::ranges::ref_view<const std::array<H, 4>>, bool (*)(const H &)>, unsigned long H::*>') can be "
                 "declared 'const' (fix available)",
                 "Variable 'v' of type 'X' (aka 'std::ranges::drop_while_view<std::ranges::ref_view<const std::vector<int>>, (lambda at "
                 "/p/c.cpp:25:35)>') can be declared 'const' (fix available)",
                 "Variable 'v' of type 'X' (aka 'std::ranges::chunk_by_view<std::ranges::ref_view<const std::vector<int>>, std::ranges::less>') "
                 "can be declared 'const' (fix available)",
                 "variable 'v' of type 'std::ranges::split_view<std::ranges::ref_view<std::string>, std::ranges::single_view<char>>' can be "
                 "declared 'const'",
             }) {
            expect(cld::const_correctness_on_non_const_view(message)) << message;
        }
        for (const std::string_view message : {
                 // a plain variable, and a transform_view over an array: const would compile, so the advice stands
                 "Variable 'n' of type 'int' can be declared 'const' (fix available)",
                 "Variable 'v' of type 'typename __invoke_result<const std::ranges::views::_Transform &, const std::array<H, 4> &, unsigned "
                 "long H::*const &>::type' (aka 'std::ranges::transform_view<std::ranges::ref_view<const std::array<H, 4>>, unsigned long "
                 "H::*>') can be declared 'const' (fix available)",
                 // a view over a ref_view of a filter: ref_view's const begin() reaches the filter as it is
                 "Variable 'v' of type 'X' (aka 'std::ranges::transform_view<std::ranges::ref_view<std::ranges::filter_view<std::ranges::"
                 "ref_view<const std::array<H, 4>>, bool (*)(const H &)>>, unsigned long H::*>') can be declared 'const' (fix available)",
                 // lazy_split_view is const-iterable over a forward range; a filter only inside an argument is no base
                 "Variable 'v' of type 'std::ranges::lazy_split_view<std::ranges::ref_view<const std::string>, std::ranges::single_view<char>>' "
                 "can be declared 'const'",
                 "Variable 'v' of type 'std::vector<std::ranges::filter_view<std::ranges::ref_view<const std::vector<int>>, P>>' can be "
                 "declared 'const'",
                 "Variable 'v' of type 'mine::filter_view<int>' can be declared 'const'",
                 // not this check's message at all
                 "Pointee of variable 'p' of type 'int *' can be declared 'const'",
                 "",
             }) {
            expect(!cld::const_correctness_on_non_const_view(message)) << message;
        }
        expect(cld::traits_for_version("23.1.0").flagsNonConstViewsConst);
        const std::vector<std::string> off { std::string { cld::CONST_CORRECTNESS_VIEWS } };
        expect(!cld::traits_for_version("23.1.0", off).flagsNonConstViewsConst) << "it can be turned off";
    };

    "a project without modules gets clangd without its modules support (WA-CLANGD-009)"_test = [] {
        namespace nz = mcppls::normalize;
        nz::EnginePlan plan;
        plan.entries.push_back(nz::EngineEntry {});
        plan.entries.back().file = "/p/device.cpp";
        expect(!cld::plan_uses_modules(plan)) << "headers and sources only";
        const auto with = [&](auto change) {
            nz::EnginePlan copy { plan };
            change(copy);
            return cld::plan_uses_modules(copy);
        };
        expect(with([](nz::EnginePlan& p) { p.entries.back().imports = { "std" }; })) << "import std;";
        expect(with([](nz::EnginePlan& p) { p.entries.back().provides = "hello"; }));
        expect(with([](nz::EnginePlan& p) { p.entries.back().module = "hello"; })) << "an implementation unit";
        expect(with([](nz::EnginePlan& p) { p.stdUnits = 2; }));
        expect(with([](nz::EnginePlan& p) { p.stubModules = { "missing" }; })) << "an import nothing provides";
        expect(with([](nz::EnginePlan& p) { p.issues.push_back(nz::PlanIssue { .code = "unresolved-module", .module = "missing" }); }));
        expect(cld::traits_for_version("23.1.0").scansModulesOnEveryRequest);
        cld::ProcessConfig config;
        auto arguments = cld::clangd_arguments(config);
        expect(std::ranges::find(arguments, std::string { "--experimental-modules-support" }) != arguments.end()) << "on by default";
        config.modulesSupport = false;
        arguments = cld::clangd_arguments(config);
        expect(std::ranges::find(arguments, std::string { "--experimental-modules-support" }) == arguments.end());
        expect(std::ranges::find(arguments, std::string { "--background-index" }) != arguments.end()) << "the rest stays";
    };
}
