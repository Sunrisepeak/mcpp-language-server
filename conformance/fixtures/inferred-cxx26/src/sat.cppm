export module sat;
import std;

// C++26: pack indexing (P2662) in a module's interface.
export template <class... Ts> using first_of = Ts...[0];

// C++26: saturation arithmetic (P0543), under its C++26 name.
export inline int clamp_add(int a, int b) { return std::saturating_add(a, b); }
