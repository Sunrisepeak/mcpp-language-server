# Typing imports with autosave

0.0.17 W4 (#59 U15). For forty seconds the editor types into `src/main.cpp` at ten keys a second,
an import line and then a line in a function body, in turn, and saves every second, as ux-mcpp's U6
does with `files.autoSave`. Some saves change the file's imports and the ones between do not.

Up to 0.0.16 each pause in the import changes was long enough for the reload they asked for to fire:
against this fixture, 6 producer runs and 13 status changes in the busiest minute (U15's budget is
12). Now a reload that saves of a file still being typed in ask for waits until the file has been
quiet for `EDIT_SETTLE` (two seconds), never more than `EDIT_DEFER_LIMIT` (thirty seconds) after the
first save that waited: 3 runs (at start, at the thirty-second bound, once typing stops) and 4
status changes. The checks hold the count of `emit` calls in the mock producer's call log to 2-3
and the status to 6 changes a minute.
