// The immutable input used to select an mcpp analysis target. No command is executed here.
export module mcppls.project.target;

import std;
import mcppls.project.compdb;

export namespace mcppls::project {

struct TargetSelection {
    std::string mode { "auto" };
    std::string requestedTarget;
    std::string source { "project" }; // configuration | compile-commands | project
    std::string reason;
    std::string evidencePath;
    bool retryable { false }; // incomplete/unstable CDB snapshot; not a protocol field
    bool operator==(const TargetSelection&) const = default;
};

bool mcpp_target_matches(std::string_view requested, std::string_view effective);
bool equivalent_mcpp_targets(std::string_view left, std::string_view right);
std::string target_selection_key(const TargetSelection& selection);
bool valid_mcpp_target_selector(std::string_view value);
TargetSelection target_from_commands(std::string_view root, std::span<const CompileCommand> commands);
TargetSelection select_mcpp_target(std::string_view root, std::string_view selector = "auto");

} // namespace mcppls::project
