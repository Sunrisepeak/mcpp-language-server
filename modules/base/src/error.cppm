// Errors travel as values. A function that can fail returns base::Result<T>;
// the error carries a stable machine code and a human message.
export module mcppls.base.error;

import std;
import mcpplibs.os;

export namespace mcppls::base {

struct Error {
    std::string code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

Error make_error(std::string_view code, std::string message);
std::unexpected<Error> fail(std::string_view code, std::string message);
// What mcpplibs.os answered, as this program's error: `code` names the operation, the message is the system's.
std::unexpected<Error> fail(std::string_view code, const mcpplibs::os::error& error);
std::string to_string(const Error& error);

} // namespace mcppls::base
