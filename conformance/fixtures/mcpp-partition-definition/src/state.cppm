export module hello.greet:state;
import std;
import :types;
export namespace hello {
struct State { int n = 0; std::string log; };
int phase_a(State& s);
int phase_b(State& s, int k);
int plain_c(int k);
struct Box { int f(const std::string& s); int g(int k); };
int phase_p(const Point& p);
std::string phase_s(const std::string& t);
}
