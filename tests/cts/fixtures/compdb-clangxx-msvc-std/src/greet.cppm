export module greet;
import std;

export namespace greet {
std::string message(std::string_view name) { return std::format("hello, {}", name); }
}
