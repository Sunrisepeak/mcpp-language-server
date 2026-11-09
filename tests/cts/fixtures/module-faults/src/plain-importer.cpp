// A plain importer of a module that fails to compile: not a unit of broken.f itself (like
// src/broken/f.cppm), just a file that imports it, the way xlings' main.cpp imported
// mcpplibs.xpkg.executor in the incident this fixture's closure-scoped checks are named for.
import broken.f;

int use_broken() { return ff(); }
