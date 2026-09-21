// Message passing between threads: a closable blocking queue. The server's
// main loop owns all session state and receives everything through one of these.
export module mcppls.platform.task;

import std;

export namespace mcppls::platform {

template <class T>
class Channel {
private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> items_;
    bool closed_ { false };

public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

public:
    // Returns false when the channel is closed and the value was dropped.
    bool push(T value) {
        {
            std::lock_guard lock { mutex_ };
            if (closed_) return false;
            items_.push_back(std::move(value));
        }
        ready_.notify_one();
        return true;
    }

    // Blocks until a value arrives; nullopt once closed and drained.
    std::optional<T> pop() {
        std::unique_lock lock { mutex_ };
        ready_.wait(lock, [&] { return closed_ || !items_.empty(); });
        return take_(lock);
    }

    // nullopt when the deadline passes first, or once closed and drained.
    std::optional<T> pop_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock { mutex_ };
        ready_.wait_until(lock, deadline, [&] { return closed_ || !items_.empty(); });
        return take_(lock);
    }

    void close() {
        {
            std::lock_guard lock { mutex_ };
            closed_ = true;
        }
        ready_.notify_all();
    }

    bool closed() const {
        std::lock_guard lock { mutex_ };
        return closed_;
    }

    std::size_t size() const {
        std::lock_guard lock { mutex_ };
        return items_.size();
    }

private:
    std::optional<T> take_(std::unique_lock<std::mutex>&) {
        if (items_.empty()) return std::nullopt;
        // Copy-initialization on purpose: braces would pick an initializer-list
        // constructor for types that have one (nlohmann::json makes an array).
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }
};

} // namespace mcppls::platform
