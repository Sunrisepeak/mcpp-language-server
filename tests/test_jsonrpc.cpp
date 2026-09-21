import std;
import mcppls.testing;
import nlohmann.json;
import mcppls.lsp.jsonrpc;
import mcppls.lsp.protocol;

using namespace mcppls::lsp;

int main() {
    using namespace mcppls::testing;

    "frames split across feeds"_test = [] {
        const std::string frame { encode_frame(make_request(1, "initialize", Json { { "rootUri", nullptr } })) };
        FrameReader reader;
        reader.feed(frame.substr(0, 10));
        expect(!reader.next().has_value());
        reader.feed(frame.substr(10, 20));
        expect(!reader.next().has_value());
        reader.feed(frame.substr(30));
        auto message = reader.next();
        expect(fatal(message.has_value() && message->has_value()));
        expect((**message)["method"] == "initialize");
        expect(kind_of(**message) == Kind::request);
        expect(!reader.next().has_value());
    };

    "two frames in one feed and extra headers"_test = [] {
        std::string bytes { encode_frame(make_notification("initialized", Json::object())) };
        const std::string body { R"({"jsonrpc":"2.0","id":"a","result":null})" };
        bytes += std::format("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncontent-length: {}\r\n\r\n{}", body.size(), body);
        FrameReader reader;
        reader.feed(bytes);
        auto first = reader.next();
        auto second = reader.next();
        expect(fatal(first.has_value() && first->has_value() && second.has_value() && second->has_value()));
        expect(kind_of(**first) == Kind::notification);
        expect(kind_of(**second) == Kind::response);
        expect((**second)["id"] == "a");
    };

    "a bad header is reported and skipped"_test = [] {
        FrameReader reader;
        reader.feed("Bogus: 1\r\n\r\n");
        reader.feed(encode_frame(make_notification("exit", nullptr)));
        auto bad = reader.next();
        expect(fatal(bad.has_value()));
        expect(!bad->has_value());
        auto good = reader.next();
        expect(fatal(good.has_value() && good->has_value()));
        expect((**good)["method"] == "exit");
    };

    "Unicode survives framing and lengths are bytes"_test = [] {
        const Json message = make_notification("x", Json { { "text", "h\xC3\xA9llo \xF0\x9F\x98\x80" } });
        const std::string frame { encode_frame(message) };
        FrameReader reader;
        reader.feed(frame);
        auto decoded = reader.next();
        expect(fatal(decoded.has_value() && decoded->has_value()));
        expect((**decoded)["params"]["text"] == "h\xC3\xA9llo \xF0\x9F\x98\x80");
        const auto parsed = parse(R"({"s":"é😀"})");
        expect(fatal(parsed.has_value()));
        expect((*parsed)["s"] == "\xC3\xA9\xF0\x9F\x98\x80");
    };

    "invalid UTF-8 never throws when serialized"_test = [] {
        const Json value { { "text", std::string { "\xFF\xFE" } } };
        expect(!dump(value).empty());
    };

    "integers stay integers"_test = [] {
        const auto parsed = parse(R"({"id":42,"x":1.5})");
        expect(fatal(parsed.has_value()));
        expect(int_at(*parsed, "id") == std::optional<std::int64_t> { 42 });
        expect(!int_at(*parsed, "x").has_value());
        expect(!parse("{\"a\":}").has_value());
    };

    "message constructors"_test = [] {
        const Json error = make_error(7, METHOD_NOT_FOUND, "nope");
        expect(kind_of(error) == Kind::response);
        expect(error["error"]["code"] == -32601);
        expect(kind_of(Json::array()) == Kind::invalid);
        expect(find_path(Json { { "a", { { "b", 3 } } } }, { "a", "b" }) != nullptr);
        expect(find_path(Json { { "a", 1 } }, { "a", "b" }) == nullptr);
    };

    "generated protocol table"_test = [] {
        const auto* definition = find_method(method::TEXT_DOCUMENT_DEFINITION);
        expect(fatal(definition != nullptr));
        expect(definition->isRequest);
        expect(definition->direction == MessageDirection::client_to_server);
        expect(definition->serverCapability == "definitionProvider");
        const auto* diagnostics = find_method("textDocument/publishDiagnostics");
        expect(fatal(diagnostics != nullptr));
        expect(!diagnostics->isRequest);
        expect(find_method("cxxModules/status") == nullptr);
        expect(PROTOCOL_VERSION == "3.18.0");
        expect(static_cast<int>(SymbolKind::module_) == 2_i);
        expect(markup_kind::MARKDOWN == "markdown");
        expect(std::ranges::is_sorted(methods(), {}, &MethodInfo::name));
    };

    return report();
}
