// Written by a package's own build.mcpp into MCPP_OUT_DIR at build time (as libxpkg's does for
// mcpplibs.xpkg.lua_stdlib): a generated module interface, not a stand-in a consumer invents when
// it cannot see the real one. Real-project stress testing real-project plan RP0, checks
// this file's own path is what hover and definition land on, never a placeholder.
export module xpkg.lua_stdlib;

export namespace xpkg {

int lua_stdlib_answer() { return 42; }

} // namespace xpkg
