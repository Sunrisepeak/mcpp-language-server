module hello.greet;
import :state;
namespace hello {
int phase_b(State& s, int k) { s.n += k; return s.n; }
}
namespace hello {
int phase_p(const Point& p) { return p.x; }
std::string phase_s(const std::string& t) { return t; }
}
namespace hello {
int Box::f(const std::string& s) { return (int)s.size(); }
}
