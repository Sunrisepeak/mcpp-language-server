module hello.greet;
import :state;
import std;
namespace hello {
int run_all() {
    State s;
    phase_a(s);
    phase_b(s, 2);
    Point pt{1, 2};
    Box bx; bx.f("y"); bx.g(1);
    phase_s("x");
    return plain_c(s.n) + phase_p(pt);
}
}
