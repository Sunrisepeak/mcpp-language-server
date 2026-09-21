// mcppls.pack.lock ports fetch.py's `load_lock` and the platform lookups trim_clangd.py and
// assemble_payload.py made through it. The fixture here is a small stand-in for
// packaging/payload.lock.json, written to scratch rather than reading the real file: a unit test
// should not depend on the repository's current lock contents (see test_archive.cpp for why
// scratch lives beside the test binary rather than in the system temp).
import std;
import mcppls.testing;
import mcppls.base.path;
import mcppls.pack.lock;
import mcppls.platform.fs;

namespace lock = mcppls::pack::lock;
namespace base = mcppls::base;
namespace fs = mcppls::platform::fs;

namespace {

std::string scratch_dir() {
    const auto root = std::filesystem::current_path() / ".test-scratch"
                      / std::format("mcppls-lock-{}", std::random_device {}());
    std::filesystem::create_directories(root);
    return root.string();
}

constexpr std::string_view FIXTURE = R"JSON({
  "lock-version": 1,
  "clangd-version": "23.1.0",
  "libcxx-version": "23.1.0",
  "entries": {
    "clangd-linux": {
      "url": "https://example.invalid/clangd-linux-23.1.0.zip",
      "file": "clangd-linux-23.1.0.zip",
      "size": 12345,
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    },
    "llvm-project-src": {
      "url": "https://example.invalid/llvm-project-23.1.0.src.tar.xz",
      "file": "llvm-project-23.1.0.src.tar.xz",
      "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    }
  },
  "platforms": {
    "linux-x64": {
      "clangd": "clangd-linux",
      "kit": {
        "recipe": "libcxx-source",
        "source": "llvm-project-src",
        "target": "x86_64-unknown-linux-gnu"
      }
    }
  }
})JSON";

} // namespace

int main() {
    using namespace mcppls::testing;

    const std::string work { scratch_dir() };
    const std::string lockPath { base::join_path(work, "payload.lock.json") };
    expect(fs::write_file(lockPath, FIXTURE).has_value());

    "a well-formed lock parses into entries and platforms"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        expect(loaded->clangdVersion == "23.1.0");
        expect(loaded->libcxxVersion == "23.1.0");
        expect(loaded->entries.size() == 2);
        expect(loaded->platforms.size() == 1);
    };

    "a known entry is returned by name"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        auto found = lock::entry(*loaded, "clangd-linux");
        expect(fatal(found.has_value()));
        if (!found) return;
        expect(found->file == "clangd-linux-23.1.0.zip");
        expect(found->url == "https://example.invalid/clangd-linux-23.1.0.zip");
        expect(found->sha256 == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        expect(found->size.has_value());
        expect(found->size.value_or(0) == 12345);
    };

    "an entry without a recorded size has none"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        auto found = lock::entry(*loaded, "llvm-project-src");
        expect(fatal(found.has_value()));
        if (!found) return;
        expect(!found->size.has_value());
    };

    "an unknown entry is an error naming the known ones"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        auto found = lock::entry(*loaded, "does-not-exist");
        expect(!found.has_value());
        if (found) return;
        expect(found.error().message.contains("clangd-linux"));
    };

    "a known platform carries its clangd entry and kit source"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        auto found = lock::platform(*loaded, "linux-x64");
        expect(fatal(found.has_value()));
        if (!found) return;
        expect(found->clangd == "clangd-linux");
        expect(found->kit.recipe == "libcxx-source");
        expect(found->kit.source == "llvm-project-src");
        expect(found->kit.target == "x86_64-unknown-linux-gnu");
    };

    "an unknown platform is an error"_test = [&] {
        auto loaded = lock::load(lockPath);
        expect(fatal(loaded.has_value()));
        if (!loaded) return;
        auto found = lock::platform(*loaded, "win32-x64");
        expect(!found.has_value());
    };

    "a missing lock file is an error rather than an empty result"_test = [&] {
        auto loaded = lock::load(base::join_path(work, "there-is-no-such-file.json"));
        expect(!loaded.has_value());
    };

    "a lock that is not valid JSON is an error"_test = [&] {
        const std::string badPath { base::join_path(work, "bad.json") };
        expect(fs::write_file(badPath, "{ not json").has_value());
        auto loaded = lock::load(badPath);
        expect(!loaded.has_value());
    };

    std::filesystem::remove_all(work);
    return report();
}
