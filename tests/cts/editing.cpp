// What a person does in an editor: typing an import or a body, saving, completion, code actions,
// formatting and tidying, verifying an edit, the watch of files, and the experience of a real project.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture code_actions{"code-actions", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture completion_keywords{"completion-keywords", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture completion_templates{"completion-templates", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture format_explicit{"format-explicit", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture include_cleaner_modules{"include-cleaner-modules", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_autosave_import_typing{"mcpp-autosave-import-typing", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture module_edit_autosave{"module-edit-autosave", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture save_plan{"save-plan", on{"linux-x64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture tidy_const_views{"tidy-const-views", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture typing_autosave{"typing-autosave", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_import{"typing-import", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_import_spin{"typing-import-spin", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture typing_pause{"typing-pause", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}, resource{"machine"}};
Fixture ux_modules_qt{"ux-modules-qt", on{"linux-x64"}, resource{"machine"}};
Fixture ux_modules_windows{"ux-modules-windows", on{"win32-x64"}, resource{"machine"}};
Fixture verify_changes{"verify-changes", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture watch_polling{"watch-polling", on{"linux-x64", "linux-arm64"}, [] { run({.fixture = "watch-polling", .as = client::polling}); }};

}  // namespace
