# Flutter SDK 操作

## 构建打包

在本目录执行，需要 Xcode、Python 3.10+ 和 depot_tools：

```bash
./release_sdk.sh
# 打包成功后，清理中间产物：
./release_sdk.sh clean
```

在 Mac 从源码构建 Android、iOS、Web、macOS 引擎并打包。
包含支持的架构以及 debug/profile/release 模式。
首次构建自动从当前干净、身份匹配的 Flutter checkout 导出源码快照。
SDK 和压缩包默认输出到当前 Flutter 目录旁的 `<目录名>-sdk-release/sdk/`，
可在本目录的 `release_sdk.env` 中设置 `VENUS_RELEASE_DIR` 修改。
`clean` 删除源码快照、构建目录和打包临时缓存，保留最终 SDK、压缩包和交付记录；未打包成功时拒绝清理。
只构建用 `./release_sdk.sh build`，已有构建结果只打包用 `./release_sdk.sh package`。
输出目录必须是新目录；参数见 `./release_sdk.sh --help`。

## 设置 Flutter 环境变量

先进入打包或解压后的 Flutter SDK 根目录（包含 `bin/flutter` 的目录），再执行（Bash/Zsh）：

```bash
export FLUTTER_ROOT="$(pwd -P)"
export PATH="$FLUTTER_ROOT/bin:$PATH"
hash -r
which flutter
```
