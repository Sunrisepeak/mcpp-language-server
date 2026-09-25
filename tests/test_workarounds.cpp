import std;
import mcppls.testing;
import mcppls.engine;
import mcppls.engine.clangd;
import mcppls.engine.clangd.workarounds;

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
}
