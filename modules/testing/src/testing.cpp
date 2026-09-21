module mcppls.testing;

import std;

namespace mcppls::testing {

namespace {

struct Counters {
    int cases { 0 };
    int failedCases { 0 };
    int assertions { 0 };
    int failedAssertions { 0 };
    bool currentFailed { false };
    std::string_view currentName;
};

Counters& counters() {
    static Counters value;
    return value;
}

// Thrown by a failed fatal expectation to leave the test case.
struct AbortCase {};

void print_error(std::string_view text) {
    std::cerr << text;
    std::cerr.flush();
}

} // namespace

Expectation::Expectation(bool passed, bool isFatal, std::source_location location)
    : passed_ { passed }, fatal_ { isFatal }, location_ { location } {
    ++counters().assertions;
}

Expectation::~Expectation() noexcept(false) {
    if (passed_) return;
    auto& state = counters();
    ++state.failedAssertions;
    state.currentFailed = true;
    print_error(std::format("  FAILED {}:{}{}{}\n", location_.file_name(), location_.line(), message_.empty() ? "" : "  ", message_));
    if (fatal_ && std::uncaught_exceptions() == 0) throw AbortCase {};
}

Expectation expect(bool condition, std::source_location location) { return Expectation { condition, false, location }; }

Expectation expect(Fatal condition, std::source_location location) { return Expectation { condition.value, true, location }; }

void TestCase::operator=(const std::function<void()>& body) const {
    auto& state = counters();
    ++state.cases;
    state.currentFailed = false;
    state.currentName = name_;
    // Unbuffered, so that a test that ends the process still shows where it was.
    print_error(std::format("[ run ] {}\n", name_));
    try {
        body();
    } catch (const AbortCase&) {
        // the failure is already reported
    } catch (const std::exception& error) {
        state.currentFailed = true;
        print_error(std::format("  EXCEPTION {}\n", error.what()));
    } catch (...) {
        state.currentFailed = true;
        print_error("  EXCEPTION of an unknown type\n");
    }
    if (state.currentFailed) {
        ++state.failedCases;
        print_error(std::format("test \"{}\" failed\n", name_));
    }
}

int report() {
    const auto& state = counters();
    std::cout << std::format("{} test cases, {} failed; {} assertions, {} failed\n", state.cases, state.failedCases,
                             state.assertions, state.failedAssertions);
    std::cout.flush();
    return state.failedCases == 0 ? 0 : 1;
}

} // namespace mcppls::testing
