// Errors travel as values. A function that can fail returns base::Result<T>;
// the error carries a stable machine code and a human message.
export module mcppls.base.error;

import std;

export namespace mcppls::base {

struct Error {
    std::string code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

Error make_error(std::string_view code, std::string message);
std::unexpected<Error> fail(std::string_view code, std::string message);
std::string to_string(const Error& error);

} // namespace mcppls::base
