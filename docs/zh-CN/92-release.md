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
| `SHA256SUMS`、`MANIFEST.md` | 否 | 是 |

增减 release 文件只需修改这份发布清单，workflow 不用改。

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

中位数和每轮的用时都在每次运行的 summary 里。想手动试一试候选版本，就从这次运行里下载 `release-candidate`：里面正是一次 release 会带的那一整套文件。

也可以先手动推送一个 `v*` 标签，效果相同。

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

Open VSX 是兼容 VS Code 的编辑器（Cursor、VSCodium、Windsurf、Trae 等）查找扩展的地方。release 一发布，`.github/workflows/publish-openvsx.yml` 就把它的每个 `mcppls-<platform>.vsix` 发到命名空间 `sunrisepeak`：无论是在网页上发布（`release: published`），还是 Release 直接发布时由它的 `openvsx` job 调用。它从 release 下载这些文件，对照 `SHA256SUMS` 检查，确认 token 有权发布到这个命名空间，然后逐个发布；每个 VSIX 自带平台信息，所以每个平台拿到的都是自己的文件。GitHub 上的 pre-release 按普通版本发布。

Open VSX 已有的版本会被跳过（`--skip-duplicate`），所以重跑是安全的：**Actions → Publish to Open VSX → Run workflow**，填上标签，只会补发缺的部分。

token 是仓库 secret `OVSX_PAT`，属于拥有该命名空间的 Open VSX 账号（它的 Eclipse 账号已签署 Open VSX Publisher Agreement）。过期后，在 open-vsx.org 的 *Settings → Access Tokens* 生成新的并替换这个 secret；没换的话，workflow 里 *The token may publish to the namespace* 这一步会最先失败。

## VS Code Marketplace

本地验证之后手动上传：把已发布 release 的每个 `mcppls-<platform>.vsix` 上传到 publisher `sunrisepeak`，可以用 `npx @vscode/vsce publish --packagePath <这些文件>`，也可以在 publisher 管理页面上逐个上传（第一个用新建扩展，其余用 *Update*）。一个版本号只发布一次：Marketplace 只接受比它见过的所有版本都高的版本号。

## 还没做的

xlings 索引还需要自己的凭证，以及 release workflow 里的一个发布步骤。`mcppls-devtools release xlings` 已经能生成 xlings 的包描述文件；提交到索引的 pull request 还没加上。从 workflow 直接上传到 VS Code Marketplace 是有意不做的：它要留在上面的本地验证之后。
