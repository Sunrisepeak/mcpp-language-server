export module hello.greet;
export import :detail;
import std;

export namespace hello {
  std::string greet(std::string_view who) { return detail::prefix() + std::string(who); }
}
