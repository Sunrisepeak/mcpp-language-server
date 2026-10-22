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

## 8. SC4：先完成引擎工作，再比较缓存

`MCPPLS_CONFORMANCE="$runner" mcpp test --profile "$build_profile" conformance` 包含确定性回归：fake LSP 先暴露一份短 owned std copy，并报告 `filesAwaitingDiagnostics > 0`；后续 report 才补齐同字节副本并报告工作结束。旧 runner 不等工作结束，立刻报新增 BMI；新 runner 应接受补齐的副本。不同大小的新 BMI、缺失工作状态、永远未完成三种情况仍必须失败，cold 也要 drain 后才能建立 warm 的基线。

真实引擎则运行原完整 `conformance/fixtures/timing`，同一 workspace/cache 依次 cold 与 `--expect-warm`，保持导航门槛 15/5 秒。`SC4-std-reused.measure.cacheDrain` 保留等待时长和末态；首导航仍是最初回答的时间，不能由后续 drain 改写。CI 的 `timing-<platform>` artifact 同时保留 cold/warm JSON；首次失败与后续修补结果分别记录。


## 9. 只读接口符号不能只改项目引用

原 `conformance/fixtures/mcpp-build-programs` 的 R1 从 `build.mcpp` 中的 `cxxflag` 引用重命名，必须返回 `-32803`。设置 `MCPPLS_BUILD_PROGRAMS_MCPP` 为支持构建程序数据库的真实 mcpp，按前述 runner/server/payload 命令运行该完整 fixture；不要更改 R1 的预期。Windows nightly 39712494651 曾返回两处项目修改而遗漏只读接口，属于 mcppls 的保护缺口；不能据此断言上游遗漏的具体原因。

`mcpp test answer_routing` 的 HeldCore 对照在旧产品 `9f84df0` 上应用同一份测试：rename 会自动返回真实的项目 WorkspaceEdit，definition/declaration 则独立控制。修复后只读 definition 或 declaration（含 Location / Location[] / LocationLink[]）均拒绝，项目/可写库及闭合文档仍可成功。两阶段共用原始 10 秒预算；取消、晚回调、文档/模型变化、缺失或无效位置均不得继续产生修改，全同步回调也需成功且只回答一次。保留旧产品失败日志、测试及产品 SHA，再对固定产品执行 dev/release 全 workspace 和真实 LSP；不能用后来通过覆盖旧失败。


## 10. CPU 采样前先确认实际 CPU 工作完成

`mcpp test process` 和 `mcpp test process --profile release` 保留原 `busy CPU > 0.5 s`、`idle CPU < 0.2 s` 断言。旧测试固定等 2 秒 wall time，不能保证获得 0.5 秒 CPU；PR/nightly 的 macOS release 首轮分别只读到 0.09/0.08 秒增量。它们仍是真失败，不能据此排除产品 sampler 的问题。

子进程 READY 后由父进程先采样，再发送 START；子进程用独立 native process CPU 时钟确认累计至少 0.75 秒并发送 DONE，保持进程存活供父进程再次采样。父进程的工作与握手共用原 30 秒期限；sampler 每次调用另有既有界限（macOS 的 ps 为 2 秒），清理也可能越过工作期限，迟到采样仍失败。记录 native work 和两组 before/after；独立工作未完成、时钟不可用或 subject 读数不足均失败。Linux oracle 直接 native clock syscall，macOS 直接 libSystem 的 Darwin CPU clock（ID 12），不使用 subject `/proc`/`ps` 读取器；当前 OpenKAL 的 `std::clock` 实际走 monotonic，不能充当 CPU oracle。该改动只涉及测试，不更改产品 sampler 或性能输入/预算。

## 11. 发送队列的编码与快照

运行两种 profile 的完整 `mcpp test process`。原 nonreader 测试仍发送 64 条、每条 64 KiB，要求全部 send 在 2 秒内返回、stop 在 10 秒内结束；现在同时记录实际耗时。Release 39732244483 的原 macOS dev 失败日志保存在 `.payload-cache/59-63-delivery/release-macos-dev-first-failure.log`，不得以另一次通过或放宽时限替代它。

双 framing 的真实 echo 子进程回归检查 FIFO、发送后修改或销毁原 JSON 的快照独立性、UTF-8 replacement；退出的 peer 必须拒绝继续发送并完成 stop。新 writer 在锁外使用既有 dump/encode_frame，编码异常必须转换成连接失败，不能逃出线程；没有添加生产测试 hook 或伪造损坏 JSON 来触发 OOM。该异常隔离有代码审查，不能宣称已经完成原生 OOM 故障注入。
