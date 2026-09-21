// The query commands (overall design 8.1, S5 7): a headless session answers one question and exits.
//   mcppls query symbol <name> [--at FILE:LINE:COLUMN] [--id USR] [--kind K] [--module M] [--max N]
//   mcppls query refs <name> [--at ...] [--id USR] [--no-declaration] [--max N]
//   mcppls query calls <name> [--at ...] [--id USR] [--callees] [--max N]
//   mcppls query outline <file>
//   mcppls query module <name> [--file F] [--graph]
//   mcppls query context <file>
//   mcppls diagnostics <file>... [--no-fresh]
//   mcppls verify [<file>...] [--changed] [--base REV] [--budget N] | --snippet FILE:LINE --code TEXT [--replace-lines N]
//   mcppls impact [<file>...] [--base REV] [--budget N]
//   mcppls review [<file>...] [--base REV] [--budget N] [--format json|text|sarif|markdown] [--output FILE]
// Every command takes --root DIR, --timeout SECONDS and --format json|text. Exit status: 0 with a
// result, 1 when the query found nothing (or diagnostics include errors), 2 when the command failed.
export module mcppls.cli.query;

import std;
import mcpplibs.cmdline;

export namespace mcppls::cli {

mcpplibs::cmdline::App query_command(bool& handled, int& status);
mcpplibs::cmdline::App diagnostics_command(bool& handled, int& status);
mcpplibs::cmdline::App verify_command(bool& handled, int& status);
mcpplibs::cmdline::App impact_command(bool& handled, int& status);
mcpplibs::cmdline::App review_command(bool& handled, int& status);

} // namespace mcppls::cli
