export module json:decode;
import std;
import :data;
import :error;
export namespace json { Value decode(std::string_view s); }
