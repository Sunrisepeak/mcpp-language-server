#!/usr/bin/env python3
"""Validates the specifications' machine-readable parts (specs/README.md: "CI validates every example against its schema").

    python3 specs/tools/validate.py            # from the repository root; needs the jsonschema package
    python3 specs/tools/validate.py ENVELOPE...  # also envelopes a real `mcpp emit build-database --format json` printed

Checks: every JSON file parses and every schema is valid draft 2020-12; examples, JSON blocks in the text and the
simulated producer data of conformance fixtures validate; semantic rules a schema cannot express; negative cases the
schemas must reject; relative links resolve; every requirement of the specifications has an identifier and evidence in
conformance/traceability.json (usable plan W10.2). Exits non-zero on any failure.
"""
import copy, json, pathlib, re, sys
from jsonschema import Draft202012Validator

root = pathlib.Path(__file__).resolve().parent.parent   # docs/specs
repository = root.parents[1]                            # docs/specs -> docs -> the repository
failures = 0
passed = []   # labels of passing checks: evidence the traceability file may name

def check(label, ok, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'}  {label}{('  ' + detail) if detail and not ok else ''}")
    if ok:
        passed.append(label)
    else:
        failures += 1

def load(path):
    return json.loads(path.read_text(encoding="utf-8"))

# 1. every JSON file parses; every schema is a valid draft 2020-12 schema
schemas = {}
for path in sorted(root.rglob("*.json")):
    try:
        doc = load(path)
        check(f"parse {path.relative_to(root)}", True)
    except Exception as e:
        check(f"parse {path.relative_to(root)}", False, str(e))
        continue
    if path.parent.name == "schema":
        try:
            Draft202012Validator.check_schema(doc)
            check(f"schema is valid 2020-12: {path.name}", True)
        except Exception as e:
            check(f"schema is valid 2020-12: {path.name}", False, str(e))
        schemas[path.name] = Draft202012Validator(doc)

s1, s2, s4 = (schemas["s1-build-database.schema.json"], schemas["s2-discovery.schema.json"],
              schemas["s4-kit.schema.json"])

def validate(label, validator, doc, expect_valid=True):
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.path))
    ok = (not errors) if expect_valid else bool(errors)
    detail = "; ".join(f"{list(e.path)}: {e.message}" for e in errors[:3]) if expect_valid else "accepted an invalid document"
    check(label, ok, detail)

ex = root / "examples"
# 2. examples validate
for name in ["s1-level3-gcc.json", "s1-level2-clang-two-sets.json"]:
    validate(f"S1 example validates: {name}", s1, load(ex / name))
validate("S2 request validates", s2, load(ex / "s2-request.json"))
envelope = load(ex / "s2-envelope.json")
validate("S2 single-document envelope validates", s2, envelope)
validate("S2 envelope carries a valid S1 database", s1, envelope["data"]["database"])
lines = [l for l in (ex / "s2-messages.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
for i, line in enumerate(lines):
    validate(f"S2 message line {i + 1} validates", s2, json.loads(line))
check("S2 stream ends with a terminal message", json.loads(lines[-1])["kind"] in ("finished", "error"))
finished = json.loads(lines[-1])
check("S2 example finished message names its profile-version", "profile-version" in finished)
check("S2 example finished message watches the build description and the module sources",
      {"mcpp.toml", "mcpp.lock"} <= set(finished["watch"]) and any("cppm" in w for w in finished["watch"]))
check("S2 example stream reports progress before finishing", any(json.loads(l)["kind"] == "progress" for l in lines[:-1]))
check("S2 example envelope carries inputs-fingerprint", "inputs-fingerprint" in envelope["data"])
check("S2 example envelope of a read-only command declares read-project only", envelope["effects"] == ["read-project"])
for name in ["s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json"]:
    validate(f"S4 example validates: {name}", s4, load(ex / name))

# 3. semantic checks the schema cannot express (S1 sections 7, 8.2, 10; S4 rule 4)
IMPORTABLE = {"module-interface", "module-partition-interface", "module-partition-implementation"}
def s1_semantics(name, doc):
    tool_ids = set(doc["ide"]["toolchains"])
    set_names = [s["name"] for s in doc["sets"]]
    check(f"S1 {name}: set names unique", len(set_names) == len(set(set_names)))
    for s in doc["sets"]:
        check(f"S1 {name}: {s['name']} toolchain id exists", s["ide"]["toolchain"] in tool_ids)
        check(f"S1 {name}: {s['name']} visible sets exist", all(v in set_names for v in s["visible-sets"]))
        for tu in s["translation-units"]:
            role = tu["ide"]["role"]
            provides = tu.get("provides", {})
            check(f"S1 {name}: {tu['source']} provides iff importable role",
                  bool(provides) == (role in IMPORTABLE))
            for module in provides:
                partition = ":" in module
                check(f"S1 {name}: {tu['source']} partition role matches name",
                      partition == (role in {"module-partition-interface", "module-partition-implementation"}))
    # every requirement resolves by section 10
    by_set = {s["name"]: s for s in doc["sets"]}
    for s in doc["sets"]:
        tool = doc["ide"]["toolchains"][s["ide"]["toolchain"]]
        for tu in s["translation-units"]:
            for req in tu.get("requires", []):
                in_set = any(req in u.get("provides", {}) for u in s["translation-units"])
                in_visible = any(req in u.get("provides", {}) and not u.get("private", False)
                                 for v in s["visible-sets"] for u in by_set[v]["translation-units"])
                stdlib = req in ("std", "std.compat") and "module-metadata" in tool.get("stdlib", {})
                check(f"S1 {name}: {tu['source']} requirement {req} resolves", in_set or in_visible or stdlib)
                check(f"S1 {name}: {tu['source']} writes partition {req} in full", not req.startswith(":"))
    # sections 5-8: profile data at every level of a level 2 document
    check(f"S1 {name}: level 2 profile data at document, set and unit level",
          all("ide" in s and all("role" in tu.get("ide", {}) for tu in s["translation-units"]) for s in doc["sets"]))
    check(f"S1 {name}: generator names the producer and its version", bool(doc["ide"].get("generator", {}).get("name"))
          and bool(doc["ide"].get("generator", {}).get("version")))
    absolute = re.compile(r"^(/|[A-Za-z]:[\\/]|\$\{)")
    check(f"S1 {name}: drivers and work directories are absolute",
          all(absolute.match(t["driver"]) for t in doc["ide"]["toolchains"].values())
          and all(absolute.match(tu["work-directory"]) for s in doc["sets"] for tu in s["translation-units"]))
    check(f"S1 {name}: toolchains list their config-files", all("config-files" in t for t in doc["ide"]["toolchains"].values()))
    check(f"S1 {name}: stdlib objects name their version", all("version" in t["stdlib"] for t in doc["ide"]["toolchains"].values() if "stdlib" in t))
    for s in doc["sets"]:
        closure, pending = set(), list(s["visible-sets"])
        while pending:
            other = pending.pop()
            if other in closure or other not in by_set:
                continue
            closure.add(other)
            pending.extend(by_set[other]["visible-sets"])
        check(f"S1 {name}: {s['name']} visible-sets list the complete closure", closure <= set(s["visible-sets"]) | {s["name"]})
        options = [s["ide"].get("options")] + [tu.get("ide", {}).get("options") for tu in s["translation-units"]]
        text = json.dumps([o for o in options if o])
        check(f"S1 {name}: {s['name']} options carry no BMI location arguments",
              not re.search(r"-fmodule-file|-fmodule-mapper|/reference|-fmodules-cache-path|@modmap", text))
        check(f"S1 {name}: {s['name']} options carry no optimization, debug or output arguments",
              not re.search(r'"(-O[0-3sz]?|-g[0-9]?|-o|/O[12dx]|/Zi|/Fo[^"]*|-M[DMF]?)"', text))
        check(f"S1 {name}: {s['name']} carries family-name, baseline-arguments, configuration and kind",
              all(k in s for k in ("family-name", "baseline-arguments")) and all(k in s["ide"] for k in ("configuration", "kind")))
        check(f"S1 {name}: {s['name']} units carry local-arguments and private",
              all("local-arguments" in tu and "private" in tu for tu in s["translation-units"]))
        needs_std = any(req in ("std", "std.compat") for tu in s["translation-units"] for req in tu.get("requires", []))
        provided = {m for other in [s] + [by_set[v] for v in s["visible-sets"] if v in by_set]
                    for tu in other["translation-units"] for m in tu.get("provides", {})}
        tool = doc["ide"]["toolchains"][s["ide"]["toolchain"]]
        check(f"S1 {name}: {s['name']} toolchain has stdlib where std is required and not provided by units",
              not needs_std or {"std"} <= provided or "stdlib" in tool)

def s1_level3(name, doc):
    check(f"S1 {name}: level 3 sets carry structured options", all("options" in s["ide"] for s in doc["sets"]))

for name in ["s1-level3-gcc.json", "s1-level2-clang-two-sets.json"]:
    s1_semantics(name, load(ex / name))
s1_level3("s1-level3-gcc.json", load(ex / "s1-level3-gcc.json"))
darwin = load(ex / "s4-kit-darwin-arm64.json")
check("S4 darwin kit declares macos-sdk", {"kind": "macos-sdk"} in darwin.get("requires", []))
for name in ["s4-kit-linux-x64.json", "s4-kit-win32-x64.json", "s4-kit-darwin-arm64.json"]:
    kit = load(ex / name)
    check(f"S4 {name}: libc++ version equals pinned clangd 23.1.0", kit["stdlib"]["version"] == "23.1.0")

# 4. negative cases: schemas reject what the text forbids
base_kit = load(ex / "s4-kit-linux-x64.json")
for label, mutate in [
    ("absolute include directory", lambda k: k["system-include-directories"].append("/usr/include")),
    ("parent traversal", lambda k: k["system-include-directories"].append("include/../../etc")),
    ("drive prefix", lambda k: k.update({"sysroot": "C:/sdk"})),
    ("backslash separator", lambda k: k["licenses"].append("licenses\\X.TXT")),
    ("unknown kit-version", lambda k: k.update({"kit-version": 2})),
    ("missing licenses", lambda k: k.pop("licenses")),
]:
    kit = copy.deepcopy(base_kit); mutate(kit)
    validate(f"S4 schema rejects {label}", s4, kit, expect_valid=False)
kit = copy.deepcopy(base_kit); kit["system-include-directories"].append("include/..hidden/v1")
validate("S4 schema accepts a component that merely starts with '..'", s4, kit)
for field in ["name", "target", "stdlib", "system-include-directories"]:
    kit = copy.deepcopy(base_kit); kit.pop(field)
    validate(f"S4 schema rejects missing {field}", s4, kit, expect_valid=False)
for field in ["name", "version", "module-metadata"]:
    kit = copy.deepcopy(base_kit); kit["stdlib"].pop(field)
    validate(f"S4 schema rejects missing stdlib.{field}", s4, kit, expect_valid=False)
kit = copy.deepcopy(base_kit); kit["requires"] = [{"note": "no kind"}]
validate("S4 schema rejects a requirement without kind", s4, kit, expect_valid=False)
kit = copy.deepcopy(base_kit); kit["future-field"] = {"x": 1}; kit["stdlib"]["vendor"] = "y"
validate("S4 schema accepts unknown fields", s4, kit)

base_db = load(ex / "s1-level3-gcc.json")
for label, mutate in [
    ("unknown role", lambda d: d["sets"][0]["translation-units"][0]["ide"].update({"role": "interface"})),
    ("unknown family", lambda d: d["ide"]["toolchains"]["gcc-16.1.0-x86_64-linux-gnu"].update({"family": "icc"})),
    ("set ide without toolchain", lambda d: d["sets"][0]["ide"].pop("toolchain")),
    ("macro with define and undefine", lambda d: d["sets"][0]["ide"]["options"]["macros"].append({"define": "A", "undefine": "A"})),
    ("module name with space", lambda d: d["sets"][0]["translation-units"][2]["requires"].append("bad name")),
    ("missing sets", lambda d: d.pop("sets")),
]:
    db = copy.deepcopy(base_db); mutate(db)
    validate(f"S1 schema rejects {label}", s1, db, expect_valid=False)
toolchain_id = "gcc-16.1.0-x86_64-linux-gnu"
for label, mutate in [
    ("missing version", lambda d: d.pop("version")),
    ("missing revision", lambda d: d.pop("revision")),
    ("ide without profile-version", lambda d: d["ide"].pop("profile-version")),
    ("ide without toolchains", lambda d: d["ide"].pop("toolchains")),
    ("generator without name", lambda d: d["ide"]["generator"].pop("name")),
    ("toolchain without family", lambda d: d["ide"]["toolchains"][toolchain_id].pop("family")),
    ("toolchain without version", lambda d: d["ide"]["toolchains"][toolchain_id].pop("version")),
    ("toolchain without driver", lambda d: d["ide"]["toolchains"][toolchain_id].pop("driver")),
    ("toolchain without target", lambda d: d["ide"]["toolchains"][toolchain_id].pop("target")),
    ("introspection without command", lambda d: d["ide"]["toolchains"][toolchain_id].update({"introspection": [{"output": "x"}]})),
    ("stdlib without name", lambda d: d["ide"]["toolchains"][toolchain_id]["stdlib"].pop("name")),
    ("stdlib of unknown name", lambda d: d["ide"]["toolchains"][toolchain_id]["stdlib"].update({"name": "stlport"})),
    ("set without name", lambda d: d["sets"][0].pop("name")),
    ("set without visible-sets", lambda d: d["sets"][0].pop("visible-sets")),
    ("set without translation-units", lambda d: d["sets"][0].pop("translation-units")),
    ("unit without source", lambda d: d["sets"][0]["translation-units"][0].pop("source")),
    ("unit without work-directory", lambda d: d["sets"][0]["translation-units"][0].pop("work-directory")),
    ("unit without arguments", lambda d: d["sets"][0]["translation-units"][0].pop("arguments")),
    ("unit ide without role", lambda d: d["sets"][0]["translation-units"][0]["ide"].pop("role")),
]:
    db = copy.deepcopy(base_db); mutate(db)
    validate(f"S1 schema rejects {label}", s1, db, expect_valid=False)
db = copy.deepcopy(base_db); db["sets"][0]["translation-units"][2]["requires"].append("módulo.ñ")
validate("S1 schema accepts non-ASCII module names", s1, db)
db = copy.deepcopy(base_db); db["unknown-top-level"] = {"x": 1}; db["sets"][0]["ide"]["vendor"] = True
validate("S1 schema accepts unknown fields", s1, db)

for label, doc in [
    ("finished without watch", {"kind": "finished", "database": "/x/db.json"}),
    ("unknown kind", {"kind": "done"}),
    ("request without profile-version", {"workspace": "/w"}),
]:
    validate(f"S2 schema rejects {label}", s2, doc, expect_valid=False)
for label, doc in [
    ("progress without message", {"kind": "progress"}),
    ("finished without database", {"kind": "finished", "watch": []}),
    ("finished with a relative database", {"kind": "finished", "database": "target/db.json", "watch": []}),
    ("error without message", {"kind": "error", "code": "x"}),
    ("request without workspace", {"profile-version": "0.2.0"}),
]:
    validate(f"S2 schema rejects {label}", s2, doc, expect_valid=False)
validate("S2 schema accepts a Windows absolute database", s2, {"kind": "finished", "database": "C:/w/db.json", "watch": []})
base_envelope = load(ex / "s2-envelope.json")
for label, mutate in [
    ("envelope without schemaVersion", lambda e: e.pop("schemaVersion")),
    ("envelope of another schemaVersion", lambda e: e.update({"schemaVersion": 2})),
    ("envelope whose kind is not a build database", lambda e: e.update({"kind": "mcpp.build"})),
    ("envelope without kindVersion", lambda e: e.pop("kindVersion")),
    ("envelope without effects", lambda e: e.pop("effects")),
    ("envelope data without database", lambda e: e["data"].pop("database")),
    ("envelope data without watch", lambda e: e["data"].pop("watch")),
    ("envelope without diagnostics", lambda e: e.pop("diagnostics")),
    ("envelope diagnostic of unknown severity", lambda e: e["diagnostics"].append({"code": "X", "severity": "fatal", "message": "m"})),
]:
    envelope = copy.deepcopy(base_envelope); mutate(envelope)
    validate(f"S2 schema rejects {label}", s2, envelope, expect_valid=False)
envelope = copy.deepcopy(base_envelope); envelope.pop("data")
envelope["diagnostics"] = [{"code": "E_TOOLCHAIN", "severity": "error", "message": "no compiler"}]
validate("S2 schema accepts a failed command without data", s2, envelope)
# S2 0.3.0: a document that describes the workspace in part carries data and an error naming what it left out.
envelope = copy.deepcopy(base_envelope)
envelope["diagnostics"] = [{"code": "MCPP_MEMBER_PLAN_FAILED", "severity": "error", "message": "member updater could not be planned",
                            "path": "tools/updater/mcpp.toml"}]
validate("S2 schema accepts a partial document: data and an error naming a path", s2, envelope)
envelope["diagnostics"][0]["path"] = 7
validate("S2 schema rejects a diagnostic path that is not a string", s2, envelope, expect_valid=False)
s2_request_only = Draft202012Validator({"$ref": "#/$defs/request",
                                        "$defs": load(root / "schema" / "s2-discovery.schema.json")["$defs"]})
validate("S2 request definition rejects a request carrying kind", s2_request_only,
         {"workspace": "/w", "profile-version": "0.2.0", "kind": "progress"}, expect_valid=False)

# 5. JSON blocks embedded in the specification text
def json_blocks(md):
    return re.findall(r"```json\n(.*?)```", md.read_text(encoding="utf-8"), flags=re.S)
s1_blocks = json_blocks(root / "s1-build-database.md")
check("S1 text: complete example equals examples/s1-level3-gcc.json", json.loads(s1_blocks[0]) == base_db)
s2_blocks = json_blocks(root / "s2-discovery.md")
validate("S2 text: request block validates", s2, json.loads(s2_blocks[0]))
s2_text = (root / "s2-discovery.md").read_text(encoding="utf-8")
stream = re.search(r"```text\n(.*?)```", s2_text, flags=re.S).group(1)
for i, line in enumerate(l for l in stream.splitlines() if l.strip()):
    validate(f"S2 text: stream line {i + 1} validates", s2, json.loads(line))
s4_blocks = json_blocks(root / "s4-semantic-kit.md")
check("S4 text: example equals examples/s4-kit-win32-x64.json", json.loads(s4_blocks[0]) == load(ex / "s4-kit-win32-x64.json"))

# 6. relative links in markdown resolve
for md in sorted(root.glob("*.md")):
    for target in re.findall(r"\]\(([^)#:]+)(?:#[^)]*)?\)", md.read_text(encoding="utf-8")):
        check(f"link in {md.name}: {target}", (root / target).exists())


# 7. the simulated producer data of conformance fixtures (mcpp-community/mcpp#636) is S1, in the shape mcpp
#    decided: level 2 (level 3 is the S1 library's), one set per package plus <package>:test and mcpp:std, and
#    every set seeing every other, since the build resolves imports over one flat module graph
def mcpp_contract(name, doc):
    sets = doc["sets"]
    names = [s["name"] for s in sets]
    check(f"mcpp {name}: level 2, structured options left to the S1 library",
          all("options" not in s["ide"] and all("options" not in tu["ide"] for tu in s["translation-units"]) for s in sets))
    check(f"mcpp {name}: sets are packages, <package>:test and mcpp:std",
          all(re.fullmatch(r"[^:]+(:test)?|mcpp:std", n) for n in names))
    check(f"mcpp {name}: every set sees every other set",
          all(sorted(s["visible-sets"]) == sorted(n for n in names if n != s["name"]) for s in sets))
    std_sets = {s["name"] for s in sets for tu in s["translation-units"] if {"std", "std.compat"} & set(tu.get("provides", {}))}
    check(f"mcpp {name}: the standard library modules are units of mcpp:std", std_sets == {"mcpp:std"})
    check(f"mcpp {name}: <package>:test sets hold tests", all(s["ide"].get("kind") == "test" for s in sets if s["name"].endswith(":test")))

for mock in sorted((repository / "conformance" / "fixtures").glob("*/mcpp-mock.json")):
    data = load(mock)
    if "database" not in data:
        continue
    text = json.dumps(data["database"]).replace("${root}", "/workspace")
    text = re.sub(r"\$\{env:[^}|]*(\|[^}]*)?\}", "/env", text)
    database = json.loads(text)
    validate(f"S1 fixture data validates: {mock.parent.name}", s1, database)
    s1_semantics(mock.parent.name, database)
    mcpp_contract(mock.parent.name, database)
# and so is the database of the S2 example, an mcpp.build-database envelope
s1_semantics("s2-envelope.json", base_envelope["data"]["database"])
mcpp_contract("s2-envelope.json", base_envelope["data"]["database"])
# and so is what a real mcpp prints (usable plan W3): CI's conformance jobs record mcpp's envelope for each mcpp
# fixture and pass them here, so the simulated data and the contract cannot drift from the producer
for path in map(pathlib.Path, sys.argv[1:]):
    envelope = load(path)
    validate(f"S2 envelope printed by mcpp validates: {path.name}", s2, envelope)
    check(f"mcpp {path.name}: the command succeeded", "data" in envelope and not any(d.get("severity") == "error" for d in envelope.get("diagnostics", [])))
    if "data" not in envelope:
        continue
    check(f"mcpp {path.name}: the command wrote nothing into the project", "write-project" not in envelope.get("effects", []))
    validate(f"S1 database printed by mcpp validates: {path.name}", s1, envelope["data"]["database"])
    s1_semantics(path.name, envelope["data"]["database"])
    mcpp_contract(path.name, envelope["data"]["database"])

# 8. traceability (usable plan W10.2): every requirement keyword carries a rule identifier S<n>-<section>-<ordinal>,
#    and conformance/traceability.json names evidence for each: a check above, a unit test, a conformance check, a
#    line of a script that enforces the rule, or the reason no automated evidence can exist.
TRACED_SPECS = {"S1": "s1-build-database.md", "S2": "s2-discovery.md", "S3": "s3-lsp-extensions.md", "S4": "s4-semantic-kit.md",
                "S5": "s5-semantic-query.md"}
KEYWORD = re.compile(r"\b(MUST NOT|MUST|SHALL NOT|SHALL|SHOULD NOT|SHOULD|REQUIRED|RECOMMENDED)\b")
HEADING = re.compile(r"^#{2,4}\s+(\d+(?:\.\d+)*)\.?\s")
RULE_ID = re.compile(r'<a id="(S\d-[\d.]+-\d+)"></a>')

rules = {}
for spec, name in TRACED_SPECS.items():
    section, in_code = "0", False
    for number, line in enumerate((root / name).read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("```"):
            in_code = not in_code
        if in_code or line.startswith("```"):
            continue
        heading = HEADING.match(line)
        if heading:
            section = heading.group(1)
            continue
        if "interpreted as described in BCP 14" in line:
            continue
        ids = RULE_ID.findall(line)
        keywords = KEYWORD.findall(re.sub(r"`[^`]*`|<[^>]*>", "", line))
        where = f"{name}:{number}"
        if len(ids) < len(keywords):
            check(f"traceability: {where} has {len(keywords)} requirement(s) and {len(ids)} identifier(s)", False, line[:120])
        for rule in ids:
            if not rule.startswith(f"{spec}-{section}-"):
                check(f"traceability: {rule} at {where} is numbered in section {section}", False)
            if rule in rules:
                check(f"traceability: {rule} is defined once", False, f"{rules[rule]} and {where}")
            rules[rule] = where

tests = {}
for path in sorted((repository / "tests").glob("*.cpp")):
    for name in re.findall(r'^\s*"((?:[^"\\]|\\.)*)"_test\s*=', path.read_text(encoding="utf-8"), flags=re.M):
        tests[f"tests/{path.name}: {name}"] = True
fixture_checks = set()
for scenario in sorted((repository / "conformance" / "fixtures").glob("*/scenario.json")):
    for item in load(scenario).get("checks", []):
        fixture_checks.add(f"{scenario.parent.name}/{item.get('id')}")

def evidence_ok(entry):
    if "validate" in entry:
        return any(label.startswith(entry["validate"]) for label in passed), f"no passing check starts with {entry['validate']!r}"
    if "test" in entry:
        return entry["test"] in tests, f"no unit test {entry['test']!r}"
    if "check" in entry:
        return entry["check"] in fixture_checks, f"no conformance check {entry['check']!r}"
    if "review" in entry:
        return (repository / "tools" / "bench" / "review" / entry["review"] / "review.json").is_file(), f"no review fixture {entry['review']!r}"
    if "script" in entry:
        path = repository / entry["script"]
        return path.is_file() and entry.get("contains", "\0") in path.read_text(encoding="utf-8"), \
            f"{entry['script']} does not contain {entry.get('contains')!r}"
    if "manual" in entry:
        return len(entry["manual"].strip()) >= 20, "a manual entry states why no automated evidence exists"
    return False, f"unknown evidence {entry}"

traceability = load(repository / "conformance" / "traceability.json")
pending = traceability.get("$pending", {})
kinds = {}
for rule, where in rules.items():
    entries = traceability.get(rule)
    if not entries and rule in pending:
        # A known gap, named with what is missing; it is listed in every run until it has evidence.
        print(f"PENDING  traceability: {rule} ({where}): {pending[rule]}")
        continue
    if not entries:
        check(f"traceability: {rule} ({where}) has evidence", False)
        continue
    for entry in entries:
        ok, detail = evidence_ok(entry)
        check(f"traceability: {rule} evidence {next(iter(entry))}", ok, detail)
        kinds[next(iter(entry))] = kinds.get(next(iter(entry)), 0) + 1
for rule in traceability:
    if rule not in rules and not rule.startswith("$"):
        check(f"traceability: {rule} exists in the specifications", False)
per_spec = {spec: sum(1 for rule in rules if rule.startswith(spec + "-")) for spec in TRACED_SPECS}
for rule in pending:
    check(f"traceability: pending {rule} exists and has no evidence yet", rule in rules and not traceability.get(rule))
print(f"\ntraceability: {len(rules)} rules ({', '.join(f'{k} {v}' for k, v in per_spec.items())}); evidence {kinds}; pending {len(pending)}")

print(f"\n{failures} failure(s)")
sys.exit(1 if failures else 0)
