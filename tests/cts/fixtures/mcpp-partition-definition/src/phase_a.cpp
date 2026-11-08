module hello.greet;
import std;
namespace hello {
int phase_a(State& s) { s.log += "a"; return ++s.n; }
int plain_c(int k) { return k * 7; }
}
