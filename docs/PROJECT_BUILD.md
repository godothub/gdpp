# 项目构建流程

## 商业用户依赖

正式用户只需 Godot 与目标平台 C++ 工具链。插件不会调用 CMake、Python、Ninja、SCons，
也不会在用户机器上生成或编译 godot-cpp。

| 平台 | 必需工具链 | 已交付架构 |
|---|---|---|
| Windows | MSVC Build Tools、Windows SDK | x86_64 |
| macOS | Xcode Command Line Tools | arm64 / Universal 2 |
| Linux | GCC 或 Clang | x86_64 |
| Android | Android NDK r28+ | arm64-v8a |
| iOS | 完整 Xcode | device arm64、Simulator arm64/x86_64 |
| Web | Emscripten | wasm32 threads / nothreads |

Windows arm64、Linux arm64 与 Android x86_64 尚未交付。导出预检会在生成或编译客户源码前
失败关闭，不会尝试用错误架构的静态库完成链接。

## 两个彼此隔离的原生角色

GDPP 只有两个原生 ABI 域：

1. compiler 插件运行在 Godot 编辑器进程中，开发 GDPP 时使用 godot-cpp 的 `editor` target
   构建。editor 静态库只属于 GDPP 自身构建树；发行包只携带已经链接完成的 compiler 动态库。
2. 客户项目运行时由导出目标加载，客户 SDK 只携带一份优化后的
   `template_release` godot-cpp 静态库。Debug 与 Release 导出都复用这份 ABI 绑定。

客户 SDK 不包含 `editor` 或 `template_debug` 静态库。compiler 动态库内部已经链接的代码不会
参与客户项目链接，也不会让客户源码再次编译。

```text
Godot 编辑器
└── gdpp_compiler.<host>          # 预编译，客户不重建

客户 SDK/<Godot 版本>/<target>
└── libgodot-cpp.*.template_release.*
                                  # 唯一项目链接绑定
```

Debug 导出仍保留 GDScript 的 `assert()` 等调试语义；这是 GDPP 的
`GDPP_SCRIPT_DEBUG_ENABLED` 代码生成契约，与 godot-cpp 的构建 target 无关。两种导出均使用
优化、死代码删除和符号白名单，只在脚本调试语义上不同。

## 零源码改动的 Attached 模型

所有兼容 `.gd` 在导出时统一生成 Attached 行为类，而不是把脚本声明成新的 Godot 原生
Node/Resource 子类：

```text
原场景或资源中的真实对象
    ├── Godot Node/Resource，或第三方 GDExtension 类
    └── AttachedCompiledScript
        └── 生成的 C++17 行为、字段、方法、属性、Signal、RPC
```

因此客户项目、场景、资源、Autoload 和第三方 GDExtension 都不需要改动。真实对象继续由
Godot 或供应商插件创建和拥有；GDPP 的 `ScriptLanguageExtension`、`ScriptExtension` 与
`ScriptInstance` 只提供原脚本行为。

导出前，compiler 插件从主线程快照 ClassDB 中的第三方类契约。项目编译器随后直接从语义图生成
declaration-local metadata，并通过 metadata-only ScriptExtension 暂时向编辑器描述生成脚本的
属性、方法、Signal 和继承关系。这个过程不为反射加载客户 `.gd`，因此不会执行静态初始化或保留
互相引用的 GDScript 资源；也不加载客户目标动态库、不生成 editor 项目库。后台编译线程只消费
快照，不访问实时 ClassDB 或 UI。

场景/Resource 转换时，每个临时 ScriptInstance 只把原 SceneState 实际保存的字段标记为
`PROPERTY_USAGE_STORAGE`。未覆盖字段不写入包内，由目标 C++ 行为构造器执行原脚本默认初始化，
避免空类型化容器或 `nil` 覆盖真实默认值。

外部 `.gd`、`.tres` 子资源和 `.tscn` 内嵌资源都按准确 `resource_path` 建立脚本身份。跨脚本
`is`/`as`、Autoload、内部类以及字段和方法分派统一查询 Attached ScriptInstance，不把行为类
指针错误当成真实 Godot 对象指针。

## 单次导出构建

普通编辑、运行和资源导入继续使用客户原有 `.gd`，GDPP 不在这些阶段构建项目动态库。只有用户
执行启用 AOT 的导出时，插件才运行以下事务：

```text
扫描客户脚本
    -> 解析和语义分析
    -> 生成 Attached GDExtension C++17
    -> 为所选导出 profile 顺序编译每个翻译单元一次
    -> 链接一个目标项目动态库
    -> 安装导出期 metadata 描述
    -> 转换 PackedScene、Resource 与 Autoload 的脚本附着
    -> 写入目标运行描述符并剥离 .gd
    -> 交还 Godot 执行普通打包
```

一次 Debug 导出生成一个 `gdpp.debug.*`，一次 Release 导出生成一个
`gdpp.release.*`。同一次导出不会先构建 editor/development 项目库，也不会同时构建
Debug 与 Release。`register_types.cpp`、GDPP runtime 和每个生成的 `.gd.cpp` 在所选目标中各有
一个对象文件。

编译和链接命令严格串行执行，Windows 隐藏 `cl.exe`、`link.exe` 与唯一一次 MSVC 环境引导
窗口。构建在后台线程运行；编辑器主线程持续处理窗口、渲染和导出协调，因此进度覆盖层能实时
刷新且不会占用 Godot 主循环。

一个连续进度条覆盖扫描、解析、语义分析、脚本预编译、原生文件生成、C++ 编译和链接。每个
阶段占固定区间，逐文件阶段继续按文件数量细分，并显示 `(当前/总数)`。

## SDK 与缓存

发行物按桌面宿主固定为 `gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip` 三个包。每个包只
携带当前宿主的 compiler/fallback 和桌面 SDK，但同时包含 Godot 4.4～4.7 全部版本。三个包
均包含 Android arm64 与 Web threads/nothreads；mac 包另外包含 iOS device 和 Universal
Simulator。每个平台/架构/线程模式都只允许一份 `template_release` 绑定。

SDK schema 11 固定：

- Godot API、平台、架构和最低系统；
- C++17、异常关闭、编译器族和 MSVC 静态 CRT；
- `debug,release` 两个可选项目 profile；
- 唯一 `distribution_binding template_release` 与 Release 优化；
- runtime ABI 13 及全部运行时头/源文件 SHA-256；
- Android STL/API、iOS slices、Web 线程模式等目标契约。

NativeBuilder 在创建第一条编译命令前验证整个清单。错误 SDK、旧 schema、混入 editor/debug
绑定、损坏 runtime 或错误工具链都会失败关闭。

对象缓存位于：

```text
addons/gdpp/build/project/native-direct/
└── <api>/<platform>/<arch>[/<web-mode>]/<debug|release>/objects/
```

缓存签名包含构建策略、API、平台、架构、profile、编译器绝对路径和可复现路径映射；翻译单元
还使用真实 include/depfile 与 SDK、桥接契约作为输入。Debug 与 Release 的对象隔离，但它们
共享项目级前端清单和生成源，普通脚本在同一次导出中不会重复解析或生成。

## 产物边界

| 目录 | 内容 | 是否进入插件发行包 | 是否进入成功游戏导出 |
|---|---|---|---|
| 根 `build/<preset>/` | GDPP 自身 CMake 构建与 QA 证据 | 否 | 否 |
| `addons/gdpp/build/` | 生成 C++、清单、对象缓存 | 否 | 否 |
| `addons/gdpp/binary/` | compiler、fallback、当前项目目标库 | 按角色筛选 | 仅所选项目目标库 |
| `addons/gdpp/sdk/` | 客户目标头文件、runtime、唯一静态绑定 | 是 | 否 |
| `.godot/` | Godot 缓存和导出事务备份 | 否 | 否 |

编辑态只有 `addons/gdpp/gdpp.gdextension` 一个物理描述符。导出事务暂时把同一路径改成目标扫描
描述符，包内同一路径写入项目运行描述符，结束或异常恢复后还原 compiler 描述符。成功导出只
携带一个匹配 profile/平台/架构的项目动态库，不携带 compiler、fallback、SDK、生成源码、对象
或客户 `.gd`。

## 第三方 GDExtension

供应商描述符与二进制保持原样。GDPP 不读取或修改供应商源码，不要求供应商头文件，不链接
供应商库，也不尝试跨动态库继承供应商 C++ 类型。compiler 从 ClassDB 捕获公开契约；CLI 可用
`gdpp_bridge.json` 提供等价离线契约。

项目运行时注册 Attached 行为不依赖 provider 加载顺序；真正实例化脚本时才验证目标原生类。
外部 `super` 通过精确 MethodBind compatibility hash 调用。契约不完整、供应商类缺失或 ABI
不匹配时阻断无源码导出，不退化成猜测式 C++ 调用。

## 失败关闭

启用 `gdpp/strip_gdscript_sources=true` 且未显式允许源码回退时，下列任一情况都会阻断商业
导出：

- 前端、语义或生成代码失败；
- SDK/工具链/目标 ABI 不匹配；
- 任一 C++ 编译或链接命令失败；
- metadata bridge、ClassDB 类或第三方契约校验失败；
- 场景、资源、Autoload 不能原子替换；
- 目标库、运行描述符或导出后源码审计不符合唯一性约束。

只有用户显式设置 `gdpp/allow_source_fallback=true`，或使用关闭源码剥离的独立预设，才允许
回到普通 GDScript 交付。
