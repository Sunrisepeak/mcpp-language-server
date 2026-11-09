module json;
import std;
namespace json { Value decode(std::string_view s) { return Value { std::string { s } }; } }
