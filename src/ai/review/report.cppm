// A review's findings in the forms that carry them further (overall design 7.4 step 7, S5 5): SARIF
// 2.1.0 for code scanning, LSP diagnostics for editors, and a Markdown report for people.
export module mcppls.ai.review.report;

import std;
import nlohmann.json;
import mcppls.spec.query;

export namespace mcppls::ai::review {

// `root` is the directory SARIF's %SRCROOT% names; locations are relative to it.
nlohmann::json to_sarif(std::span<const spec::Finding> findings, std::string_view root, std::string_view base);

// An LSP 3.17 Diagnostic of a finding: the range from its location's line text (UTF-16), evidence
// as related information, the fingerprint as data. `uri_of` turns an S5 file name into a URI.
nlohmann::json to_lsp_diagnostic(const spec::Finding& finding, const std::function<std::string(std::string_view file)>& uri_of);

std::string to_markdown(std::span<const spec::Finding> findings, std::string_view base, const nlohmann::json& summary);

} // namespace mcppls::ai::review
