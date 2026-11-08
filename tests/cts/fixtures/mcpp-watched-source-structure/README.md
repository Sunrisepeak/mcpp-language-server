# Watched C++ source acknowledgement

Run the fixture with the normal `mcppls-conformance run --fixture` command and the
bundled mock producer beside the runner. It uses real LSP file-watch notifications,
an offline producer call log and a valid, unchanged S1 dependency graph.

`#if 0` excludes `disabled.alpha` from the compiler's dependency graph, while the
lexical scanner still sees its import. The first structural edit must run the
producer once. Body-only edits and a repeated identical write must then avoid both
another emit and a `loading` status. A second structural edit, a manifest edit, a
live target change and replacement of the native producer must still run it.

The original **28-check** version failed against the unchanged `1341938` product:
11 actual emits instead of the expected 6, with 14 failed checks (including count
cascades). These results are saved under
`.payload-cache/59-63-watched-source-structure/old-product-*`. They demonstrate a
repeat-scheduling defect; they do not identify the cause of the original U15
13-transitions-per-minute failure.

The current **70-check** version keeps those assertions and adds directive-change,
failed-description/retry and in-flight-edit controls. A failed producer response
cannot acknowledge a source. A later valid unchanged response may acknowledge only
the structure captured when that run started. A second edit while a delayed run is
active needs its own producer run. The native workspace unit regression separately
changes disk before the later watch arrives, distinguishing a run-start snapshot
from an incorrect snapshot taken when its answer completes. It also restores the
original bytes after an intervening structure change: the changed file stamp must
prevent that run from acknowledging the original structure.

The added `src/condition.cpp` begins with an active conditional import that exactly
matches its producer dependencies. Disabling it must cause a new description both
on its first watch and after a successful acknowledgement. The producer recordings
change that unit's `requires` between `target.platform` and empty alongside those
source changes; the test does not pretend an active import has no dependency.

Acknowledgement is session-local and conservative. Only a non-inferred producer
answer with no model issues confirms it; informational notices and the engine's
source diagnostics do not prevent confirmation. Cache restoration, rejected or
stale answers, unreadable/unknown units and input changes outside that confirmed
generation retain the existing reload path. Snapshot reads cover only previously
watched source paths, with a shared 32 MiB bound and file stamps unchanged through
the valid answer. Stamps use the existing size/time identity; this is not a claim
to detect a writer deliberately restoring those metadata.

The key includes module/import structure and exact preprocessor logical lines,
including comments and continuations. Directive changes therefore remain inputs
even when import names are unchanged. Alternate directive spellings, pragma
operators, location/counter macros, or `#` characters not accounted for by the
scanner outside comments and literals conservatively disable acknowledgement instead of guessing. Build-program
sources retain their existing dedicated handling.

An import the lexer cannot fully parse, including `import SELECT(active);`, is not
acknowledged: macro replacement can change its dependency while the lexical names
and macro definition remain unchanged. Header-unit imports also conservatively
disable acknowledgement; the global `required_names` meaning is unchanged. The
scanner's directive-string retention is explicitly opt-in for these two workspace
checks; ordinary index and engine scans retain no directive text.

Unparsed module declarations (including private fragments not represented by this scanner) also conservatively forgo acknowledgement; the normal global module fragment `module;` remains supported. This is a lexical acknowledgement guard, not a full preprocessor.
