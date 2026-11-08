export module hello.greet;

import std;

export namespace hello {
  std::string greet(std::string_view who) { return std::string("hello, ") + std::string(who); }
}
