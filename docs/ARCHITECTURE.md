# 架构

GDPP 是“导出期编译器 + Attached Script runtime + 直接原生构建器”。编译器核心不依赖
Godot 对象；Godot 集成层只负责 ClassDB 快照、编辑器/导出生命周期、路径和子进程。

```text
项目 .gd
  -> lexer / parser / 强类型 AST
  -> 项目符号图与目标 Godot API
  -> semantic model
  -> 类型化 HIR
  -> CFG / MIR / verifier / optimizer
  -> GDExtension C++17
  -> NativeBuilder 直接编译与链接
  -> 单个 gdpp.<profile>.<platform>.<arch> 项目库
  -> 场景/Resource/Autoload Attached 转换
  -> Godot 标准导出与无源码审计
```

普通编辑、资源导入和编辑器内运行不经过这条流水线，继续使用客户原来的 GDScript。只有启用
GDPP AOT 的导出触发编译和原生构建。

## 模块边界

```text
core -> frontend -> semantic -> ir -> codegen -> compiler -> project
numeric -----------------------------------------------> runtime / generated code
support ---------------------------------------------------------------> project
compiler + project + runtime --------------------------------> integration/godot
compiler + project -----------------------------------------------------> cli
```

| 模块 | 职责 |
|---|---|
| `core` | Source、SourceSpan、诊断、Godot 目标版本、确定编译线程栈 |
| `numeric` | 纯 C++17 的 64 位整数、移位、除余和 range 边界契约 |
| `frontend` | Unicode/token、lexer、parser、强类型 AST、语言特性表、常量求值 |
| `semantic` | 类型、作用域、流收窄、intrinsic、API/项目/第三方符号解析 |
| `ir` | 类型化 HIR、await ANF、CFG/MIR、verifier 和保守优化 |
| `codegen` | 只消费已验证 IR 的 GDExtension C++17 emitter |
| `compiler` | 单翻译单元流水线、诊断和阶段指标 |
| `project` | 脚本发现、依赖图、增量清单、Extension bridge 和 NativeBuilder |
| `runtime` | 生成代码 ABI：Variant、动态分派、容器、迭代、协程和 Attached Script |
| `support` | UTF-8 路径、SHA-256 等无业务状态工具 |
| `integration/godot` | compiler GDExtension、ClassDB 快照和 export fallback |
| `cli` | 离线编译宿主 |

`tools/check_architecture.py` 检查模块目录、公共头布局和 include 方向。`runtime` 不依赖编译器；
编译器核心不包含 Godot 编辑器状态。

`Compiler` 与 `ProjectCompiler` 的公开入口统一经 `core` 调度到 16 MiB 工作线程栈：Windows
使用 `_beginthreadex`，POSIX 使用 `pthread`，线程局部重入标记避免递归创建工作线程。语义层
把互斥的调用与成员分析分支隔离为独立帧，从而不继承 Godot 约 512 KiB 的脚本工作线程栈，也
不依靠修改编辑器或客户进程的全局栈配置。

## 前端与语义

AST 的表达式、语句和 match pattern 使用有名结构的 `std::variant`，而不是通用
`kind + value + children` 存储。每个节点保留 `SourceSpan`；恢复解析合并范围时始终保留首节点
的完整范围，并且不会让缺失尾部节点把结束位置倒退到尾随空白之前。语义层统一完成：

- GDScript 类型、可空性、值/共享容器/Object/Ref 所有权分类；
- 字段、函数、Signal、enum、属性访问器、注解和内部类验证；
- Godot 4.4～4.7 版本化 API 重载、属性、虚函数和 ABI meta 选择；
- 项目脚本、Autoload、脚本 Resource、第三方 ClassDB 契约和依赖记录；
- `is`/`null`/真值控制流收窄；
- `Array[T]`、`Dictionary[K,V]` 和 PackedArray 的唯一容器规则；
- `range`、load/preload、语言 utility 和动态分派 intrinsic；
- RPC、协程身份和跨脚本公开 ABI。

后端不得重新解释注解字符串、拆分类型文本或猜测 iterable。语义不完整时在 IR/代码生成前失败。

## HIR 与 MIR

HIR 拥有解析后的类型、成员身份、调用契约、迭代计划、RPC 配置和源码范围。局部声明、参数、
迭代/match 绑定、identifier 引用和调试可见变量共享稳定 `FlowSymbolId`，因此遮蔽、lambda 捕获
与跨 await 提升不会退化为按源码名字关联。原始
`AwaitExpression` 在 HIR lowering 中变为 A-normal form：

- 挂起前按源码顺序物化接收者、索引和参数；
- 短路和三元只降低被选择分支；
- `while` 条件每轮重新计算；
- match 先绑定模式，再执行可挂起 guard；
- assert 条件和惰性消息使用独立 debug-only 前缀；
- breakpoint 携带精确词法作用域快照，不由 C++ emitter 重新推断可见绑定；
- 跨挂起写入的循环局部值和参数按稳定符号身份提升到共享状态；lambda 创建时物化捕获快照，
  每次调用再建立独立可写帧。

MIR 为每个方法、getter、setter 和 lambda 建立显式基本块、前驱、终止指令与副作用。分支通过
`BranchRole` 区分普通条件、迭代协议、match 模式、guard 和断言；`debug_breakpoint` 以
`observes_debugger` 副作用阻止优化器错误删除或跨越调试点。verifier 在优化前后检查：

- 唯一入口、有效目标和精确前驱；
- return/jump/branch/suspend/stop 的合法形状；
- suspend 只有一个恢复边；
- 迭代计划、协程和 HIR 载荷一致；
- 不可达块删除后的重新编号与边完整性。

当前 MIR 不是完整 SSA。常量折叠、字面量死分支、不可达块和基础 CFG 简化已经进入主链；稳定
Value ID、所有权数据流、逃逸分析、CSE 和统一协程帧仍在路线图中。

## Attached Script runtime

GDPP 不把每个脚本变成另一个 Godot 原生 Node/Resource 子类。原对象仍由 Godot 或供应商
GDExtension 创建，生成行为通过下列层附着：

```text
真实 Godot/供应商 Object
  -> AttachedCompiledScript (ScriptExtension)
  -> AttachedScriptInstance
  -> 生成的 AttachedScriptBehavior
```

生成项目库注册 `ScriptLanguageExtension`、`ScriptExtension`、`ScriptInstance` 和行为描述：

- 字段、方法、属性、Signal、RPC 和脚本继承由行为提供；
- 原生生命周期、原生属性/Signal 和供应商对象身份保持不变；
- 项目脚本 `is/as` 查询附着脚本链，不把行为指针当 Object；
- 脚本 override 从供应商回调进入 Attached dispatch；
- 外部 `super` 通过 ClassDB 的精确 MethodBind compatibility hash；
- provider 类型在实例化时解析，不依赖扩展注册表的偶然加载顺序；
- 动态 Signal/Callable、FunctionState await 和跨脚本调用经统一 runtime ABI。

该设计避免 Godot 尚不支持的跨 GDExtension C++ 继承，也不需要供应商头文件或链接库。

### 调试器桥接

Debug 生成代码在函数入口登记轻量原生帧，并在语句边界更新原始 `.gd` 行号。只有 Godot
`EngineDebugger` 已连接并注册当前语言时帧才进入线程局部栈；Release 编译不包含这些调用。
执行 `breakpoint` 时，runtime 通过 `ScriptLanguageExtension` 提供：

- 原始脚本路径、函数名、当前行和规范 `ScriptInstance`；
- 当前词法作用域内的参数、局部变量和常量，内部遮蔽优先；
- 当前脚本及继承脚本字段；
- 以同一 Attached owner 为宿主的调试表达式求值。

静态函数没有实例宿主；协程在原生入口帧已经返回后可为恢复点建立临时顶层帧。当前没有把编辑器
gutter 行断点和全部单步协议冒充为已完成能力。

## 项目编译

`ProjectCompiler` 一次扫描项目范围内全部客户 GDScript，包括第三方 addon 的 `.gd`，只排除
GDPP 自己的受控 build/SDK 区域。它建立：

- 路径稳定的脚本身份、全局类和递归内部类；
- 继承拓扑、普通跨引用和 Autoload；
- 实现哈希、公开 ABI 哈希、依赖边和 compiler/codegen 指纹；
- 每个脚本的生成头/源、metadata 和桥接契约；
- 源码/目标版本/第三方契约变化的精确失效集合。

manifest 只在完整编译成功后提交。陈旧生成单元按受控白名单清理，外部路径、符号链接和异常
清单名不能逃出输出目录；原地升级还会精确清除已退役的 `gdpp_project.gdextension` 与 CMake
项目脚手架，防止旧入口 ABI 与当前直接构建 manifest 并存。对象缓存再通过 depfile、SDK/runtime
摘要、工具链和 profile 校验。

跨进程项目锁尚未实现；同一项目不能同时由多个编辑器/CLI 安全写入同一构建目录。

## Extension bridge

编辑器主线程在第三方 GDExtension 注册后从 ClassDB 生成不可变契约快照：

- 类型、Godot 基类和 editor-only 状态；
- 属性、实例/静态方法、默认参数、Signal；
- 常量、命名 enum/bitfield；
- 方法 compatibility hash 和确定性类契约哈希。

ProjectCompiler 后台只消费快照，不访问实时 ClassDB。离线 CLI 可用 `gdpp_bridge.json`。实际被
脚本引用的契约进入缓存身份；私有或未注册 API 失败关闭。供应商目标动态库内容摘要尚未成为
统一 bridge 锁的一部分，属于当前供应链缺口。

## NativeBuilder

客户导出不调用 CMake、Ninja、Python 或 SCons。NativeBuilder 验证 SDK schema 12/runtime
ABI 18 后，直接生成系统工具链命令：

- Windows：MSVC/Windows SDK、x86_64、静态 CRT；
- macOS：AppleClang、arm64/x86_64/Universal 2；
- Linux：GCC 或 Clang、x86_64；
- Android：NDK clang、arm64/API 28/`c++_shared`；
- iOS：device arm64、Simulator arm64/x86_64、动态 XCFramework；
- Web：Emscripten wasm32，threads/nothreads 隔离。

每个平台/架构/线程模式只有一份优化的 `template_release` godot-cpp 静态绑定。Debug 和 Release
导出都链接它；区别是 GDPP 是否保留脚本调试语义。每个翻译单元与链接命令严格串行，后台线程
执行，主线程负责 UI 刷新和导出协调。

项目库文件使用 `gdpp.<debug|release>.<platform>.<arch>` 前缀；唯一公开 C 入口固定为
`gdpp_library_init`。文件名与入口符号是分别校验的两个契约。

动态语言失败使用线程局部 script fault frame。越界、除零、失效对象、Callable 参数、严格强类型
存储和第三方扩展调用会记录原始 `.gd` 路径/行列并终止当前生成函数的后续求值，不让 C++ 异常或
未定义行为跨越 GDExtension ABI；外层 GDScript 调用者仍按目标返回类型的默认值继续执行。

同步生成函数进入时把自己的 fault state 压入当前线程，退出时恢复上一层；状态只由该活动调用
线程读写，因此不需要在每个表达式后执行原子操作。真正挂起的协程把 fault state 放入
FunctionState，共享状态只能在协程恢复互斥锁持有期间安装到恢复线程；它可以跨线程迁移，但不能
被两个线程并发执行。同步函数的检查器只在该同步栈帧内直接引用状态；协程、协程 lambda 和恢复
continuation 使用无捕获检查器读取当前线程已安装的状态，禁止把同步 `ScriptFunctionScope&` 复制
进可逃逸闭包。嵌套普通函数和 lambda 默认建立独立状态，初始化事务等明确要求继承失败的边界才
复用外层状态。

轮询位置不是基于函数名的热点特判。C++ 后端在已经解析的类型和调用 resolution 上计算保守的
故障效应：动态调用/运算、严格 Variant→强类型转换、容器下标、整数除法/取模、对象有效性检查、
Callable/Signal 重入和未知外部边界视为可能失败；字面量、普通标识符、精确类型存储、安全整数
运算和不进入脚本的 String 值方法可证明不会设置 fault state。复合表达式按左到右求值把效应向
外传播，三元和短路分支分别保留自己的边界；不确定时必须保留检查。生成器只据此删除冗余轮询，
不会删除表达式、副作用或运行时验证。

静态 `Dictionary[key]` 和 `Dictionary.key` 分别使用 Godot Variant 的 keyed/named ABI。
两条 ABI 都在一次查找中返回独立有效位，因此合法的“键存在且值为 `null`”不会与缺失或强类型
不兼容键混淆，也不需要 `get()` 后再执行 `has()`。点号路径直接缓存 `StringName`；强类型
Dictionary 先验证该键能进入声明的键存储，并把声明值类型保留到 typed IR 和生成 C++。若接收者
本身可失败，生成代码先固定并检查接收者，再查找，因此不会用默认空字典覆盖原始故障。

直接 Dictionary 写入使用返回有效位的受检 setter，缺失类型契约或只读存储会设置当前 fault
frame。复合写入按“受检读取→右值→运算→写回”执行；官方 GDScript 对强类型复合值拒绝会保持
原条目并继续，而只读写回仍会终止当前函数，runtime 明确区分这两个结果。后端只有在全函数分析
能以 `FlowSymbolId` 证明目标来自局部非强类型字面量、键初始存在、且没有重赋值、别名、逃逸、
下标、方法访问或闭包捕获时，才把读取降为借用 `const Variant&`，并把复合写入降为原地
`Variant&` 更新；任何不确定路径保留完整受检 ABI。证明只选择等价后端路径，不会跳过 typed
Dictionary 键/值合同、接收者求值或可能失败的动态边界。

当复合更新的现有槽位和右值都精确为 `Variant::INT` 时，runtime 使用 godot-cpp 的
`VariantInternal` 取得同一 int64 存储，并以公共 `integer` 合同原地执行加减乘除、取模、移位
及位运算。运算结果不会经过临时 Variant；除零/模零在写入前设置带源码位置的 fault，幂、非整数
或未知运算继续调用 Godot 的通用 Variant ABI。该路径不会改变槽位类型，也不会绕过证明或
Dictionary 有效性检查。

native-to-Variant 适配按值类别保留所有权：现有 Variant lvalue 直接借用，Variant rvalue 转移，
其他原生值才构造新的 Variant。普通赋值仍先固定接收者、再只求值一次 RHS；该 RHS 是编译器拥有
且提交后不再观察的快照，因此 String、Variant、容器等引用支持值可以移动进入受检转换和最终
存储。动态失败边界先完成转换并轮询 fault 再提交；Dictionary 自赋值和 godot-cpp 不安全的移动
赋值继续由 `assign_native_storage` 强制走受保护复制 ABI。

本地 lambda 的必选参数数、位置参数数和变参属性编码在生成的 `LocalCallable` 类型中。仍处于
创建调用栈内的具体适配器保存原生参数 tuple，类型完全一致时直接传给 lambda；类型不一致、缺省
参数、结构化容器和动态值仍走严格 Variant 转换。复制/移动赋值到另一个 Callable 状态会清除
直达资格，返回、Signal 和其他逃逸路径只保留 Godot `CallableCustom`，重新执行 ObjectDB 有效性
和参数检查。实例直达路径依赖 Godot 对当前正在调用对象的锁定生命周期，静态 lambda 则没有宿主。

## 描述符与导出事务

源项目中只有 `addons/gdpp/gdpp.gdextension` 一个活动物理描述符。标准 AOT 导出不改写该文件：

1. 生成并构建当前目标的一个项目库；
2. 在导出文件回调中跳过 editor-only 物理描述符，并向包内同一路径写入 runtime 描述符字节；
3. 由 GDPP 通过公开导出 API 只注册一次项目库，避免内置 GDExtension scanner 重复加入；
4. 对需要 Universal 2 归一化的 provider 验证真实 Mach-O 切片，只虚拟化包内描述符；
5. 用 metadata-only Script 描述转换场景、Resource 和 Autoload，并剥离客户 `.gd/.gdc`；
6. 成功或失败时恢复临时 Autoload/兼容回退状态；启动时仍可恢复 1.7.8 及更旧版本留下的备份。

项目库标记为不可热重载。重新导出/重启是当前安全边界，不尝试让现有 Attached 实例跨动态库
世代迁移。

## 产物边界

| 位置 | 内容 | 插件 ZIP | 成功游戏 |
|---|---|---:|---:|
| 根 `build/<preset>/` | GDPP 自身构建与 QA | 否 | 否 |
| `addons/gdpp/build/` | 生成 C++、manifest、对象和事务临时文件 | 否 | 否 |
| `addons/gdpp/binary/` | compiler/fallback/当前项目库 | 筛选 compiler/fallback | 仅一个项目库 |
| `addons/gdpp/sdk/` | 头文件、runtime、静态绑定和 manifest | 是 | 否 |
| `.godot/` | Godot 缓存和事务备份 | 否 | 否 |

最终 PCK/目录审计拒绝 `.gd/.gdc`、compiler、fallback、SDK、静态库、生成 C++、对象和多余项目
动态库。

## 失败与安全边界

编译、语义、IR、SDK、工具链、第三方契约、C++ 编译/链接、描述符、资源转换或内容审计任一步
失败都会阻断严格 AOT 导出。只有用户显式允许 source fallback 才可交付 GDScript。

原生化提高脚本逆向门槛，但不加密资源、反射元数据、业务字符串或客户端内存。签名、公证、
SBOM、provenance 和符号服务属于尚未完成的供应链层，不应由架构描述暗示已经提供。
