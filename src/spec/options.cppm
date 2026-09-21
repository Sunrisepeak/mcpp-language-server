// S1 section 9 from the consumer's side: a document's arguments structured into
// SemanticOptions, so that level 3 comes from this library instead of from every
// producer. mcpp emits level 2 and leaves level 3 to the S1 library
// (mcpp-community/mcpp#636).
export module mcppls.spec.options;

import std;
import mcppls.spec.database;

export namespace mcppls::spec {

// Arguments in the dialect of `family`, without the driver and the source, as SemanticOptions:
// the fields S1 section 9 structures; every other argument that affects meaning under
// raw-semantic-arguments, in its order; nothing that does not (outputs, dependency files,
// optimization, debug information, warnings, BMI locations; S1-9-1, S1-9-4).
SemanticOptions structure_arguments(std::span<const std::string> arguments, Family family);

// S1 section 11.1 level 3 from a level 2 document. A set whose toolchain family is known
// and whose producer stated no options gets them from its baseline-arguments, or, without
// those, from the arguments every unit shares; each of its units whose arguments differ gets a
// delta from its local-arguments, or from what it adds to the set's. The options this adds are
// marked derived: they restate the arguments, which stay what a consumer compiles (rule 1
// applies to what a producer stated).
void complete_options(Database& database);

} // namespace mcppls::spec
