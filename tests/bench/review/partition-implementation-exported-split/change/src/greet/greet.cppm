export module hello.greet;

import std;
export import :format;
export import :counter;

export namespace hello {
std::string greet(std::string_view who);
int count();
}
