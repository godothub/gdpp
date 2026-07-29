# 商业交付模型

## 发行物

正式发布只提供两个多宿主插件 ZIP 和一个校验和文件：

| 归档 | Godot SDK | 编辑器与导出目标 |
|---|---|---|
| `gdpp.zip` | 4.6、4.7 | macOS Universal 2、Linux x86_64、Windows x86_64 编辑器，以及全部支持的桌面、Android、iOS、Web 导出 SDK |
| `gdpp-all.zip` | 4.4～4.7 | 与 `gdpp.zip` 相同的平台集合，额外提供 4.4、4.5 SDK |
| `SHA256SUMS` | 两个 ZIP 的 SHA-256 | 不含插件内容 |

两个 ZIP 都可以直接解压到项目根目录；默认包布局为：

```text
addons/
└── gdpp/
    ├── plugin.cfg
    ├── gdpp.gdextension
    ├── *.gd
    ├── binary/
    └── sdk/
        ├── 4.6/
        └── 4.7/
```

`gdpp-all.zip` 在相同布局中增加 `4.4/` 和 `4.5/`。外层必须是 `addons/gdpp`，不是裸
`gdpp/`，也不是把多个平台插件 ZIP 再压一次。每个包都包含三种桌面宿主的 compiler/fallback；
每个版本的公共头文件和 runtime 只保留一份，macOS、Linux、Windows、Android、iOS 与 Web
Release 绑定合并到共享 `lib/`，平台 ABI 由独立 target manifest 选择。

## 交付目标

| 目标 | 交付架构/模式 | 最低契约 |
|---|---|---|
| Windows | x86_64 | Windows 10、MSVC 19.x、静态 CRT |
| macOS | Universal 2 | macOS 11.0 |
| Linux | x86_64 | Ubuntu 22.04 / glibc 2.35 |
| Android | arm64-v8a | Android 9 / API 28、`c++_shared` |
| iOS | device arm64、Simulator arm64/x86_64 | iOS 16.0 |
| Web | wasm32 threads/nothreads | 匹配 Godot 的 Emscripten target pack |

Windows arm64、Linux arm64、Android x86_64 不在当前包中。构建器会在生成客户 C++ 前拒绝这些
架构，不允许误用 x86_64/arm64 静态库。

## 单一分发绑定

compiler 插件自身在 GDPP 发布构建中链接 godot-cpp `editor`。发行 ZIP 只保留链接完成的
compiler 动态库，不携带 editor 静态库。

客户 SDK 每个平台/架构/线程模式只包含一份优化后的 godot-cpp `template_release` 静态库：

- 没有 `template_debug`；
- 没有 editor 静态库；
- Debug/Release 项目导出都复用同一绑定；
- 一次导出只编译所选 profile 的客户翻译单元一次；
- Debug 的 assert、breakpoint 和源码调试帧由 GDPP 代码生成开关保留，Release 完全移除。

这样避免插件包和客户编译因两套模板绑定重复。

## SDK 契约

SDK schema 12、runtime ABI 21 由源码、打包器和发布门禁共同校验。manifest 包含：

- GDPP/Godot 版本、平台、架构、最低系统；
- C++17、异常关闭、工具链族/版本和 MSVC CRT；
- `debug,release` profile 与唯一 `template_release`；
- runtime、Attached runtime、整数语义文件的 SHA-256；
- Android API/STL、iOS slices、Web threads 与路径映射。

打包器拒绝：

- 缺少当前归档声明的任一 Godot SDK；
- schema/runtime ABI 或文件摘要冲突；
- 缺少任一桌面宿主、混入 editor/template_debug 静态库；
- 项目生成库、build 目录、嵌套 ZIP；
- symlink、AppleDouble、`.DS_Store`、`__MACOSX`；
- 来源组件中的客户路径或不允许的产物。

## 导出责任边界

GDPP 只在 AOT 导出时编译客户项目。普通编辑、导入和编辑器运行继续使用原 `.gd`。

一次导出：

```text
一个目标 + 一个 profile
  -> 一次项目 frontend/codegen
  -> 每个翻译单元顺序编译一次
  -> 链接一个项目库
  -> 转换场景/资源/Autoload
  -> 剥离源码
  -> Godot 标准打包
```

不存在客户 editor/development 项目库。成功成品只携带一个
`gdpp.<debug|release>.<platform>.<arch>` 项目库；compiler、fallback、SDK、静态库、生成 C++、
对象缓存和客户 `.gd/.gdc` 都不得进入。

## 第三方 GDExtension

第三方描述符和目标二进制由 Godot 原样导出。GDPP：

- 不读取/修改供应商源码；
- 不要求供应商头文件；
- 不链接供应商库；
- 不生成跨动态库 C++ 继承；
- 从 ClassDB 捕获公开 runtime 契约；
- 通过 Attached Script runtime 保留客户 `extends VendorType`；
- 通过精确 MethodBind hash 调用外部 `super`；
- 对 provider 缺失、editor-only 类型或契约不完整失败关闭。

第三方 addon 自己的 `.gd` 与项目脚本统一进入 AOT，不能按 `addons/` 目录静默跳过。

## 源码与描述符事务

源工程中只有 `addons/gdpp/gdpp.gdextension` 一个活动 GDPP 描述符。标准 AOT 导出保持该
editor-only 物理文件不变，在导出回调中跳过它，并在成品同一路径提供项目 runtime 描述符字节。
项目库由 GDPP 通过公开导出 API 注册一次；Universal 2 provider 的归一化或 Debug→Release
选择也只存在于包内描述符。临时 Autoload 和必要的 source-fallback registry 状态会在成功或失败
后恢复；启动恢复逻辑兼容 1.7.8 及更早版本留下的中断备份。

这一模型防止：

- compiler 与项目库同时进入成品；
- 同一项目库被两个描述符重复加载；
- provider 描述符为了扫描被永久修改；
- 失败导出留下 AOT 场景缓存污染普通 GDScript 导出。

项目库不可热重载。重新导出/重启是 Attached 实例的安全版本边界。

## 二进制与内容审计

正式门禁检查：

- 项目库只有 `gdpp_library_init` 一个公开 C 入口；
- Windows PE、Linux/Android ELF、macOS/iOS Mach-O、Wasm 目标格式匹配；
- macOS compiler/SDK/项目库为真实 Universal 2；
- Release ELF 普通符号表剥离，链接器 dead stripping/section GC 生效；
- 成功目录和 PCK 只有一个匹配项目库；
- `.gd/.gdc`、compiler、fallback、SDK、godot-cpp、静态库、生成 C++、对象泄漏为零；
- 导出日志有 AOT 完成摘要，无未解释错误/警告；
- 成品在没有 compiler 和 SDK 的环境独立运行。

项目库文件名使用 `gdpp` 前缀，入口符号固定为 `gdpp_library_init`；入口名不是最终文件名。

## 发布门禁

最近完成的 1.8.2 正式发布运行 `30311849463` 包含 48 个成功作业，并在既有发布拓扑上增加真实
跨脚本工程的前端、项目语义、生成 C++17 与原生语法兼容门禁：

- macOS/Linux/Windows 编译器核心和 plugin 集成；
- Windows compiler 在进入 GDExtension ABI 前的 `LoadLibraryExW`、入口导出与卸载安全；
- ASan、UBSan、TSan；
- 官方项目/parser 语料、coverage-guided fuzz、完整 AST span golden；
- 全 Variant/fault 源码/AOT 差分、协程访问器与 await 默认参数；
- 真实 ENet 多 peer RPC 与二进制动态 Script 资源加载；
- 独立 4.7 double precision custom add-on 构建与精确 API/ABI 审计；
- Godot 4.4.1、4.5.2、4.6.3、4.7.1；
- 三桌面 4.4～4.7 host SDK；
- Android 4.4～4.7 SDK 与 4.5.2 APK；
- iOS 4.4～4.7 SDK 与 4.6.2 无签名 Simulator Xcode 导出；
- Web 4.4～4.7 双 SDK 与 4.5.2 Chromium 双模式；
- 两个最终 ZIP 组装；
- 默认 ZIP 在 macOS/Linux/Windows 干净项目中的安装、导出、运行和 PCK 审计；
- Release readiness、GitHub Release 与资产合同验证。

发布前测试和构建并行，只有全部成功后才组装/验证最终包并创建 Release。正式资产固定为两个
ZIP 加 `SHA256SUMS`。

## 失败关闭

商业预设：

```text
gdpp/strip_gdscript_sources=true
gdpp/allow_source_fallback=false
```

编译、链接、ClassDB/第三方契约、资源转换、描述符、库加载或内容审计任一步失败，都阻断导出。
客户确实选择普通 GDScript 交付时必须显式允许 source fallback。

## 商业声明边界

可以声明：已覆盖目标和语言范围内的导出期 AOT、单项目库、无脚本文本和失败关闭交付。

不能声明：

- 任意 GDScript、任意项目或全部设备已经兼容；
- 原生库不可逆或业务数据已加密；
- Android/iOS 商店、所有浏览器、Windows/Linux arm64 已认证；
- 包已经包含代码签名、公证、SBOM、provenance 或客户符号服务。

客户项目只能作为内部验证语料。公开 changelog/文档应记录通用能力和证据，不写客户项目名称或
把单项目结果外推为产品承诺。
