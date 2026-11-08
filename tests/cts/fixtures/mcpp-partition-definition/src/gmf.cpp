module;
#include <cstring>
module hello.greet;
import std;
namespace hello {
std::string shout(std::string_view s) { std::string r{s}; for (auto& c : r) c = (char)std::toupper(c); return r; }
}
