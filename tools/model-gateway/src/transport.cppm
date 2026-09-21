// A single POST-JSON operation over http:// or https://. See PROTOCOL.md
// "Transport" and README.md "Build form" for why http:// is handled by a
// small client of our own on top of mcpplibs::tinyhttps::Socket rather than
// mcpplibs::tinyhttps::HttpClient (which refuses every scheme but https).
export module mcppls.model.transport;

import std;

export namespace mcppls::model {

struct HttpResult {
    bool ok { false };             // a real HTTP response arrived (any status code)
    int status { 0 };
    std::string body;
    bool cancelled { false };      // *cancelled was observed; ok is always false when this is true
    std::string transportError;    // set when !ok: connect/timeout/malformed-response/unsupported-scheme
};

// `cancelled`, when non-null, is polled while waiting on I/O so a `cancel`
// notification can be honored promptly for the http:// path; the https://
// path (mcpplibs::tinyhttps::HttpClient has no mid-flight hook) only checks
// it before and after the single blocking call.
HttpResult post_json(const std::string& url, const std::map<std::string, std::string>& headers,
                      const std::string& body, int timeoutSeconds, const std::atomic<bool>* cancelled);

} // namespace mcppls::model
