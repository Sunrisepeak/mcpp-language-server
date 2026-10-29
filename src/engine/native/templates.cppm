// Code templates (S3-6.4): declarations a person writes from nothing, which a semantic engine does
// not offer since nothing declares them yet -- `auto main() -> int { }` and `int main() { }` -- from
// the text alone, at file scope, merged with what the core engine answers as the module-syntax
// keywords are. Not semantic: what was typed and where, and whether the file has the declaration.
export module mcppls.engine.native.templates;

import std;
import nlohmann.json;
import mcppls.base.text;
import mcppls.project.scan;

export namespace mcppls::index {

struct TemplateOptions {
    // The client declared textDocument.completion.completionItem.snippetSupport: an item is a
    // snippet that leaves the cursor in the body; otherwise it is the template as written, plain.
    bool snippets { false };
};

// The template items for `position` in `text`: at file scope (outside any braces, comments,
// literals and preprocessor lines), where the declaration typed so far on the line is a prefix of
// the name a template declares, or its type, blanks and such a prefix, with nothing after the
// cursor. None in a file that declares the name already, and none in a named module unit (`scan`,
// the file's scan, has its module declaration). An empty array when there are none.
nlohmann::json template_completion(std::string_view text, base::Position position, const project::ScanResult* scan,
                                   TemplateOptions options = {});

} // namespace mcppls::index
