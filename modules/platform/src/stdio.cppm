// This program's standard input and output as raw byte streams. Standard output
// carries the Language Server Protocol, so writes are serialized.
export module mcppls.platform.stdio;

import std;
import mcppls.base.error;

export namespace mcppls::platform::stdio {

// Blocks until bytes arrive; an empty string means standard input ended.
base::Result<std::string> read_input();
base::Result<void> write_output(std::string_view bytes);
base::Result<void> write_error(std::string_view bytes);

} // namespace mcppls::platform::stdio
