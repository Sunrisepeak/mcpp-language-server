export module hello.greet;
import std;

export namespace hello {
  std::string greet(std::string_view who) { return "Hello, " + std::string(who); }
}
