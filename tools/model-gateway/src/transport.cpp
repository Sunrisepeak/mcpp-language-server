module mcppls.model.transport;

import std;
import mcpplibs.tinyhttps;

namespace mcppls::model {

namespace {

struct ParsedUrl {
    bool https { false };
    std::string host;
    int port { 0 };
    std::string path;
};

std::optional<ParsedUrl> parse_url(const std::string& url) {
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return std::nullopt;
    const std::string scheme { url.substr(0, schemeEnd) };

    ParsedUrl parsed;
    if (scheme == "https") parsed.https = true;
    else if (scheme != "http") return std::nullopt;

    const std::string rest { url.substr(schemeEnd + 3) };
    const auto pathStart = rest.find('/');
    const std::string authority { pathStart == std::string::npos ? rest : rest.substr(0, pathStart) };
    parsed.path = pathStart == std::string::npos ? std::string { "/" } : rest.substr(pathStart);
    if (parsed.path.empty()) parsed.path = "/";

    const auto colon = authority.find(':');
    if (colon == std::string::npos) {
        parsed.host = authority;
        parsed.port = parsed.https ? 443 : 80;
    } else {
        parsed.host = authority.substr(0, colon);
        try {
            parsed.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (parsed.host.empty() || parsed.port <= 0) return std::nullopt;
    return parsed;
}

// Polls in short slices instead of one long wait, so a `cancel` notification
// observed on another thread interrupts a wait quickly instead of only
// between waits.
inline constexpr int POLL_SLICE_MS { 100 };

enum class Wait { ready, timed_out, cancelled_out };

Wait wait_cancellable(mcpplibs::tinyhttps::Socket& socket, int totalTimeoutMs, bool forRead, const std::atomic<bool>* cancelled) {
    int elapsed { 0 };
    while (elapsed < totalTimeoutMs) {
        if (cancelled && cancelled->load()) return Wait::cancelled_out;
        const int slice { std::min(POLL_SLICE_MS, totalTimeoutMs - elapsed) };
        const bool ready { forRead ? socket.wait_readable(slice) : socket.wait_writable(slice) };
        if (ready) return Wait::ready;
        elapsed += slice;
    }
    return (cancelled && cancelled->load()) ? Wait::cancelled_out : Wait::timed_out;
}

// A minimal, blocking HTTP/1.1 POST over a plain (non-TLS) socket: enough to
// reach a local OpenAI-compatible endpoint and this package's own tests.
// Always sends `Connection: close`, so the response body can be read to
// end-of-stream without implementing chunked-transfer decoding.
HttpResult post_plain(const ParsedUrl& target, const std::map<std::string, std::string>& headers,
                       const std::string& body, int timeoutSeconds, const std::atomic<bool>* cancelled) {
    HttpResult result;
    const int timeoutMs { std::max(1, timeoutSeconds) * 1000 };

    if (cancelled && cancelled->load()) {
        result.cancelled = true;
        return result;
    }

    mcpplibs::tinyhttps::Socket socket;
    if (!socket.connect(target.host.c_str(), target.port, timeoutMs)) {
        result.transportError = "connect failed: " + target.host + ":" + std::to_string(target.port);
        return result;
    }

    std::string request;
    request += "POST " + target.path + " HTTP/1.1\r\n";
    request += "Host: " + target.host + (target.port == 80 ? std::string {} : ":" + std::to_string(target.port)) + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n";
    for (const auto& [key, value] : headers) request += key + ": " + value + "\r\n";
    request += "\r\n";
    request += body;

    std::size_t sent { 0 };
    while (sent < request.size()) {
        switch (wait_cancellable(socket, timeoutMs, false, cancelled)) {
            case Wait::cancelled_out: result.cancelled = true; return result;
            case Wait::timed_out: result.transportError = "write timed out"; return result;
            case Wait::ready: break;
        }
        const int written { socket.write(request.data() + sent, static_cast<int>(request.size() - sent)) };
        if (written <= 0) {
            result.transportError = "write failed";
            return result;
        }
        sent += static_cast<std::size_t>(written);
    }

    std::string response;
    char buffer[4096];
    bool stalled { false };
    while (!stalled) {
        switch (wait_cancellable(socket, timeoutMs, true, cancelled)) {
            case Wait::cancelled_out: result.cancelled = true; return result;
            case Wait::timed_out:
                if (response.empty()) {
                    result.transportError = "read timed out";
                    return result;
                }
                stalled = true; // a stall after some bytes: treat what arrived as the whole response
                continue;
            case Wait::ready: break;
        }
        const int received { socket.read(buffer, sizeof(buffer)) };
        if (received <= 0) break; // the peer closed, as asked via Connection: close
        response.append(buffer, static_cast<std::size_t>(received));
    }

    const auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        result.transportError = "malformed response: no header terminator";
        return result;
    }
    const std::string statusLine { response.substr(0, response.find("\r\n")) };
    const auto firstSpace = statusLine.find(' ');
    if (firstSpace == std::string::npos) {
        result.transportError = "malformed status line";
        return result;
    }
    const auto secondSpace = statusLine.find(' ', firstSpace + 1);
    const std::string codeText { secondSpace == std::string::npos
        ? statusLine.substr(firstSpace + 1)
        : statusLine.substr(firstSpace + 1, secondSpace - firstSpace - 1) };
    try {
        result.status = std::stoi(codeText);
    } catch (...) {
        result.transportError = "malformed status code";
        return result;
    }

    result.body = response.substr(headerEnd + 4);
    result.ok = true;
    return result;
}

HttpResult post_https(const std::string& url, const std::map<std::string, std::string>& headers,
                       const std::string& body, int timeoutSeconds, const std::atomic<bool>* cancelled) {
    HttpResult result;
    if (cancelled && cancelled->load()) {
        result.cancelled = true;
        return result;
    }

    mcpplibs::tinyhttps::HttpClientConfig clientConfig;
    clientConfig.connectTimeoutMs = std::max(1, timeoutSeconds) * 1000;
    clientConfig.readTimeoutMs = std::max(1, timeoutSeconds) * 1000;
    clientConfig.keepAlive = false;
    mcpplibs::tinyhttps::HttpClient client { clientConfig };

    mcpplibs::tinyhttps::HttpRequest request;
    request.method = mcpplibs::tinyhttps::Method::POST;
    request.url = url;
    request.body = body;
    request.headers = headers;

    const auto response = client.send(request);

    // Best effort only: tinyhttps::HttpClient::send offers no mid-flight
    // cancellation hook (see README.md "Build form").
    if (cancelled && cancelled->load()) {
        result.cancelled = true;
        return result;
    }
    if (response.statusCode == 0) {
        result.transportError = response.statusText.empty() ? "transport error" : response.statusText;
        return result;
    }
    result.ok = true;
    result.status = response.statusCode;
    result.body = response.body;
    return result;
}

} // namespace

HttpResult post_json(const std::string& url, const std::map<std::string, std::string>& headers,
                      const std::string& body, int timeoutSeconds, const std::atomic<bool>* cancelled) {
    auto target = parse_url(url);
    if (!target) {
        HttpResult result;
        result.transportError = "unsupported or malformed URL: " + url;
        return result;
    }
    if (target->https) return post_https(url, headers, body, timeoutSeconds, cancelled);
    return post_plain(*target, headers, body, timeoutSeconds, cancelled);
}

} // namespace mcppls::model
