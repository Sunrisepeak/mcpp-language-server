// A sibling module of the project's own that uses the generated package's declarations.
export module app.consumer;
import xpkg.lua_stdlib;

export int consumerAnswer() { return xpkg::lua_stdlib_answer() + 1; }
