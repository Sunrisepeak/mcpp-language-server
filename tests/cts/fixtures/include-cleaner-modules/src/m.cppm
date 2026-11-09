module;
#include <cstdio>
#include <vector>
#include <string>
#include <regex>
#include <format>
#include <map>
export module m;
export std::vector<int> f() { std::printf("x"); return {}; }
export using std::string;
export std::map<int, int> g();
