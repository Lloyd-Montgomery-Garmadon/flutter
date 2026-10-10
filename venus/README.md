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
默认增量编译；首次构建导出当前干净、身份匹配的源码快照，后续复用构建目录、工具链和 Ninja 缓存。
已提交的同版本源码更新会校验并前移快照，再由 GN/Ninja 重建受影响目标；升级版本使用各版本独立目录。
SDK 和压缩包默认输出到当前 Flutter 目录旁的 `<目录名>-sdk-release-3.44.9/sdk/`，
可在本目录的 `release_sdk.env` 中设置 `VENUS_RELEASE_DIR` 修改。
`clean` 只删除打包验证的临时缓存，保留源码快照、构建缓存、最终 SDK、压缩包和交付记录；未打包成功时拒绝清理。
只构建用 `./release_sdk.sh build`，已有构建结果只打包用 `./release_sdk.sh package`。
再次执行同一命令即可增量构建；打包校验通过后替换旧 SDK，失败保留旧交付与构建缓存。
废弃的自有打包临时目录会清理，工作区锁拒绝并发构建/清理；参数见 `./release_sdk.sh --help`。

## 设置 Flutter 环境变量

最终 Flutter SDK 根目录默认是当前 Flutter 目录旁的
`<目录名>-sdk-release-3.44.9/sdk/flutter/`（包含 `bin/flutter`）。
在本目录执行（Bash/Zsh），直接读取打包入口已确定的路径，
无需再传发布目录或填写版本：

```bash
export FLUTTER_ROOT="$(./release_sdk.sh sdk-path)"
export PATH="$FLUTTER_ROOT/bin:$PATH"
hash -r
which flutter
```
