// The stdio loop: PROTOCOL.md's line protocol in on stdin, out on stdout,
// diagnostics on stderr only. Owns the reader/worker threading that lets a
// `cancel` notification reach an in-flight `complete` call.
export module mcppls.model.gateway;

export namespace mcppls::model {

// Runs to completion (end of input, or an `exit` notification) and returns
// the process exit code. Parses argv itself (mcppls.model.config); a bad
// flag or --help/--version returns without reading any input.
int run(int argc, char* argv[]);

} // namespace mcppls::model
