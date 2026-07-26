# 项目构建与导出

## 用户需要什么

正式插件包已经包含 compiler、GDPP runtime、生成所需头文件和 Godot 4.4～4.7 的
`template_release` 静态绑定。客户不需要 CMake、Ninja、Python、SCons，也不需要自行构建
godot-cpp。

客户只需要 Godot 和目标平台工具链：

| 导出目标 | 工具链 | 交付架构 |
|---|---|---|
| Windows | Visual Studio/Build Tools 的 MSVC x64 C++ 工具与 Windows SDK | x86_64 |
| macOS | Xcode Command Line Tools | Universal 2 |
| Linux | GCC 或 Clang | x86_64 |
| Android | Android NDK r28+ | arm64-v8a |
| iOS | 完整 Xcode | device arm64、Simulator arm64/x86_64 |
| Web | Emscripten | wasm32 threads/nothreads |

未交付：Windows arm64、Linux arm64、Android x86_64。

## 安装

下载当前桌面宿主包并直接解压到项目根目录：

```text
project/
└── addons/
    └── gdpp/
        ├── plugin.cfg
        ├── gdpp.gdextension
        ├── binary/
        └── sdk/
            ├── 4.4/
            ├── 4.5/
            ├── 4.6/
            └── 4.7/
```

ZIP 外层包含 `addons/gdpp`，不需要手工移动文件。然后在 Godot 的插件设置中启用 GDPP。

三个发布包：

- `gdpp-mac.zip`：macOS Universal 2 + Android + iOS + Web；
- `gdpp-linux.zip`：Linux x86_64 + Android + Web；
- `gdpp-win.zip`：Windows x86_64 + Android + Web。

每个包都同时包含 4.4～4.7，不按 SDK 版本拆成多个 ZIP。

## 项目设置

插件创建或使用以下项目设置：

| 设置 | 默认/用途 |
|---|---|
| `gdpp/target_godot_version` | 从当前编辑器检测；可选 4.4、4.5、4.6、4.7 |
| `gdpp/build/sdk_root` | 当前包内 SDK 根目录 |
| `gdpp/build/cpp_compiler` | Windows 默认 `cl.exe`，macOS/Linux 默认 `c++` |
| `gdpp/build/android_ndk_root` | Android NDK 根目录 |
| `gdpp/build/emscripten_cxx` | 默认 `em++` |
| `gdpp/strip_gdscript_sources` | AOT 预设应为 `true` |
| `gdpp/allow_source_fallback` | 商业二进制预设应为 `false` |

目标 Godot 版本影响 API 表、SDK、缓存、项目库和运行描述符，不只是一个文档标签。

## 工具链发现

### Windows

默认配置是 `cl.exe`，不包含任何测试机专用路径。GDPP 按以下顺序寻找并初始化 MSVC：

1. 可选环境变量 `GDPP_VCVARS_PATH`；
2. 用户配置的绝对 compiler 路径附近；
3. Visual Studio Installer 的 `vswhere.exe`；
4. `VSINSTALLDIR` / `VCINSTALLDIR`；
5. Program Files 下 Visual Studio 2026/2022/2019 的 BuildTools、Community、Professional、
   Enterprise、Preview 标准布局。

找到 `vcvars64.bat` 后，在隐藏的隔离进程中只初始化一次环境，缓存结果，再解析 `cl.exe` 和同目录
链接器。`cl.exe`、`link.exe` 和 bootstrap 不创建可见控制台窗口。找不到时导出预检给出安装
x64 C++ tools 或配置 `gdpp/build/cpp_compiler` 的错误，不尝试硬编码用户目录。

### macOS

桌面目标使用配置的 `c++`/AppleClang。iOS 使用 `xcrun --find clang++`、当前
`xcode-select` Developer 目录和目标 SDK；完整 Xcode 缺失时在构建客户源码前失败。

### Linux

默认从 `PATH` 使用 `c++`，可以配置 GCC 或 Clang 绝对路径。发行 SDK 的 glibc 基线为 2.35；
客户工具链仍需能链接匹配的 C++17、PIC 和系统库。

### Android/Web

Android 从显式设置或标准环境确定 NDK，Web 使用 `em++`。目标 SDK manifest 会校验 NDK API/
STL 或 Web threads 模式，不能用另一个模式的静态库勉强链接。

## 导出期流水线

平时编辑、导入和运行项目不编译客户项目库。启用 AOT 的导出执行一次事务：

```text
扫描项目脚本
  -> 解析
  -> 语义分析和项目固定点
  -> 预编译脚本行为
  -> 写入生成 C++17/metadata
  -> 顺序编译每个翻译单元一次
  -> 链接一个目标项目库
  -> 转换 PackedScene/Resource/Autoload
  -> 写入运行描述符并剥离 .gd/.gdc
  -> 交还 Godot 标准打包
  -> 审计并恢复源工程事务
```

一次 Debug 导出只构建 Debug，一次 Release 只构建 Release。不会先构建 editor/development 项目
库，也不会同时编译两种 profile。Debug/Release 均链接唯一的 Release 优化
`template_release`，Debug 区别只在脚本 `assert` 等语义。

每个翻译单元和链接命令严格串行。构建运行在后台线程，主线程继续渲染、处理窗口和驱动导出。
一个连续进度条覆盖全部大阶段；逐文件步骤显示 `(当前/总数)`，结束后让出 Godot 自己的打包进度。

## Attached 转换

客户脚本不会变成取代原对象的第二个原生类。场景或资源中的真实 Godot/第三方对象保持原类型，
GDPP 通过 `AttachedCompiledScript` 提供生成行为。

转换规则：

- 只复制原 SceneState/Resource 实际保存的存储属性；
- 未覆盖默认字段由生成 C++ 构造逻辑初始化；
- onready 在用户 `_ready` 前执行；
- 继承脚本、内部类、Signal、RPC 和 Autoload 保持脚本语义；
- 动态 Material/Shader 参数、网络资源和运行期属性仍走 Godot Object/Variant ABI；
- 第三方 provider 描述符和二进制保持独立。

转换失败不会提交部分场景或剥离部分源码。

## SDK 与项目库

SDK schema 11 固定以下契约：

- Godot API、平台、架构和最低系统；
- C++17、异常关闭、工具链族和 MSVC 静态 CRT；
- `debug,release` 项目 profile；
- 唯一 `distribution_binding template_release`；
- runtime ABI 13 和所有 runtime 文件 SHA-256；
- Android API/STL、iOS slices、Web threads 等目标字段。

NativeBuilder 在创建第一条编译命令前验证完整 manifest。旧 schema、错误 API/架构、损坏
runtime、混入 editor/template_debug 或错误工具链都会失败关闭。

输出名称：

```text
gdpp.debug.windows.x86_64.dll
gdpp.release.windows.x86_64.dll
libgdpp.release.linux.x86_64.so
libgdpp.release.macos.universal.dylib
libgdpp.release.android.arm64.so
libgdpp.release.web.wasm32.nothreads.wasm
libgdpp.release.web.wasm32.threads.wasm
libgdpp.release.ios.arm64.xcframework/
```

动态库文件前缀是 `gdpp`；GDExtension C 入口固定为 `gdpp_project_library_init`。二者不要混淆。

## 缓存

```text
addons/gdpp/build/project/
├── generated/
├── manifest.txt
├── bridge.lock
└── native-direct/
    └── <api>/<platform>/<arch>[/<web-mode>]/<profile>/
        ├── objects/
        └── build-configuration.txt
```

项目 manifest 区分源码实现哈希和公开 ABI 哈希，只让真实依赖方失效。对象缓存再包含 include/
depfile、SDK/runtime、第三方 bridge、工具链绝对路径、profile 和可复现路径映射。

当前缓存事务在单进程内安全；多个编辑器或 CLI 同时写同一项目尚无跨进程锁，不属于支持用法。
新进程仍需重新执行前端，持久化 AST/符号摘要尚未实现。

## 产物边界

成功游戏只包含：

- 一个匹配 profile/平台/架构的 `gdpp.*` 项目库；
- 同路径的项目运行描述符；
- 原项目非脚本资源；
- 第三方目标 GDExtension 及其描述符。

不会包含：

- 客户 `.gd/.gdc`；
- GDPP compiler/fallback；
- SDK、godot-cpp 静态库或头文件；
- 生成 C++、对象、depfile、manifest 或构建日志；
- 另一个 profile/架构的项目库。

## 失败恢复

严格二进制导出推荐：

```text
gdpp/strip_gdscript_sources=true
gdpp/allow_source_fallback=false
```

任一步失败都会注入不可满足的目标库并阻断 Godot 打包，同时恢复 compiler 描述符、extension
registry、供应商扫描描述和 Autoload。插件下次启动也会检查中断事务备份。

已知尚未闭合的构建可靠性边界：

- 多进程同时导出；
- 用户取消和编辑器关闭期间的子进程终止；
- 断电、低磁盘、只读目录和跨卷原子提交；
- 动态拼接 `.gd` 路径的无源码保留策略；
- 运行期热重载（明确不提供）。

需要普通 GDScript 包时必须显式启用 source fallback 或使用独立非剥离预设，不能把 AOT 失败
静默当成功。
