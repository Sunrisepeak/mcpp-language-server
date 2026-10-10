import std;
import nlohmann.json;
import mcppls.testing;
import mcppls.base.engineidentity;

using Json = nlohmann::json;
namespace base = mcppls::base;

int main() {
    using namespace mcppls::testing;
    const Json sample { { "engine-version", "23.1.0-mcppls.1" }, { "llvm-base-version", "23.1.0" },
                        { "llvm-commit", std::string(40, 'a') }, { "fork-commit", std::string(40, 'b') },
                        { "patch-series-sha256", std::string(64, 'c') }, { "platform", "linux-x64" },
                        { "sha256", std::string(64, 'd') }, { "features", Json::array({ "semantic-tokens-range" }) } };
    "an engine identity binds its platform version base source and bytes"_test = [&] {
        const auto identity = base::decode_engine_identity(sample);
        expect(fatal(identity.has_value()));
        if (!identity) return;
        expect(!base::engine_identity_problem(*identity, "23.1.0-mcppls.1", "linux-x64", std::string(64, 'd')));
        expect(base::engine_identity_problem(*identity, "23.1.0", "linux-x64", std::string(64, 'd')).has_value());
        expect(base::engine_identity_problem(*identity, identity->version, "win32-x64", std::string(64, 'd')).has_value());
        expect(base::engine_identity_problem(*identity, identity->version, identity->platform, std::string(64, 'e')).has_value());
        auto changed = *identity;
        changed.llvmCommit = std::string(40, '0');
        expect(base::engine_identity_problem(changed, changed.version, changed.platform, changed.sha256).has_value());
        changed = *identity;
        changed.llvmBase = "23.1.0.1";
        expect(base::engine_identity_problem(changed, changed.version, changed.platform, changed.sha256).has_value());
        changed = *identity;
        changed.features.clear();
        expect(!base::engine_identity_problem(changed, changed.version, changed.platform, changed.sha256)) << "a version suffix does not grant a capability";
    };
    "missing or mistyped immutable fields cannot become an engine identity"_test = [&] {
        for (const auto& field : { "engine-version", "llvm-base-version", "llvm-commit", "fork-commit", "patch-series-sha256", "platform", "sha256", "features" }) {
            auto changed = sample;
            changed.erase(field);
            expect(!base::decode_engine_identity(changed));
            changed = sample;
            changed[field] = 42;
            expect(!base::decode_engine_identity(changed));
        }
        auto changed = sample;
        changed["features"] = Json::array({ 42 });
        expect(!base::decode_engine_identity(changed));
    };
}
