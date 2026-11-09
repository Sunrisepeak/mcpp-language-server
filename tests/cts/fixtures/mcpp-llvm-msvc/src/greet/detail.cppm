export module hello.greet:detail;
import std;
export namespace hello::detail {
  std::string prefix() { return "Hello, "; }
}
