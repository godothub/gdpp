# 商业交付模型

## 发行物

每个公开 ZIP 都包含完整的 `addons/gdpp`，直接解压到 Godot 项目根目录即可安装，不是多个插件压缩包的嵌套。
正式发布只提供三个按桌面宿主划分的归档，每个归档同时包含 Godot 4.4、4.5、4.6、4.7 四套
SDK：

| 归档 | compiler/fallback 与桌面 SDK | 额外目标 SDK |
|---|---|---|
| `gdpp-mac.zip` | macOS Universal 2（arm64 + x86_64） | Android arm64、iOS device/Universal Simulator、Web threads/nothreads |
| `gdpp-linux.zip` | Linux x86_64 | Android arm64、Web threads/nothreads |
| `gdpp-win.zip` | Windows x86_64 | Android arm64、Web threads/nothreads |

compiler 插件固定使用最低 Godot 4.4 GDExtension ABI，同一前端支持 4.4～4.7 目标 API。目标
SDK 版本决定客户项目库的 Godot ABI，不会改变 compiler 的最低加载版本。

| 目标 | 交付架构 | 原生基线 |
|---|---|---|
| Windows | x86_64 | Windows 10、MSVC 19.x、静态 CRT |
| macOS | Universal 2（arm64 + x86_64） | macOS 11.0 |
| Linux | x86_64 | Ubuntu 22.04 / glibc 2.35 |
| Android | arm64-v8a | Android 9 / API 28、`c++_shared` |
| iOS | device arm64、Simulator arm64/x86_64 | iOS 16.0 |
| Web | wasm32 threads / nothreads | Emscripten target pack |

每个包只携带当前桌面宿主的 compiler/fallback，不包含另外两个桌面平台的插件或 SDK。四个
Godot 版本均使用同一 `sdk/<版本>/` 布局，Android、iOS、Web 位于对应版本的目标子目录。
打包器拒绝缺失版本或目标、运行时 ABI/摘要冲突、额外桌面宿主、editor/template_debug
静态绑定、嵌套 ZIP、build 目录、客户项目库、AppleDouble/`.DS_Store` 和其他本地生成物。

## 单绑定 SDK

compiler 动态库和客户目标 SDK 是两个隔离的 ABI 域：

- compiler 在 GDPP 发布构建中链接 godot-cpp `editor`，发行物只保留链接完成的动态库；
- 客户 SDK 每个平台/架构/线程模式只携带一份 godot-cpp `template_release` 静态库。

客户包不携带 `editor` 或 `template_debug` 静态库。Debug/Release 游戏导出都链接唯一
`template_release`，Debug 的脚本断言和诊断由 GDPP 编译定义保留。这样不会因 profile 增加
几百 MiB 重复绑定，也不会在一次导出中把客户翻译单元编译两遍。

SDK schema 11 和 runtime ABI 13 是强制契约。包清单记录版本、宿主、目标、最低系统与内容摘要；
SDK 清单进一步记录 C++17、异常模型、工具链/CRT、目标 profile、唯一分发绑定、移动/Web ABI
字段和所有 runtime 文件摘要。任一字段缺失或不一致都在客户编译前失败。

## 导出期职责

普通编辑、运行和导入完全沿用项目原有 `.gd`。只有启用 AOT 的导出触发项目编译，而且一次导出
只构建用户选择的一个目标：

```text
Debug export   -> 一次 frontend/codegen -> 一次 Debug 目标编译/链接
Release export -> 一次 frontend/codegen -> 一次 Release 目标编译/链接
```

不存在客户 development/editor 项目库。客户源码、GDPP runtime 和注册单元都只进入当前目标的一组
对象；所有命令按翻译单元串行执行。构建线程不占用 Godot 主循环，主线程只处理 UI、ClassDB
快照和导出事务。

成功游戏只携带当前 `gdpp.<debug|release>.<platform>.<arch>` 及同路径运行描述符。
compiler、fallback、SDK、静态库、生成 C++、对象缓存和客户 `.gd` 均不得进入包。

## 零改动第三方 GDExtension

所有兼容 GDScript 统一使用 Attached AOT。Godot 内置对象或第三方 GDExtension 对象继续拥有
真实 Node/Resource 身份；生成行为由 `ScriptLanguageExtension`、`ScriptExtension` 与
`ScriptInstance` 附着。客户不修改脚本、场景、资源、Autoload，也不修改供应商插件。

compiler 在主线程捕获第三方 ClassDB 契约，并从项目语义图生成、安装 metadata-only 脚本描述；
不加载客户 `.gd` 来反射，也不执行其静态初始化。每个临时实例只序列化原 SceneState/Resource
明确保存的字段，未覆盖默认值由目标行为构造器负责。后台构建只消费不可变快照。项目库不继承
供应商 C++ 类、不读取供应商头文件、不链接供应商库；外部 `super` 使用精确 MethodBind
compatibility hash。

供应商描述符和动态库原样进入成品。macOS Universal 2 描述符只在验证双切片后为导出扫描临时
归一化，包内恢复供应商原始字节。项目运行时不依赖 provider 与 GDPP 描述符在 extension registry
中的偶然顺序，缺失 provider 时确定性失败。

## 描述符与事务

编辑态只有 `addons/gdpp/gdpp.gdextension` 一个物理描述符。导出器按事务执行：

1. 备份 compiler 描述符、extension registry、供应商扫描描述符和 Autoload 设置；
2. 写入当前目标的临时扫描描述符；
3. 转换场景、资源和 Autoload；
4. 向成品同一路径写入项目运行描述符；
5. 在成功、失败或下次插件启动恢复所有源工程文件。

该模型避免两个 GDPP 描述符被重复扫描或同一项目库被重复打包。fallback 只用于显式普通
GDScript 导出或失败扫描保护；成功 AOT 包中不得存在。

## 符号、体积与源码保护

compiler、fallback 与项目库分别只公开唯一的 GDExtension C 入口。ELF 使用 version script 和
`--exclude-libs`，Mach-O 使用 exported-symbol list，Windows 使用明确导出；Debug 与 Release
项目目标都启用优化、section 级死代码删除和本地符号裁剪。

归档门禁至少验证：

- 每个客户 SDK 目录恰好一个 `template_release`，没有 editor/template_debug；
- 插件包没有 `addons/gdpp/build` 或 `gdpp.<debug|release>.*` 项目构建产物；
- 成功游戏目录和 PCK 恰好一个匹配目标的项目库；
- PCK 中 `.gd`/`.gdc`、compiler、fallback、SDK、静态库、生成 C++ 和对象文件泄漏数为零；
- 项目库公开符号只有 `gdpp_project_library_init`；
- 导出日志有完成摘要且没有未解释的错误或警告；
- 导出程序能在没有编辑器和 SDK 的环境独立启动并通过行为 oracle。

原生编译提高脚本逆向成本，但不承诺客户端逻辑不可逆。文案不得把“无脚本文本”描述成加密或
绝对防护。

## 失败关闭与恢复

商业预设默认：

```text
gdpp/strip_gdscript_sources=true
gdpp/allow_source_fallback=false
```

编译、链接、ClassDB/第三方契约、资源转换、描述符、产物唯一性或源码审计任一步失败，导出器
都注入缺失目标库并阻断打包，不能因为 Godot 某些版本返回进程码 0 就视为成功。发布判定同时
要求正确退出码、完成摘要、零错误日志和导出后黑盒审计。

客户确需普通 GDScript 交付时必须显式开启 source fallback，或使用独立的非剥离预设。两者是
产品选择，不是 AOT 失败后的静默降级。

## 发布门禁

正式发布流水线必须覆盖：

- C++ 单元、架构规则、sanitizer、真实生成 C++17 编译；
- 官方 Godot 4.4～4.7 plugin load、Debug/Release 导出、运行和 PCK 审计；
- 独立第三方 GDExtension 的两种加载顺序、Attached 继承和无源码运行；
- Windows x86_64 MSVC、macOS、Linux 的真实目标构建；
- Android APK、iOS Xcode/XCFramework 与 Web threads/nothreads 的目标格式审计；
- macOS、Linux、Windows 三个跨版本宿主包的确定性重组、SHA-256、目标白名单和
  schema/runtime ABI 一致性。

Release 标签前不得用历史客户项目名称代替产品级回归说明。客户项目可以作为内部认证语料，
但 changelog 只记录修复的通用能力、边界和验证结果。
