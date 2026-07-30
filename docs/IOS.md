# iOS 平台支持与认证边界

GDPP 的 iOS 后端面向 Godot 4.4～4.7，最低系统版本为 iOS 16.0。目标产物是动态
XCFramework，而不是只包含真机 arm64 的单一 dylib：同一个项目库同时提供 iPhone arm64、
Apple Silicon 模拟器 arm64 和 Intel 模拟器 x86_64 三种切片。

当前状态是“构建链路与无签名模拟器集成通过”，不是“App Store 全流程已认证”。平台总体证据
见 [平台测试报告](PLATFORM_TEST_REPORT.md)，未完成项见 [路线图](ROADMAP.md)。

## 用户依赖

- GDPP 的 macOS Universal 2 编辑器插件最低支持 macOS 11.0，同时包含 arm64 与 x86_64
  切片；
- 能提供 iOS 16 SDK 的完整 Xcode、该 Xcode 要求的 macOS 宿主版本，以及通过 `xcode-select`
  选中的有效 Developer 目录；因此“插件可在 macOS 11.0 加载”不等于“macOS 11.0 可以安装
  当前 Xcode 并执行 iOS 导出”；
- `gdpp.zip`（Godot 4.6～4.7）或 `gdpp-all.zip`（Godot 4.4～4.7）；两个包都包含 macOS
  Universal 2 编辑器插件及 iOS、桌面、Android 与 Web release SDK；`gdpp-lite.zip` 不包含
  iOS 导出支持；
- 已安装对应 Godot iOS 导出模板；
- 真机或 App Store 导出所需的 Apple Developer Team、Bundle ID、证书和描述文件。

用户不需要 CMake、Ninja、Python、SCons，也不需要自行编译 godot-cpp。每套 SDK 的共享
`lib/` 中包含 device 和 Universal Simulator 两份优化后的 `template_release` 静态依赖；
`sdk/manifests/ios.arm64.sdk.manifest` 描述其身份。Debug 与 Release 导出复用这些 release
binding，它们只参与链接，不会整包进入游戏。

## 导出流程

1. 将所需版本范围的插件 ZIP 解压到项目根目录，得到 `addons/gdpp/`；iOS 依赖已经合并到
   `addons/gdpp/sdk/<Godot版本>/`，不需要再下载或拼接 target pack。
2. 使用 macOS 上对应版本的 Godot 打开项目并启用 GDPP。
3. 安装 Godot iOS 导出模板，在 iOS 预设中设置 Team ID、Bundle ID 和应用信息。
4. 保持 `gdpp/strip_gdscript_sources=true`、`gdpp/allow_source_fallback=false`。
5. 执行 Debug 或 Release 导出。GDPP 通过 metadata-only 脚本描述完成导出期类型和场景验证，
   不构建主机项目库；随后调用 `xcrun` 分别构建三个 iOS 切片，最后事务式提交 XCFramework。
6. Godot 将项目 XCFramework 嵌入生成的 Xcode 工程；签名、设备安装、Archive 和上传仍使用
   Godot/Xcode 的标准流程。

项目库固定生成到：

```text
addons/gdpp/binary/libgdpp.<debug|release>.ios.arm64.xcframework/
```

对象、切片 dylib 和尚未提交的合包只存在于 `addons/gdpp/build/project/native-direct/`。只有三个
切片全部编译、链接且 `xcodebuild -create-xcframework` 成功后，新目录才会替换当前项目库；
失败不会破坏上一次可用产物。

## 最低版本与 ABI 规则

- 所有切片显式使用 `-target *-apple-ios16.0`，target pack 清单也必须声明
  `platform_minimum iOS_16.0` 和 `ios_deployment_target 16.0`。
- 真机只支持 arm64；模拟器包同时包含 arm64 和 x86_64。
- Debug 与 Release 均使用唯一的 Release 优化 `template_release` ABI 静态依赖；Debug 脚本
  语义由 GDPP 自身的编译定义保留，target pack 不包含 `template_debug`。
- 发布库只导出 `gdpp_library_init`，Release 链接启用 dead stripping 并移除本地符号表。
- iOS 项目不生成 development 或 editor 项目库。

## 签名与 CI 边界

仓库的 iOS 流水线分为两层：Godot 4.4～4.7 SDK 矩阵检查三种切片、最低版本、路径
可复现性和 SDK 契约；Godot 4.6.2 集成门禁再生成无源码 Xcode 工程，并以关闭签名的方式完成
iOS Simulator Release 构建。集成门禁从官方 Godot 模板的实际静态库读取可用架构，并只选择
该架构与 GDPP Universal Simulator target pack 的交集；target pack 矩阵仍独立强制检查 arm64
和 x86_64，避免官方模板的单架构限制掩盖 GDPP 切片缺失。关闭签名只用于可重复 CI，不代表
正式包可以省略 Apple 签名。

商业发布仍需在发布组织自己的 Apple 账号环境完成：

- 真机安装和启动；
- 前台/后台、来电中断、内存警告、音频会话和触控测试；
- Release Archive、签名验证、TestFlight 与 App Store Connect 上传；
- 隐私清单、权限描述、图标、启动画面和商店合规检查。

这些发布凭据不应存入 GDPP 仓库或公共 CI。GDPP 负责生成可被标准 Godot iOS 导出器处理的
项目原生库，不接管 Apple 账号与商店发布流程。

## 当前未覆盖

- iPhone/iPad 真机的启动、网络、图形、音频、暂停恢复和内存压力；
- 签名 Archive、TestFlight、App Store Connect 与商店审核；
- Swift/Objective-C 原生插件与 GDPP 项目库的组合矩阵；
- 后台下载、推送、Game Center、StoreKit 等平台服务；
- 大型项目的设备内存、热量、启动和帧时间门禁。

这些是明确的认证缺口，不应被无签名模拟器构建结果掩盖。若客户交付包含上述能力，应在其
签名环境中增加对应 fixture、设备运行 oracle 和失败日志归档后再宣称支持。
