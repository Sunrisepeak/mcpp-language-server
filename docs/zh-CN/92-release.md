# 发布 release

[English](../92-release.md) | **简体中文**

> release 发布在本仓库的 GitHub release 页面。VS Code Marketplace、Open VSX 和 xlings 索引是各自独立的渠道，有自己的凭证，目前还没有开始发布；每个渠道会用到的标识符见 [91-naming.md](91-naming.md)。

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

[`packaging/release.manifest.json`](../../packaging/release.manifest.json) 声明了这些：每个 release 文件是什么、怎么装。这份文件不是文档——release workflow 会拿它核对已暂存的目录，必需的 release 文件缺失、某个文件是空的，或者出现了一个清单里没声明的文件，就拒绝发布。release 页面上的说明文字也是从同一份声明渲染出来的，页面和检查不会走两个版本。

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
| `draft` | 默认开启——release 先暂存着，供你在别人看到之前先看一遍 |
| `prerelease` | 默认开启；正式发布时关掉 |

运行之前：

1. 一切都在 `main` 上，CI 是绿的。
2. `mcpp run -p devtools -- version --set <version>` 的结果已经提交。这次运行会先检查这一点，各处版本不一致时会在构建前停止，避免花一个小时才发现一行写错。
3. `CHANGELOG.md` 里有这次发布的条目。

接下来这次运行会执行下面的 pre-release 测试：重新运行完整的 CI，构建 Zed 和 CLion 插件并安装，测性能和稳定性，把所有东西暂存好，对着发布清单检查，写出 `SHA256SUMS` 和 `MANIFEST.md`。只有全部通过，才会创建标签，把暂存好的候选版本逐个文件发布出去。

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

## 还没做的

Marketplace、Open VSX 和 xlings 索引都还需要各自的凭证，以及 release workflow 里的一个发布步骤。`mcppls-devtools release xlings` 已经能生成 xlings 的包描述文件；提交到索引的 pull request，以及另外两个渠道，都还没加上。
