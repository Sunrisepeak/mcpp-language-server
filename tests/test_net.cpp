// Loopback connections (overall design 6.3, experiment X2): what the workspace daemon listens on.
import std;
import mcppls.testing;
import mcppls.platform.net;

namespace net = mcppls::platform::net;

int main() {
    using namespace mcppls::testing;

    "a loopback connection carries bytes both ways and ends when one side is done"_test = [] {
        auto listener = net::Listener::listen_local();
        expect(fatal(listener.has_value())) << (listener ? std::string {} : listener.error().message);
        expect(listener->port() > 0);
        std::optional<std::string> received;
        std::thread server { [&] {
            auto accepted = listener->accept();
            if (!accepted) return;
            std::string all;
            while (true) {
                auto chunk = accepted->read();
                if (!chunk || chunk->empty()) break;
                all += *chunk;
            }
            received = all;
            (void)accepted->write("pong\n");
            accepted->close();
        } };
        auto client = net::Connection::connect_local(listener->port());
        expect(fatal(client.has_value())) << (client ? std::string {} : client.error().message);
        expect(client->write("ping\n").has_value());
        // Half-closed: the server reads the end of the stream and still answers.
        client->shutdown_write();
        std::string answer;
        while (true) {
            auto chunk = client->read();
            if (!chunk || chunk->empty()) break;
            answer += *chunk;
        }
        server.join();
        expect(received == std::optional<std::string> { "ping\n" });
        expect(answer == "pong\n") << answer;
    };

    "a connection is read by one thread while others write, shut down and close it"_test = [] {
        // A read and a write that wait for each other never end; the test ends the program instead.
        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::thread { [finished] {
            for (int i { 0 }; i < 600 && !*finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
            if (!*finished) {
                std::println(std::cerr, "a read and a write on one connection waited for each other");
                std::_Exit(1);
            }
        } }.detach();

        auto listener = net::Listener::listen_local();
        expect(fatal(listener.has_value()));
        const auto open_pair = [&] {
            std::shared_ptr<net::Connection> server;
            std::thread accepting { [&] {
                if (auto accepted = listener->accept()) server = std::make_shared<net::Connection>(std::move(*accepted));
            } };
            auto client = std::make_shared<net::Connection>();
            if (auto connected = net::Connection::connect_local(listener->port())) *client = std::move(*connected);
            accepting.join();
            return std::pair { server, client };
        };
        // Everything a connection receives, read on a thread of its own until the end of the stream.
        const auto collect = [](std::shared_ptr<net::Connection> connection) {
            return std::async(std::launch::async, [connection] {
                std::string all;
                while (true) {
                    auto chunk = connection->read();
                    if (!chunk || chunk->empty()) break;
                    all += *chunk;
                }
                return all;
            });
        };

        auto [server, client] = open_pair();
        expect(fatal(server != nullptr && client->valid()));
        auto atServer = collect(server);
        auto atClient = collect(client);
        // Both readers are waiting when the writes come: a relay forwarding a request, a daemon answering it.
        std::this_thread::sleep_for(std::chrono::milliseconds { 200 });
        expect(client->write("request\n").has_value());
        const std::string large(1 << 20, 'x');
        expect(server->write("reply\n" + large).has_value());
        expect(client->write("second request\n").has_value());
        client->shutdown_write();
        const std::string requests { atServer.get() };
        expect(requests == "request\nsecond request\n") << requests;
        server->shutdown_write();
        const std::string replies { atClient.get() };
        expect(replies.size() == large.size() + 6 && replies.starts_with("reply\n")) << replies.size();

        // A close from another thread ends a read that is waiting.
        auto [idle, peer] = open_pair();
        expect(fatal(idle != nullptr && peer->valid()));
        auto waiting = collect(idle);
        std::this_thread::sleep_for(std::chrono::milliseconds { 200 });
        idle->close();
        expect(waiting.get().empty());
        *finished = true;
    };

    "tokens are random hex"_test = [] {
        const std::string first { net::random_token() };
        const std::string second { net::random_token() };
        expect(first.size() == 32u && second.size() == 32u);
        expect(first != second);
        expect(std::ranges::all_of(first, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    };

    "connecting where nothing listens is an error"_test = [] {
        int port { 0 };
        {
            auto listener = net::Listener::listen_local();
            expect(fatal(listener.has_value()));
            port = listener->port();
        }
        expect(!net::Connection::connect_local(port).has_value());
    };

    return report();
}
