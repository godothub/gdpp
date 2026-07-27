# GDScript 兼容性

本页描述 GDPP 1.8.0 能接受、生成、运行和交付的语言边界。总体状态和优先级见
[当前状态与功能缺口](STATUS.md)，平台证据见[平台验证报告](PLATFORM_TEST_REPORT.md)。

## 兼容模型

GDPP 有两个彼此独立的版本维度：

1. 语言前端跟随最新稳定 GDScript，目前基线是 Godot 4.7 stable。
2. Godot API 按导出目标选择 4.4、4.5、4.6 或 4.7 的独立能力表。

较新的语法不等于较新的引擎 API。编译器可以解析最新版语法，但对 4.4 目标调用 4.7 才出现的
ClassDB 方法仍会在生成 C++ 前报错。

普通编辑和编辑器运行由目标 Godot 自己解析 `.gd`。因此当前对普通 `.gd` 的商业承诺仍要求源码
能被客户所用 Godot 编辑器正常加载。专用于高版本或增强语法的 `.gdpp` ScriptLanguage 尚未
交付，不应依赖它绕过低版本编辑器。AOT 只在导出时接管编译与打包。

官方标准 release 导出模板默认以 `disable_path_overrides=true` 构建，因此 Godot 会禁用
`--script`、`--scene`、`--path` 等路径覆盖入口。这是引擎导出模板的安全边界，不是 GDPP
Script loader 的能力缺失；GDPP 不改写客户模板，也不以开启路径覆盖作为运行前提。发布门禁的
独立 AOT 进程通过 `OS.get_cmdline_user_args()` 选择 oracle，实际客户成品仍从原项目主场景
启动。自定义 Godot 模板若显式启用路径覆盖，编译后的 `.gd` Script 仍可由 GDPP loader 按
`res://` 路径加载。

## 状态定义

| 状态 | 含义 |
|---|---|
| 完成 | 语法、语义、C++17 和至少一条 Godot 运行证据均存在 |
| 主路径 | 常用行为已实现，但边界差分或平台矩阵尚未完整 |
| 失败关闭 | 编译器明确拒绝，不生成猜测代码，也不静默携带源码 |
| 未实现 | 合法 GDScript 能力尚无等价 AOT 实现 |

“完成”只覆盖表内描述的行为，不代表任意项目或任意输入组合已经认证。

## 前端证据

| 门禁 | 当前结果 |
|---|---|
| Godot 4.7 官方合法 parser 语料 | 114 / 114 未被 lexer/parser 拒绝 |
| Godot 4.7 官方非法 parser 语料 | 76 / 76 最终拒绝；接受、超时、崩溃均阻断 CI |
| 固定编译器单元测试 | 544 / 544 |
| 恶意输入 | 非法 UTF-8、NUL、超深递归、超长链、诊断风暴和资源上限均失败关闭 |
| Unicode | Unicode 17.0 XID、关键字 confusable 防护、稳定 ASCII C++ 标识符 |
| 字面量 | 整数基数/边界、浮点非有限值、raw/三引号字符串和 Unicode 转义 |

lexer/parser 已有持续 coverage-guided fuzz、同文件多错误恢复、每类 AST 节点的完整源范围
golden 和资源预算。模糊测试是持续门禁而不是一次性“完成率”；新 stable 仍需刷新官方语料和
漂移快照。

单文件与项目编译 API 会把完整流水线调度到 GDPP 自有的 16 MiB 工作线程栈，并在重入时复用
当前编译线程。语义分析中占用最大的调用和成员分支具有独立帧，因此 Godot 脚本工作线程、客户
原生插件线程或其他嵌入宿主的默认栈大小不会变成额外的语言深度限制。

## 声明与类型

| 能力 | 状态 | 边界 |
|---|---|---|
| `extends`、`class_name`、内部类 | 完成 | 项目路径继承需要 ProjectCompiler 上下文；单文件 CLI 无法独立解析外部脚本图 |
| 字段、常量、静态字段 | 完成 | `_static_init` 惰性执行；扩展卸载释放静态资源 |
| 函数、默认参数、rest 参数 | 完成 | 默认值按调用时/常量时语义分类；typed rest 遵循目标 Godot 规则 |
| 属性 getter/setter | 完成 | 内联和绑定访问器、背字段直接访问及 Inspector 元数据 |
| Signal 声明与调用 | 完成 | 声明、emit、await、发射期连接变更、一次性/延迟/引用计数连接和宿主销毁 |
| enum/bitfield | 完成 | 命名/匿名、常量表达式、导出提示和第三方 ClassDB 枚举 |
| `@abstract` | 完成 | 脚本/内部类/方法、跨脚本义务和动态分派 |
| `@tool` | 完成 | 工具脚本与 runtime 脚本执行域隔离；编辑器专用脚本不能进入 runtime 图 |
| `@icon` | 完成 | `res://`/`uid://`、路径逃逸拒绝和 ClassDB 图标描述 |
| `@static_unload` | 完成（当前目标） | 保留注解与缓存身份；4.4～4.7 按当前 Godot 行为保持静态状态到语言/扩展关闭 |

类型系统覆盖：

- `null`、`void`、`Variant`、所有 Godot Variant 值类型、Object/Ref 可空性；
- 强类型字段、局部变量、参数、返回值、Signal 和 Inspector 属性；
- `Array[T]`、`Dictionary[K,V]` 与十种 PackedArray；
- 项目脚本、内部类、Autoload、第三方 GDExtension 类型和命名枚举；
- 流敏感 `is`/`is not`、`null`、真值与短路分支收窄；
- 编译期不变容器赋值和运行时容器元数据守卫。

嵌套强类型容器按 Godot 自身规则拒绝。动态容器转换、普通/强类型 Array/Dictionary、十种
PackedArray、Object/Ref 与所有 Variant 值家族已进入官方源码/AOT fault 差分；错误包含源码
位置并终止当前函数。Godot 自己跨 patch 会调整诊断措辞，因此兼容合同锁定错误类别、位置、
求值顺序和中止边界，不承诺不同引擎 patch 的日志逐字节恒定。

## 表达式与控制流

| 能力 | 状态 | 边界 |
|---|---|---|
| 算术、比较、逻辑、位运算、幂 | 完成 | 左到右求值、短路和 64 位整数边界由公共 runtime 契约保证 |
| 成员、下标、调用、构造 | 完成 | 静态路径走原生 ABI，动态路径走统一 Variant runtime |
| `is`、`as` | 完成 | Godot 类型、项目脚本、内部类、Attached 脚本与容器约束 |
| 三元表达式 | 完成 | 未选分支不会求值；包含 await 时使用分支 ANF |
| `if`/`elif`/`else` | 完成 | 流敏感类型与显式 CFG |
| `match` | 完成 | 多模式、绑定、数组/字典/rest、guard、await 守卫与正文 |
| `while`/`for` | 完成 | range、数学向量、String、Array、Dictionary、PackedArray 和对象迭代协议 |
| `break`/`continue`/`return` | 完成 | 同步与异步循环恢复边 |
| `assert` | 完成 | Debug 保留、Release 完全移除条件与消息求值 |
| `breakpoint` | 完成 | Debug 进入 Godot 调试器并保留源码帧；Release 不生成调试插桩 |

错误路径采用安全 runtime，避免未定义行为和进程崩溃。除零、模零、越界、失效对象、错误键、
Callable、强类型存储和第三方调用均验证接收者/参数的左到右求值及当前函数中止；调用者继续执行
与官方 GDScript 对照。诊断不承诺跨 Godot patch 逐字节相同。

`breakpoint` 覆盖普通函数、访问器、静态函数、lambda、Attached 第三方 GDExtension 基类和
协程恢复点。调试器可读取原始 `.gd` 路径、函数、当前行、按词法遮蔽后的当前帧局部变量，以及
当前和继承脚本成员。它不等于完整的源码调试器：gutter 行断点、单步控制、所有父帧变量和原生
崩溃符号反查仍在路线图中。

## Callable、lambda 与协程

| 能力 | 状态 | 边界 |
|---|---|---|
| 脚本方法 Callable | 完成 | 实例/静态方法、默认参数、`.call()`、bind/unbind、有效性、相等性和 Signal 生命周期 |
| lambda | 完成 | 创建时值快照、共享容器/Object 身份、嵌套/返回/递归闭包和每次调用独立局部帧 |
| 实例协程 | 完成 | 同步立即值或逐次调用独有 FunctionState；类型化返回、并发调用和跨脚本 await |
| 结构化 await | 完成 | 赋值、参数、容器、短路、三元、循环、match、assert |
| await 默认参数 | 完成 | 按实参缺失顺序在调用帧中求值，可挂起并保留已求值参数 |
| 协程属性访问器 | 完成 | 内联/绑定、实例/静态/内部类，跨脚本 ABI 与缓存失效 |
| 大型恢复链 | 完成 | MIR 扁平状态机和共享帧路径，4996 项/每帧 200 项批处理运行 oracle |
| 静态函数协程 | 完成 | 无实例宿主的独立 FunctionState、真正挂起、类型化返回和 Signal 恢复 |
| lambda 协程 | 完成 | 类型化返回、捕获值、每次调用独立挂起状态及并发逆序恢复 |
| 异步引擎虚函数 | 完成 | 与 GDScript 一样立即返回 FunctionState，并由 Godot 转换为虚函数声明的原生返回类型 |

已知协程的返回值被消费时必须写 `await`；故意丢弃返回值的 detached 调用可以执行。协程身份进入
项目公开 ABI 和精确增量失效，不能把同步旧对象误用于新协程实现。

真正挂起的调用返回引用计数 FunctionState 对象。它公开 `completed(result)`、`resume(arg)`、
`is_valid(extended_check)`，折叠零个/一个/多个 Signal 参数，并在手动恢复或完成时断开旧的一次性
连接。GDPP 的 `await` 同时识别自身状态和 Godot 的 `GDScriptFunctionState`。异步引擎虚函数
没有额外的“等待引擎”协议：官方 Godot 也会把挂起状态立即交给虚函数 Variant→native 转换；
GDPP 在官方 4.7.1 中对状态对象、恢复、完成信号、即时字符串和 continuation 做相同差分。

lambda 与 GDScript 一样在创建 Callable 时捕获当前局部值；标量在每次调用中获得独立工作副本，
Array、Dictionary 与 Object 仍保持共享身份。因此递归通过捕获的共享 `Array[Callable]` 建立，
而“先捕获未赋值 Callable、再把 lambda 赋给它”不会被 GDPP 特判成非 GDScript 的隐式自引用。
真实 Godot 运行矩阵还覆盖两个线程同时调用同一个生成 Callable、延迟 one-shot 信号以及发射期间
断开旧回调并连接新回调。

## 注解

已完成并进行目标/参数检查的注解：

- `@export`、`@export_range`、`@export_enum`、`@export_flags`；
- 2D/3D render、physics、navigation、avoidance layer flags；
- `@export_file`、`@export_global_file`、`@export_dir`、`@export_global_dir`；
- `@export_multiline`、`@export_color_no_alpha`、`@export_node_path`；
- `@export_exp_easing`、`@export_custom`、`@export_storage`、`@export_placeholder`、
  `@export_tool_button`；
- `@export_group`、`@export_subgroup`、`@export_category`；
- `@onready`、`@rpc`、`@abstract`、`@tool`、`@icon`；
- `@warning_ignore`、`@warning_ignore_start`、`@warning_ignore_restore` 和 Godot 4.7 warning 名表。

未支持注解会产生诊断，不会只接受语法后忽略行为。`@static_unload` 保留到生成 metadata；当前
4.4～4.7 目标跟随 Godot 已记录的“脚本不因引用归零卸载”行为，并在扩展终止时清理。若未来
目标 Godot 改变该行为，版本能力表和运行差分必须一起更新。

## 标准库与 Godot API

4.7 能力表索引全部非空 Variant 内建类型、构造器、方法、成员、常量、运算符、全局工具函数、
全局 enum 和整数常量。四版本元数据的每条范围、native meta、参数、属性读写、Signal、enum/
bitfield 和 pointer 禁用规则均自动验证；运行门禁覆盖各 ABI 家族与边界代表。逐 patch、逐重载
的穷举实机组合属于持续认证，不是缺失的编译器语义分支。

专用 GDScript intrinsic 已覆盖：

`range`、`len`、`load`、`preload`、`convert`、`type_exists`、`char`、`ord`、`Color8`、
`is_instance_of`。

API 规则：

- 目标 patch 版本归一到对应 minor 能力表，但不支持的 minor 会明确拒绝；
- 类型、构造器、方法、属性、虚函数、单例、工具函数、enum 和默认参数均从同一快照生成；
- custom engine/double precision 使用独立构建产物，绑定目标 API SHA-256、precision、版本、
  godot-cpp 与 runtime ABI；标准包不会与 custom SDK 混用；
- 静态强类型调用使用精确 native meta，动态对象仍通过 Godot ABI，不猜测 C++ 布局。

## 项目级能力

| 能力 | 状态 |
|---|---|
| 跨脚本继承、常量、enum、方法、字段和类型 | 完成 |
| 循环引用的普通跨脚本引用 | 完成；头文件/前置声明稳定 |
| 继承环、缺失基类、ABI 不安全 override | 失败关闭 |
| Autoload（脚本与场景） | 完成 |
| PackedScene、Resource、内嵌/共享/循环资源图 | 完成主路径 |
| 动态 ShaderMaterial/Shader 参数 | 完成主路径并有 Attached 集成 fixture |
| HTTP/WebSocket、二进制消息、网络图片 | 完成主路径并有 Windows 端到端 fixture |
| 字面量 `.gd` preload/load 与脚本 `.new()` | 完成 |
| 动态拼接 `.gd` 路径 | 完成；从全项目编译清单解析，清单外路径确定失败 |
| 第三方 addon 内 `.gd` | 与项目脚本统一 AOT，不按目录跳过 |
| 第三方 `.gdextension` | Godot 原样加载和导出 |

脚本发现以项目根为边界，忽略 GDPP 自己的受控构建/SDK 目录，不会因为位于 `addons/` 就跳过
客户或第三方 GDScript。

每个已编译脚本还登记唯一规范 `Script` 资源身份。`load()`、`ResourceLoader`、相对路径、
`uid://`、线程加载、缓存、`exists()`、`get_script()` 和 `.new()` 即使路径在运行时拼接也使用
同一二进制清单；不存在的路径返回确定错误，成品无需保留 `.gd`。

## 第三方 GDExtension

第三方类由编辑器进程中的 ClassDB 自动采集。GDPP 支持公开属性、方法、静态方法、Signal、
常量、命名 enum/bitfield、Engine 单例，以及 `extends VendorNode` Attached 继承。项目动态库：

- 不需要供应商源码或头文件；
- 不链接供应商库；
- 不跨动态库建立 C++ 继承；
- 外部 `super` 使用精确 MethodBind compatibility hash；
- 不依赖 provider 与项目扩展的加载顺序。

CLI 无 Godot 进程时可以消费机器生成的 `gdpp_bridge.json`。私有/未注册 C++ API 会失败关闭。
当前仍缺供应商目标二进制内容摘要的统一导出锁定、更多真实供应商与全平台升级矩阵。

## 目标平台

| 目标 | 架构/模式 | 构建状态 | 运行认证边界 |
|---|---|---|---|
| Windows | x86_64 | 正式交付 | 官方 Godot、MSVC、最终 ZIP 安装/导出/运行 |
| macOS | Universal 2 | 正式交付 | 官方 Godot、AppleClang、双切片和最终 ZIP |
| Linux | x86_64 | 正式交付 | 官方 Godot、GCC、ELF 与运行 oracle |
| Android | arm64-v8a | 交付 | APK 构建/审计；真机与商店流程未认证 |
| iOS | device arm64、Simulator arm64/x86_64 | 交付 | XCFramework/Xcode 无签名模拟器；真机/App Store 未认证 |
| Web | wasm32 threads/nothreads | 交付 | Chromium 两模式；其他浏览器/CDN 未认证 |

未交付架构：Windows arm64、Linux arm64、Android x86_64。

## 低版本 Godot 与新版语法

GDPP 的编译器前端不是调用目标 Godot 的 GDScript parser，因此 AOT 语法能力不被 4.4 parser
实现限制；目标引擎 API 仍严格受 4.4 能力表限制。但普通 `.gd` 在编辑期仍会被 4.4 自己解析。
当前产品不拦截编辑器 parser，也不在平时替换 ScriptLanguage，因此：

- 使用目标 Godot 已接受的 `.gd` 是正式支持路径；
- CI 中少数已知低版本 parser 诊断（例如 4.4 的 typed variadic 与新版嵌套块遮蔽规则）只有
  在 AOT 导出、运行和精确到文件/行号/诊断的 allowlist 全部通过时才可视为非权威提示；
- 未来高版本/增强语法应使用独立 `.gdpp` 后缀和 ScriptLanguage，当前版本未交付该机制；
- 无源码 AOT 成品不会包含原始 `.gd`，低版本运行时不会再次解析这些脚本。

## 失败关闭

启用 `gdpp/strip_gdscript_sources=true` 且
`gdpp/allow_source_fallback=false` 时，下列情况阻断导出：

- 未实现语法/语义或不支持的 Godot API；
- 目标 SDK、工具链、架构或 runtime ABI 不匹配；
- 项目/第三方契约不完整或发生冲突；
- 场景、Resource、Autoload 或描述符转换不完整；
- C++ 编译、链接、目标库加载或运行描述符失败；
- PCK/目录仍包含源码、compiler、SDK、生成物或错误数量的项目库。

显式 source fallback 是单独的交付选择，不是 AOT 失败后的自动降级。
