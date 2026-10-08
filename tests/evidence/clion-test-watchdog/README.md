# CLion unit-test backend watchdog mode

Native product7062e07 failed during fixture initialization because CLion2026.2.3 rejects its backend freeze watchdog in unit tests. SDK bytecode reads this key as an integer, default2000; timeout0 disables the tracker. tasks.test now supplies0, retaining real server startup/shutdown deadlines and every functional assertion. The force-enable boolean stays false.

Exact current local Gradle suite passes all5 tests in2m9s, including the two-engine/one-notice case, and emits no watchdog assertion. [SDK contract and test identities](result.json). Corrected native headless job [113281429141](https://github.com/Sunrisepeak/mcpp-language-server/actions/runs/37766811581/job/113281429141) passes on source1748249. The overall run was subsequently cancelled by a newer push; this qualifies that job, not all CI or the joint engine. Unrelated platform log warnings are retained.
