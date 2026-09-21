// A minimal test harness for mcppls's unit tests, written as a named module.
//
//   import mcppls.testing;
//   int main() {
//       using namespace mcppls::testing;
//       "adds"_test = [] { expect(1 + 1 == 2) << "arithmetic"; };
//       return report();
//   }
//
// mcpp runs every tests/**/*.cpp as a program and a test passes when it exits
// zero; report() returns non-zero when any expectation failed.
export module mcppls.testing;

import std;

export namespace mcppls::testing {

// Wraps a condition whose failure ends the current test case.
struct Fatal {
    bool value { false };
};

inline Fatal fatal(bool value) { return Fatal { value }; }

class Expectation {
private:
    bool passed_ { true };
    bool fatal_ { false };
    std::source_location location_;
    std::string message_;

public:
    Expectation(bool passed, bool isFatal, std::source_location location);
    Expectation(const Expectation&) = delete;
    Expectation& operator=(const Expectation&) = delete;
    ~Expectation() noexcept(false);

public:
    template <class T>
    Expectation& operator<<(const T& value) {
        if (!passed_) {
            if constexpr (std::is_convertible_v<const T&, std::string_view>) {
                message_ += std::string_view { value };
            } else {
                message_ += std::format("{}", value);
            }
        }
        return *this;
    }
};

Expectation expect(bool condition, std::source_location location = std::source_location::current());
Expectation expect(Fatal condition, std::source_location location = std::source_location::current());

class TestCase {
private:
    std::string_view name_;

public:
    explicit constexpr TestCase(std::string_view name) : name_ { name } {}
    // Runs the body now, catching what escapes it.
    void operator=(const std::function<void()>& body) const;
};

inline constexpr TestCase operator""_test(const char* name, std::size_t size) { return TestCase { std::string_view { name, size } }; }
inline constexpr int operator""_i(unsigned long long value) { return static_cast<int>(value); }

// Prints the summary and returns the process exit code.
int report();

} // namespace mcppls::testing
