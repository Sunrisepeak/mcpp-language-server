// S1 section 9: structured SemanticOptions as the consumer uses them. When a
// database carries options they decide the unit's semantics (rule 1), so they
// are written back as the command line of the set's toolchain family and take
// the same translation as a build's own arguments.
export module mcppls.normalize.semantic;

import std;
import mcppls.spec.database;

export namespace mcppls::normalize {

// Rule 2: the set's options merged with the unit's — objects key by key, arrays
// concatenated with the set's elements first, scalars taking the unit's value.
// Empty when neither has options.
std::optional<spec::SemanticOptions> effective_options(const std::optional<spec::SemanticOptions>& set,
                                                       const std::optional<spec::SemanticOptions>& unit);

// The options in the dialect of `family`, driver first and source last, as a build would have written them.
std::vector<std::string> options_arguments(const spec::SemanticOptions& options, spec::Family family, std::string_view driver,
                                           std::string_view source);

} // namespace mcppls::normalize
