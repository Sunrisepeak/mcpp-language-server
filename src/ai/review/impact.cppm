// What a change can break (overall design 7.4 step 3): starting from the names a changed interface
// no longer exports, or exports differently, the units that can use them — importers found through
// the module graph — the uses of those names there, the sets that build them, and the tests that
// cover them.
export module mcppls.ai.review.impact;

import std;
import nlohmann.json;
import mcppls.spec.query;
import mcppls.ai.query.view;
import mcppls.ai.review.changes;
import mcppls.ai.review.semantic;

export namespace mcppls::ai::review {

struct NameUses {
    std::string qualifiedName;
    std::string module;                          // the module that exported the name
    std::vector<spec::Location> uses;            // in units other than the declaring one, as they are now
    bool semantic { false };                     // found by the core engine rather than by reading the text
};

struct TestCoverage {
    std::string set;                             // an S1 set of kind test
    std::vector<std::string> files;              // its units that can use the changed interface
    bool changed { false };                      // one of its units is part of the change
};

struct Impact {
    std::vector<std::string> modules;            // modules whose interface changed
    std::vector<std::string> files;              // units that can use them
    std::vector<std::string> unsearched;         // beyond the budget
    std::vector<std::string> sets;               // sets building a changed unit or one that can use it
    std::vector<TestCoverage> tests;
    std::vector<NameUses> names;                 // removed or changed exports and their uses
};

Impact analyze_impact(query::View& view, const ChangeSet& changes, std::span<const UnitDiff> diffs, std::size_t budget, query::Clock::time_point deadline);

// Where `name` is used in `text` as an identifier: not in comments or literals, qualified by the last
// component of `scope` when the text does not open that namespace itself. Lines and columns from 1.
std::vector<spec::Location> identifier_uses(std::string_view text, std::string display, std::string_view name, std::string_view scope);

nlohmann::json to_json(const Impact& impact);

} // namespace mcppls::ai::review
