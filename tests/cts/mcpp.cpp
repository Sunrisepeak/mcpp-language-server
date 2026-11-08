// Projects mcpp builds: the build description it emits (S1) and its stages, the build programs,
// generated modules, the watch of its inputs, its toolchains and the producer's identity.
import mcppls.cts;

using namespace mcpplibs::mtest;
using namespace mcppls::cts;

namespace {

Fixture build_discovery_off{"build-discovery-off", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture build_program_broken{"build-program-broken", on{"linux-x64", "darwin-arm64"}};
Fixture build_program_gated{"build-program-gated", on{"linux-x64", "darwin-arm64"}};
Fixture build_program_manual{"build-program-manual", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module{"generated-module", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_vscode{"generated-module@vscode", on{"linux-x64"}};
Fixture generated_module_negotiated{"generated-module-negotiated", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_old_mcpp{"generated-module-old-mcpp", on{"linux-x64", "darwin-arm64"}};
Fixture generated_module_old_mcpp_neovim{"generated-module-old-mcpp@neovim", on{"linux-x64"}};
Fixture mcpp_all_cppm{"mcpp-all-cppm", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_build_programs{"mcpp-build-programs", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_build_programs_cross{"mcpp-build-programs-cross", on{"linux-x64"}};
Fixture mcpp_emit{"mcpp-emit", on{"linux-x64"}};
Fixture mcpp_emit_broken{"mcpp-emit-broken", on{"linux-x64"}};
Fixture mcpp_emit_edits{"mcpp-emit-edits", on{"linux-x64"}};
Fixture mcpp_emit_hang{"mcpp-emit-hang", on{"linux-x64", "win32-x64"}};
Fixture mcpp_emit_needs_download{"mcpp-emit-needs-download", on{"linux-x64"}};
Fixture mcpp_emit_package_std{"mcpp-emit-package-std", on{"linux-x64"}};
Fixture mcpp_emit_partial{"mcpp-emit-partial", on{"linux-x64"}};
Fixture mcpp_emit_partial_download{"mcpp-emit-partial-download", on{"linux-x64"}};
Fixture mcpp_emit_provisioned{"mcpp-emit-provisioned", on{"linux-x64"}};
Fixture mcpp_emit_unavailable{"mcpp-emit-unavailable", on{"linux-x64"}};
Fixture mcpp_emit_wait{"mcpp-emit-wait", on{"linux-x64", "darwin-arm64", "win32-x64"}};
// The second session plans with the model the first one cached, before the producer has said
// anything, and only then confirms it (build description design 4.1).
Fixture mcpp_emit_warm{"mcpp-emit@warm", on{"linux-x64"}};
Fixture mcpp_emit_watch{"mcpp-emit-watch", on{"linux-x64"}};
Fixture mcpp_emit_watch_polling{"mcpp-emit-watch@polling", on{"linux-x64"}};
Fixture mcpp_gcc{"mcpp-gcc", on{"linux-x64"}};
Fixture mcpp_gcc_plain{"mcpp-gcc@plain", on{"linux-x64"}};
Fixture mcpp_llvm{"mcpp-llvm", on{"linux-x64", "darwin-arm64"}};
Fixture mcpp_llvm_linked{"mcpp-llvm@linked", on{"linux-x64"}};
Fixture mcpp_llvm_msvc{"mcpp-llvm-msvc", on{"win32-x64"}};
Fixture mcpp_msvc{"mcpp-msvc", on{"win32-x64"}};
Fixture mcpp_partition_definition{"mcpp-partition-definition", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_polling_watch_inputs_polling{"mcpp-polling-watch-inputs@polling", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_file_identity{"mcpp-producer-file-identity", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_identity{"mcpp-producer-identity", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_producer_identity_polling{"mcpp-producer-identity@polling", on{"linux-x64"}};
Fixture mcpp_producer_relative_path{"mcpp-producer-relative-path", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_rules_generated{"mcpp-rules-generated", on{"linux-x64"}};
Fixture mcpp_split{"mcpp-split", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_split_gcc{"mcpp-split-gcc", on{"linux-x64"}};
Fixture mcpp_split_msvc{"mcpp-split-msvc", on{"win32-x64"}};
Fixture mcpp_watch{"mcpp-watch", on{"linux-x64", "darwin-arm64", "win32-x64"}};
Fixture mcpp_watch_polling{"mcpp-watch@polling", on{"linux-x64"}};
Fixture mcpp_watched_source_structure{"mcpp-watched-source-structure", on{"linux-x64", "linux-arm64", "darwin-arm64", "win32-x64"}};
Fixture s1_two_sets{"s1-two-sets", on{"linux-x64"}};

}  // namespace
