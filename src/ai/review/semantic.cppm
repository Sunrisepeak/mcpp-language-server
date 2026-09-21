// The semantic diff of a unit (overall design 7.4 step 2): what its module declaration, imports and
// exported declarations were before a change and are after it, read by mcppls's own engine from the
// two texts, without building either.
export module mcppls.ai.review.semantic;

import std;
import nlohmann.json;
import mcppls.spec.query;

export namespace mcppls::ai::review {

enum class ExportChangeKind { added, removed, changed };
std::string_view to_string(ExportChangeKind kind);

struct ExportChange {
    ExportChangeKind kind { ExportChangeKind::changed };
    std::string qualifiedName;
    std::string name;
    std::string declarationKind;             // function | class | struct | ... (the extractor's kinds)
    std::string before;                      // the declaration text before
    std::string after;                       // and after
    std::optional<spec::Location> baseLocation;
    std::optional<spec::Location> headLocation;
};

struct ImportChange {
    bool added { true };
    std::string module;                      // "m" or "m:p", as the unit names it fully
    bool exported { false };
    spec::Location location;                 // in the head for an added import, in the base for a removed one
};

struct UnitDiff {
    std::string file;                        // as S5 names it
    std::string baseModule;                  // what the unit provided ("m", "m:p"); empty for none
    std::string headModule;
    std::string baseRole;
    std::string headRole;
    std::vector<ImportChange> imports;
    std::vector<ExportChange> exports;
    // The interface others see changed: exports, re-exports or the module the unit provides.
    bool interface_changed() const;
};

// `base` is nullopt for a new file, `head` for a removed one; `display` names the file in locations.
UnitDiff semantic_diff(std::string display, const std::optional<std::string>& base, const std::optional<std::string>& head);

nlohmann::json to_json(const UnitDiff& diff);

} // namespace mcppls::ai::review
