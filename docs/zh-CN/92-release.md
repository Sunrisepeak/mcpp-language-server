# 发布 release

[English](../92-release.md) | **简体中文**

> release 发布在本仓库的 GitHub release 页面。release 一发布，Open VSX 会自动跟上；VS Code Marketplace 在本地验证过 release 的 VSIX 之后手动上传；xlings 索引还不是发布渠道（见下文）。每个渠道用到的标识符见 [91-naming.md](91-naming.md)。

## 版本号

产品的版本号只存在 `mcpp.toml` 里，别处没有第二份。由它派生出的一切，都靠一条命令写入或核对：

```bash
mcpp run -p devtools -- version --check          # 检查各处版本一致
mcpp run -p devtools -- version --set 0.0.2
```

这条命令让这些地方保持一致：`mcpp.toml`、运行中的二进制文件报出的常量（`modules/base/src/version.cppm`）、每个编辑器插件的版本号（VS Code、Zed、CLion，以及 Claude Code 插件和它在 marketplace 里的条目），以及服务端声明的 clangd 和 kit 版本与 payload 实际构建所用版本的对应关系。

版本号是三段式的语义化版本 `MAJOR.MINOR.PATCH`，从 `0.0.1` 开始，每个插件都原样使用它——三段式是 VS Code Marketplace、Open VSX、Zed 和 JetBrains 都接受的唯一格式。`version --set` 会拒绝其他格式，包括四段式的日期版本。应用市场只接受比已发布过的版本更高的版本号，所以一个版本号只要在任何地方发布过，就不再重复使用。

CI *构建时用*的版本号是另一回事，放在 `.github/versions.env` 里。

## 一个 release 里有什么

[`packaging/release.manifest.json`](../../packaging/release.manifest.json) 声明了这些：每个 release 文件是什么、怎么装。这份文件不是文档——release workflow 会拿它核对已暂存的目录，必需的 release 文件缺失、某个文件是空的，或者出现了一个清单里没声明的文件，就拒绝发布。release 页面的说明是 `CHANGELOG.md` 里这个版本的那一节（放在“What changed”下面），后面接着同一份声明渲染出来的文件列表，页面和检查不会走两个版本。`CHANGELOG.md` 里没有这个版本那一节的，在构建任何东西之前就会被拒绝。

| | 按平台各一份 | 是否必需 |
|---|---|---|
| `mcppls-<platform>.vsix` | 是 | 是 |
| `payload-<platform>.tar.gz` | 是 | 是 |
| `mcppls-zed-<version>.tar.gz` | 否 | 是 |
| `mcppls-clion-<version>.zip` | 否 | 是 |
| `mcppls-<version>-openkal.zip` | 否——一个包服务所有系统和架构 | 是 |
| `mcppls-<version>.okb` | 否——两种架构的映像 | 是 |
| `mcppls-<version>-<system>-<arch>`（`linux-x86_64`、`linux-aarch64`、`macos-aarch64`、`windows-x86_64.exe`） | 每个系统与架构一个 | 是 |
| `SHA256SUMS`、`MANIFEST.md` | 否 | 是 |

增减 release 文件只需修改这份发布清单，workflow 不用改。

**服务端只有一份字节。** 它按架构构建为 ok 映像（mcpp 的 `<arch>-openkal`，平台为 openkal-platform），上表中凡是带服务端的文件，带的都是这些映像：okb 带两份；每个 payload 和 VSIX 都把这个 okb 作为 `bin/mcppls.okb`，旁边是本平台的运行器 `bin/okrun`；通用包在 `lib/<arch>/` 下带两份，并带它服务的每个系统的运行器；单文件是一个系统的运行器，里面装着一份映像。CI 把每一个都与最先构建的映像（`server-image`）比对：通用包在 Linux、macOS、Windows 上的各行、okb、每个 payload 的 okb、每个 VSIX 的 okb（`distribution`，“一个 okb，每个运行器都能启动它”），并在各自的系统上运行它分发的每个运行器。按平台不同的只有 clangd 和工具包，所以 payload 和 VSIX 仍然按平台各一份。

| 任务（`ci.yml`） | 产出 | 运行于 |
|---|---|---|
| `server-universal` | 每个系统的 self-contained 通用包（`mcpp pack --target x86_64-openkal --mode self-contained --pkg-format zip`）；在 Linux 上还有 okb（`--pkg-format okb`，用 `okld bundle check` 检查）和单文件（`--pkg-format single-file --systems linux,macos,windows`，两种架构） | Linux、macOS、Windows |
| `server-openkal` | `mcppls-<version>-openkal.zip`：用 `mcpp pack --merge` 合并三者，带 linux-x86_64、linux-aarch64、macos-aarch64 与 windows-x86_64 的运行器 | Linux |
| `distribution` | 不产出文件：每个 VSIX 只带一个 okb，就是那一个 okb，它的运行器能启动它；通用包和单文件能启动服务端 | 每个平台 |

## 发起一次发布

整个过程是手动运行一次 workflow。**Actions → Release → Run workflow**，填上版本号：

| 输入 | 含义 |
|---|---|
| `version` | 例如 `0.0.2`。这次运行会创建标签 `v<version>` |
| `draft` | 默认开启——release 先暂存着，供你在任何人拿到之前先验证一遍，Open VSX 也包括在内 |
| `prerelease` | 默认开启；正式发布时关掉 |

运行之前：

1. 一切都在 `main` 上，CI 是绿的。
2. `mcpp run -p devtools -- version --set <version>` 的结果已经提交。这次运行会先检查这一点，各处版本不一致时会在构建前停止，避免花一个小时才发现一行写错。
3. `CHANGELOG.md` 里有这次发布的条目。

接下来这次运行会执行下面的 pre-release 测试：重新运行完整的 CI，构建 Zed 和 CLion 插件并安装，测性能和稳定性，把所有东西暂存好，对着发布清单检查，写出 `SHA256SUMS` 和 `MANIFEST.md`。只有全部通过，才会创建标签，把暂存好的候选版本逐个文件发布出去。

一个 release 到达用户要经过四步，只有第一步是自动的：

1. 这次运行把 release 创建成**草稿**。
2. 你在本地验证草稿里的 `mcppls-<platform>.vsix`（见[核实一次发布](#核实一次发布)）。
3. 你发布这个草稿。这会触发 `.github/workflows/publish-openvsx.yml`，把同样的文件发到 Open VSX（见 [Open VSX](#open-vsx)）。
4. 你把同样的文件上传到 VS Code Marketplace（见[下文](#vs-code-marketplace)）。

关掉 `draft` 的运行有意跳过第 2 步：release 立即发布，并由它的 `openvsx` job 发到 Open VSX，因为 workflow 创建的 release 不会触发其他 workflow。

## pre-release 测试

`.github/workflows/prerelease.yml` 就是一次不发布的 release：先跑完整的 `ci.yml`，再用这次运行构建出的产物跑 `.github/workflows/release-checks.yml`。它会在打上 `v<version>-rc<n>` 标签时自己触发，在 `main` 上每周跑一次，Release 本身也会先跑它。改动 release 打包方式的 pull request，由 CI 在自己的 job 全部通过后接着跑同样的 release 检查，CI 不会因此跑两遍。它的各个 job，除非另有说明，都会在 Linux、macOS、Windows 三个平台各跑一遍：

| Job | 通过条件 |
|---|---|
| CI | `ci.yml` 的每个 job 都通过 |
| plugins (Linux) | Zed 和 CLion 插件能构建、能打包 |
| install | devtools 能从 release 压缩包里装好两个插件，装好的服务端能应答 `inferred` 这个 fixture，`uninstall` 之后什么都不留下 |
| performance | 五次冷启动首次导航的中位数在 12 秒以内，五次热启动在 5 秒以内，且没有检查失败 |
| performance on a real project (Linux) | 服务端能通过 `self-mcpp` 这个 fixture：固定某次提交的 mcpp 仓库本身 |
| stability | 关键项目形态的 fixture（全 `.cppm`、`.cppm`/`.cpp` 混合、被监视的编辑、损坏的模块、挂起的构建工具、多个根目录）连续跑三轮都通过 |
| release candidate | 每个 release 文件都在，并且对着发布清单核对过；结果就是 `release-candidate` 这个 artifact |

`ci.yml` 本身在上述 fixture 之外，还会按用户的编辑器和平台来把关一次改动：

| Job | 通过条件 |
|---|---|
| `code-oss-e2e` | 扩展的端到端测试在 Code - OSS（VSCodium，版本固定为 `.github/versions.env` 里的 `VSCODIUM_VERSION`）中通过 |
| `proot` | 服务端在 termux 的 PRoot 下，两种模式、x64 和 arm64 上都通过检查（issue #32 的环境） |
| `ux` | 用户体验场景的响应时间都在预算之内，跑在固定版本的 mcpp 和 xlings 上（fixture `ux-mcpp` 和 `ux-xlings`） |

nightly 运行还多一个 `termux` job：termux 自己的用户空间和 PRoot，里面装一个由 `proot-distro` 提供的 Debian。它只是第二份参考，失败不会让 nightly 失败；CI 里没有真机 Android。

中位数和每轮的用时都在每次运行的 summary 里。想手动试一试候选版本，就从这次运行里下载 `release-candidate`：里面正是一次 release 会带的那一整套文件。

也可以先手动推送一个 `v*` 标签，效果相同。

## 新引擎

随附的 clangd 是 mcppls-clangd，在它自己的仓库发布，在这里由 `packaging/payload.lock.json` 锁定。
同时涉及两边的改动按下面的顺序进行，保证引擎发布出来的东西已经和本仓库的 CI 一起通过，发布后不会再冒出问题：

1. **引擎 PR 的 CI 为每个平台构建打包好的部件**（`clangd-<platform>-portable`，带 `engine.json`），验证方式与正式发布相同。
2. **本仓库的 PR 在引擎合入之前就用这份构建测试。** 提交 `packaging/engine-candidate.json`，写明引擎 PR 的那次运行：

   ```json
   { "repository": "Sunrisepeak/mcppls-clangd", "run-id": 38052537482,
     "engine-commit": "<构建它的引擎 PR head>" }
   ```

   CI 的 `payload` 任务随后用那次运行的部件组装每个平台的 payload，而不是锁文件里的归档，之后的所有任务都测试它。发现的问题在哪个 PR 就在哪个 PR 修，并把文件指向新的运行，直到两边都通过。
3. **合入并发布引擎**，再把锁文件切换到发布的归档（大小和 SHA-256，各自用两次下载和发布的 `.sha256` 核对），并在同一个提交里**删除候选文件**。CI 在发布的字节上再跑一次。
4. 合入本 PR 并发布。

候选文件永远不会进入发布：CI 在 `main` 和发布的运行中拒绝它，Release 工作流发现它会立即停止。在 `main` 以外的分支上手动运行 CI 或 nightly 时可以使用它：引擎对空闲内存或输入延迟的影响要在 nightly 的 ux 轮次里才看得出来，所以这些轮次要在引擎发布之前就能测到它。构建产物保留 14 天；更早的运行可以重新运行引擎 PR 的 CI 重建。

## 衡量一次改动

同样的字节，nightly 每次测出来的结果都会有起伏：runner 慢一些、时段忙一些都会影响。一轮红不等于回归，一轮绿也不等于修好了。手动运行 nightly（`gh workflow run nightly.yml --ref <分支> -f …`）可以在不重新构建的情况下衡量一次改动：

| 输入 | 作用 |
|---|---|
| `payload-run` | 测试该分支某次 CI 运行（填它的 run id）构建好的 payload，不再重新构建：约 35 分钟而不是 80 分钟，测的就是 PR 的 CI 测过的那份字节 |
| `scope` | `all`（所有任务）或 `ux`（只跑 ux 轮次、预算汇总和对比） |
| `ux-repeat` | 每一轮把 edits 阶段（输入、保存、U5、U6、U7a、U15）跑这么多次，每次一个新的 server 会话，复用 cold 和 warm 阶段准备好的东西：填 4，三轮合计每项 12 个样本，只构建一次 |
| `ux-baseline` | 一个已发布的版本（`v0.0.16`），它的 server 在同一批机器上跑同样的阶段，用自己的工作区和缓存，与本版本按 A B B A 交替；`ux budgets` 随后输出 `mcppls-devtools measure compare`：输入、保存、状态变化、空闲和超时这几项数字并排列出，中位数比基线差、且超出两者波动范围的会被标出 |

两个 server 用的 runner、fixture 和预算都是该分支的，所以唯一的差别就是 server。

## 核实一次发布

不以构建过程的输出为准，而是用公开可下载的文件重新验证。

| 检查项 | 怎么做 |
|---|---|
| 应有的 release 文件都在 | 对照 `MANIFEST.md`，这份清单 release 自己就带着 |
| 每个 release 文件的哈希 | **重新下载一遍**，算哈希，和 `SHA256SUMS` 比对 |
| VSIX 能装、能用 | 在一台机器上，用 VS Code，打开一个真实的 C++ modules 项目 |
| payload 不靠 VS Code 也能用 | 解压它，在另一个编辑器里通过 stdio 跑 `mcppls serve` |
| 真实升级能成功 | 在已经装过上一个版本的机器上试，不能只在干净机器上试 |

把结果记到这次发布的 tracking issue 上，包括哪里失败了、哪些是手动补完的。后续发布者依靠这些记录了解实际情况。

## Open VSX

Open VSX 是兼容 VS Code 的编辑器（Cursor、VSCodium、Windsurf、Trae 等）查找扩展的地方。release 一发布，`.github/workflows/publish-openvsx.yml` 就把它的每个 `mcppls-<platform>.vsix` 发到命名空间 `sunrisepeak`：无论是在网页上发布（`release: published`），还是 Release 直接发布时由它的 `openvsx` job 调用。它从 release 下载这些文件，对照 `SHA256SUMS` 检查，确认 token 有权发布到这个命名空间，然后逐个发布；每个 VSIX 自带平台信息，所以每个平台拿到的都是自己的文件。发布完成后，它会把每个文件从 Open VSX 重新下载下来，与 `SHA256SUMS` 比对哈希；Open VSX 提供的内容不一致时，这个 job 就会失败。GitHub 上的 pre-release 按普通版本发布。

Open VSX 已有的版本会被跳过（`--skip-duplicate`），所以重跑是安全的：**Actions → Publish to Open VSX → Run workflow**，填上标签，只会补发缺的部分。

token 是仓库 secret `OVSX_PAT`，属于拥有该命名空间的 Open VSX 账号（它的 Eclipse 账号已签署 Open VSX Publisher Agreement）。过期后，在 open-vsx.org 的 *Settings → Access Tokens* 生成新的并替换这个 secret；没换的话，workflow 里 *The token may publish to the namespace* 这一步会最先失败。

## VS Code Marketplace

本地验证之后手动上传：把已发布 release 的每个 `mcppls-<platform>.vsix` 上传到 publisher `sunrisepeak`，可以用 `npx @vscode/vsce publish --packagePath <这些文件>`，也可以在 publisher 管理页面上逐个上传（第一个用新建扩展，其余用 *Update*）。一个版本号只发布一次：Marketplace 只接受比它见过的所有版本都高的版本号。

## 还没做的

xlings 索引还需要自己的凭证，以及 release workflow 里的一个发布步骤。`mcppls-devtools release xlings` 已经能生成 xlings 的包描述文件；提交到索引的 pull request 还没加上。从 workflow 直接上传到 VS Code Marketplace 是有意不做的：它要留在上面的本地验证之后。
