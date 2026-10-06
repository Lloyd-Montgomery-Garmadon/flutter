# Venus Flutter SDK

## 打包

在本目录打开终端。首次复制配置文件，填写两个本机路径：

```bash
cp release_sdk.env.example release_sdk.env
```

- `VENUS_ROOT`：Venus 仓库目录。
- `VENUS_RELEASE_DIR`：本次发布的新输出目录。

脚本自动读取同目录配置，然后执行：

```bash
./release_sdk.sh          # 查看用法
./release_sdk.sh source   # 导出发布源码到 source/
./release_sdk.sh build    # 构建本机产物到 build/
```

所有构建机器共用同一份 `source/`。完整发布需要 macOS、Linux、Windows
构建环境，包含 debug/profile/release。跨机器传递时复制整个 `build/artifacts/`。

收齐产物后组包。macOS arm64 示例使用 Linux x64 和 Mac x64 的完整构建结果：

```bash
./release_sdk.sh package \
  --build-receipt /你的/linux-artifacts/receipt.json \
  --build-receipt /你的/mac-x64-artifacts/receipt.json \
  --flutter-root /你的/本机构建目录/build/flutter
```

SDK 归档与 `delivery.json` 输出到 `sdk/`。构建需要 Python 3.10+、Git、
PATH 中可用的 `gclient` 和各平台官方工具链。现有输出不会被覆盖。

## 设置 Flutter 环境变量

进入**要使用的 Flutter SDK 的 `venus/` 目录**，执行以下 Bash 命令：

```bash
export FLUTTER_ROOT="$(cd .. && pwd -P)"
export PATH="$FLUTTER_ROOT/bin:$PATH"
hash -r
which flutter
```

这会切换当前终端使用的 Flutter，Bash/Zsh 均可用。

本地 `release_sdk.env` 不提交。模板、补丁和 Skill 均维护在 Venus 的
`scripts/flutter_sdk/`。`./release_sdk.sh --help` 查看全部打包参数。
