# Flutter SDK 操作

## 构建打包

在本目录执行，需要 Xcode、Python 3.10+ 和 depot_tools：

```bash
./release_sdk.sh
```

在 Mac 从源码构建 Android、iOS、Web、macOS 引擎并打包。
包含支持的架构以及 debug/profile/release 模式。
SDK 和压缩包输出到配置目录的 `sdk/`。
只构建用 `./release_sdk.sh build`，已有构建结果只打包用 `./release_sdk.sh package`。
输出目录必须是新目录；参数见 `./release_sdk.sh --help`。

## 设置 Flutter 环境变量

仍在本目录执行（Bash/Zsh）。下面相对路径对应同级的 `flutter-sdk-release`
发布目录；如果改了输出目录，把路径替换为实际的 `sdk/flutter`：

```bash
export FLUTTER_ROOT="$(cd ../../flutter-sdk-release/sdk/flutter && pwd -P)"
export PATH="$FLUTTER_ROOT/bin:$PATH"
hash -r
which flutter
```
