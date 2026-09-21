// mcppls-model: the reference model gateway (PROTOCOL.md). A child process
// mcppls starts and speaks a line protocol with over stdio; it is the only
// part of mcppls that talks to the network.
import std;
import mcpplibs.tinyhttps;
import mcppls.model.gateway;

int main(int argc, char* argv[]) {
    // Winsock needs WSAStartup before any socket call; a no-op elsewhere.
    // Neither mcpplibs::tinyhttps::HttpClient nor this package's own transport
    // calls it, so it happens once, here.
    mcpplibs::tinyhttps::Socket::platform_init();
    const int status { mcppls::model::run(argc, argv) };
    mcpplibs::tinyhttps::Socket::platform_cleanup();
    return status;
}
