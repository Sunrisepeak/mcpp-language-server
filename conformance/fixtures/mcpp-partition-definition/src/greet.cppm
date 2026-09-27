export module hello.greet;
export import :types;
export import :state;
import std;

export namespace hello {
std::string greet(std::string_view name);
int add(int a, int b);
int twice(int a);
int run_all();
std::string configure(bool verbose, int level = 1,
                      std::vector<std::string> names = {});
std::string shout(std::string_view s);

class Counter {
public:
    void bump();
    int value() const;
private:
    int n_ = 0;
};
}
