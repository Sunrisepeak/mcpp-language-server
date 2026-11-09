module;
#include "tp.hpp"
#include <cstdio>
module n;
tp::table make() { tp::table t { TP_VERSION }; std::printf("%d", tp::count(t)); return t; }
