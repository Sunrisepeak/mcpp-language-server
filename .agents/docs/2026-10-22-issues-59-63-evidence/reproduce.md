# #59 / #63 复现与核验

以下在 Linux/bash 执行。使用新 worktree、独立 workspace/cache，不污染当前开发目录。性能门槛不改；冷/暖/编辑共用 cache-a，faults 独立 cache-b。硬件不同可得到不同结果，应记录实际失败。

## 1. 固定源码、工具与 payload

历史归档对应 `5e19005d91af56cd5fd18b05225fc8b6c9d8b538`，恢复补修检查点为 `dc12357`。下面在待审提交的 checkout 中执行，默认固定其完整 HEAD；也可用环境变量 `SOURCE_COMMIT` 指定复现版本，未提交改动不会被带入。CI/nightly 的 UX 使用 release server 和 release runner，所以默认构建 release；先前本地 dev 性能数据单独保留，不能混称 release 验收。需要 mcpp 2026.10.21.1、LLVM 22.1.8、GCC 16.1.0、git、gh、jq、taskset。fixture 的项目 launcher 依赖按 `.github/workflows/ci.yml` 的 UX job 安装；server producer 单独固定。

```bash
set -euo pipefail
source_commit=$(git rev-parse "${SOURCE_COMMIT:-HEAD}")
build_profile=release
audit_dir="$PWD/.payload-cache/repro59-63-$(date +%Y%m%dT%H%M%S)"
mkdir -p "$audit_dir"
git worktree add --detach "$audit_dir/checkout" "$source_commit"
cd "$audit_dir/checkout"
mcpp --version
mcpp build --profile "$build_profile" --jobs 4

# fresh worktree 只构建一个 profile；拒绝在多个旧 bucket 中随意挑选。
choose_binary() {
  local name="$1"
  local -a paths
  mapfile -t paths < <(rg --files --no-ignore target | rg "/bin/$name$")
  test "${#paths[@]}" -eq 1
  realpath "${paths[0]}"
}
server=$(choose_binary mcppls)
runner=$(choose_binary mcppls-conformance)

gh release download v0.0.16 --repo Sunrisepeak/mcpp-language-server   --pattern payload-linux-x64.tar.gz --pattern SHA256SUMS   --dir "$audit_dir/released"
(
  cd "$audit_dir/released"
  sha256sum --ignore-missing -c SHA256SUMS
  tar -xzf payload-linux-x64.tar.gz
)
payload="$audit_dir/released/payload"
producer="$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.9.30.2/bin/mcpp"
test -x "$producer"
"$producer" --version
sha256sum "$server" "$runner" "$payload/clangd/bin/clangd" > "$audit_dir/binaries.sha256"
printf '%s\n' "$source_commit" "$build_profile" > "$audit_dir/source-profile.txt"
lscpu -e=CPU,CORE,SOCKET,ONLINE > "$audit_dir/topology.txt"
```

若历史 producer 未安装，先安装该版本并填写其真实绝对路径，不用 PATH 上另一个版本代替。根源码的构建工具与 fixture prepare 的 launcher 不必等于 server producer，应分别记录版本。官方 payload 解压后只取其中的 .4 engine/kit，`--server` 使用上面编译出的待审版本。

## 2. 单测与目标选择回归

```bash
MCPPLS_CONFORMANCE="$runner" mcpp test --profile "$build_profile"
python3 docs/specs/tools/validate.py

export CONFORMANCE_CLANGXX="$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++"
test -x "$CONFORMANCE_CLANGXX"
for case in mcpp-target-selection mcpp-target-live mcpp-target-race   mcpp-target-windows mcpp-target-provider-excluded mcpp-producer-identity   mcpp-producer-file-identity mcpp-polling-watch-inputs untrusted; do
  "$runner" run --server "$server" --payload "$payload"     --fixture "conformance/fixtures/$case" --timeout 60     --measure "$audit_dir/$case.json"
done
```

dev 验证另用 fresh worktree，将第 1 步 `build_profile` 设为 `dev`，并重新选择该 worktree 唯一的 runner；不要交叉使用不同 profile 的 runner/server。历史 dev 崩溃定位使用对应历史提交和 dev profile，不用 release 重放覆盖。

完整目标配置矩阵是 `conformance/fixtures/mcpp-target-*`、`mcpp-producer-*`；polling 路径再对 identity/target-selection/emit-watch 添加 `--no-dynamic-watch`。回归检查实际 producer argv、模型身份、目标状态、过期结果、CDB fallback、provider 排除和生命周期；仅看版本字符串不够。CDB publication 与 write-failure 的 native 对照位于 `tests/test_server.cpp`，runner drain 位于 `tests/test_conformance.cpp`。

## 3. 原始性能链路

仅复制 fixture 并增加固定 producer 路径，不改 prepare pin、checks、预算和阶段时长。

```bash
for name in mcpp xlings; do
  cp -R "conformance/fixtures/ux-$name" "$audit_dir/fixture-$name"
  jq --arg producer "$producer"     '.["server-arguments"] += ["--mcpp", $producer]'     "$audit_dir/fixture-$name/scenario.json" > "$audit_dir/fixture-$name/fixed.json"
  mv "$audit_dir/fixture-$name/fixed.json" "$audit_dir/fixture-$name/scenario.json"
done

run_stage() {
  local name="$1" cpus="$2" stage="$3" cache="$4" code
  local -a warm=()
  if [[ "$stage" != cold && "$stage" != faults ]]; then warm=(--expect-warm); fi
  mkdir -p "$audit_dir/$name"
  sha256sum "$server" "$runner" > "$audit_dir/$name/$stage-binaries.sha256"
  if taskset -c "$cpus" "$runner" run --server "$server" --payload "$payload"     --fixture "$audit_dir/fixture-$name" --workspace-dir "$audit_dir/$name/work"     --cache-dir "$audit_dir/$name/$cache" --stage "$stage" "${warm[@]}"     --timeout 900 --verbose --measure "$audit_dir/$name/$stage.json"     > "$audit_dir/$name/$stage.log" 2>&1; then code=0; else code=$?; fi
  printf '%s %s\n' "$stage" "$code" >> "$audit_dir/$name/exits.txt"
}
for stage in cold warm edits faults; do
  cache=cache-a; [[ "$stage" != faults ]] || cache=cache-b
  run_stage mcpp 4-5 "$stage" "$cache"
done
for stage in cold warm edits faults long; do
  cache=cache-a; [[ "$stage" != faults ]] || cache=cache-b
  run_stage xlings 12-15 "$stage" "$cache"
done
cat "$audit_dir/mcpp/exits.txt" "$audit_dir/xlings/exits.txt"
```

CPU 编号是本机记录，不保证其他主机可用或拓扑相同。变更亲和性必须标成另一组实验。mcpp 对应 nightly 两核阶段；xlings 包含 nightly round 1 的全部五阶段，另保留本机 12–15 亲和性。它们不是对 GitHub 所有轮次/平台的替代。不要以增加核数、缩短 warm/edits/600 s idle、换 cache lineage、去掉失败 check 或调整门槛代替原场景。某阶段失败仍继续后续阶段；`exits.txt` 的非零值都算失败，缺少 measure 时也不能报通过。

读取结果时区分：U15 原节点间隔与真实模块无活动；U8 实际构建数、旧有效 key 和混合闭包；U13 有限工作期与随后固定 600 s 测量。先前失败及其完整原始记录由 [索引](raw-index.tsv) 保留。

## 4. runner 139 的独立定位

上述检查点确实可能崩溃。使用新 workspace/cache 重放同一个 cold，不覆盖失败目录；GDB 正常退出只是未复现，不证明已修。

```bash
taskset -c 6-7 gdb --args "$runner" run --server "$server" --payload "$payload"   --fixture "$audit_dir/fixture-mcpp" --workspace-dir "$audit_dir/gdb-work"   --cache-dir "$audit_dir/gdb-cache" --stage cold --timeout 900 --verbose   --measure "$audit_dir/gdb-cold.json"
```

GDB 内先 `set disable-randomization off`，再 `run`。只有收到 SIGSEGV 时才执行 `thread apply all bt` 和 `generate-core-file`；正常退出后不要将无 inferior 的 gcore 错误当成 runner 失败。当前捕获的原始 frame 是合法 429,463 B completion、深度 7；core 证明当时未耗尽 reader 栈，不能据此随意增加栈或放松 JSON 限制。

完整 core 保留本地，不进 PR。复现产物目录为 `$audit_dir`；需要归档时保留所有阶段、exit code、版本/哈希和拓扑，而不是只选通过结果。

## 5. home 内缓存的故障注入回归

默认报告会将 home 前缀显示为 `~`，不能把这个展示字符串当作文件路径。下面仍运行原始完整 fixture，不改断言。若本项目 checkout 位于 home 内，`$audit_dir` 也在 home 内，可直接覆盖 Windows 暴露的脱敏边界；不需要修改 HOME。前提仍为第 1 节已固定的二进制与 payload。

```bash
mkdir -p "$audit_dir/report-cache-check"
CONFORMANCE_CLANGXX="$HOME/.mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++" \
  "$runner" run --server "$server" --payload "$payload" \
  --fixture conformance/fixtures/mcpp-target-mismatch \
  --cache-dir "$audit_dir/report-cache-check/cache" --timeout 60 \
  --measure "$audit_dir/report-cache-check/measure.json"
```

旧 helper 的 C1 会报告无法打开 `~/.../model.mcpp.json`；修复后应实际注入缓存、拒绝不一致模型并重新取得 i686 结果，原完整 fixture 零失败。只有截到 C1 的旧诊断对照不能代替这份完整验收。

## 6. 平台测试工具先确认能启动

在目标平台运行同一组构建产物，先执行以下命令，再运行完整 fixture；Windows 加 `.exe` 后缀。这能在几秒内暴露 mock 的进程启动问题，不能替代后续原 conformance 断言。旧 macOS cross artifact 在进入 `main` 前分配全局字符串、早于 allocator 初始化，导致真实 producer 模拟从未运行；修复后应返回正常协议 JSON 与退出码 0。

```bash
"${runner%/*}/mcppls-mock-mcpp" --protocol-version
```

CI 同时检查原生 dev/release 和 payload 内打包的 mock。旧失败的 artifact SHA、初始化顺序及必要反汇编另保存在本地 `.payload-cache/59-63-macos-mock-startup/`；Linux 上的静态 Mach-O 分析不能代替 macOS 原生运行。

## 7. 后台补全节奏与逐词证据

完成对应 profile 的 `mcpp build` 后，运行真实 Workspace 回归和 runner 的 fake-LSP 回归：

```bash
MCPPLS_CONFORMANCE="$runner" mcpp test --profile "$build_profile" answer_routing
MCPPLS_CONFORMANCE="$runner" mcpp test --profile "$build_profile" conformance
```

新增 K1 回归保留四次引擎请求：原请求等待 1100 ms 后已缓存先答，再等 150 ms 给真实引擎答案。报告必须包含完整延迟，下一请求在 750 ms 时仍等待引擎；最终完整列表只能由引擎提供。旧产品加同一测试会有 4 个断言失败，修复后通过。对照应在独立 worktree 中将新增测试应用到 `0e2a146`，不要在正进行验收的构建中来回替换源码。

Windows 原生环境仍运行完整 `ux-modules-windows` fixture，以原 `askedAsTypedPerWord <= 2.0` 门槛验收。首次 `--measure` 的 `wordTrace` 可逐词核对 first/incomplete、请求版本和位置、原始 `isIncomplete`、候选数量与响应延迟；超过 256 条的数量明确写入 `wordTraceDropped`。CI 的 `conformance-measures-win32-x64-*` artifact 保存首次结果，原失败不会被 verbose 重跑覆盖。不得通过改变 trigger context、输入节奏或门槛使旧失败消失。
