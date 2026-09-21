import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.platform.task;

int main() {
    using namespace mcppls::testing;
    using mcppls::platform::Channel;

    "values cross threads in order"_test = [] {
        Channel<int> channel;
        std::jthread producer { [&] {
            for (int i { 0 }; i < 1000; ++i) channel.push(i);
            channel.close();
        } };
        int expected { 0 };
        bool ordered { true };
        while (auto value = channel.pop()) ordered = ordered && *value == expected++;
        expect(ordered);
        expect(expected == 1000_i);
    };

    "a deadline expires without a value"_test = [] {
        Channel<std::string> channel;
        const auto started = std::chrono::steady_clock::now();
        auto value = channel.pop_until(started + std::chrono::milliseconds { 50 });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(!value.has_value());
        expect(elapsed >= std::chrono::milliseconds { 40 });
        expect(elapsed < std::chrono::seconds { 10 });
    };

    "a push wakes a reader waiting with a deadline"_test = [] {
        Channel<int> channel;
        std::jthread producer { [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
            channel.push(7);
        } };
        const auto started = std::chrono::steady_clock::now();
        auto value = channel.pop_until(started + std::chrono::seconds { 10 });
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(value == std::optional<int> { 7 });
        expect(elapsed < std::chrono::seconds { 5 }) << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";
    };

    "values with initializer-list constructors are moved, not wrapped"_test = [] {
        Channel<nlohmann::json> channel;
        channel.push(nlohmann::json { { "id", 1 } });
        auto value = channel.pop();
        expect(fatal(value.has_value()));
        expect(value->is_object() && value->contains("id")) << value->dump();
    };

    "closing wakes a waiting reader"_test = [] {
        Channel<int> channel;
        std::jthread closer { [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
            channel.close();
        } };
        expect(!channel.pop().has_value());
        expect(!channel.push(1));
    };

    return report();
}
