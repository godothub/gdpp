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
| `core` | Source、SourceSpan、诊断、Godot 目标版本 |
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

## 前端与语义

AST 的表达式、语句和 match pattern 使用有名结构的 `std::variant`，而不是通用
`kind + value + children` 存储。每个节点保留 `SourceSpan`。语义层统一完成：

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

HIR 拥有解析后的类型、成员身份、调用契约、迭代计划、RPC 配置和源码范围。原始
`AwaitExpression` 在 HIR lowering 中变为 A-normal form：

- 挂起前按源码顺序物化接收者、索引和参数；
- 短路和三元只降低被选择分支；
- `while` 条件每轮重新计算；
- match 先绑定模式，再执行可挂起 guard；
- assert 条件和惰性消息使用独立 debug-only 前缀；
- 跨挂起写入的循环局部值和参数提升到共享状态。

MIR 为每个方法、getter、setter 和 lambda 建立显式基本块、前驱、终止指令与副作用。分支通过
`BranchRole` 区分普通条件、迭代协议、match 模式、guard 和断言。verifier 在优化前后检查：

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
- 动态 Signal/Callable、await 和跨脚本调用经统一 runtime ABI。

该设计避免 Godot 尚不支持的跨 GDExtension C++ 继承，也不需要供应商头文件或链接库。

## 项目编译

`ProjectCompiler` 一次扫描项目范围内全部客户 GDScript，包括第三方 addon 的 `.gd`，只排除
GDPP 自己的受控 build/SDK 区域。它建立：

- 路径稳定的脚本身份、全局类和递归内部类；
- 继承拓扑、普通跨引用和 Autoload；
- 实现哈希、公开 ABI 哈希、依赖边和 compiler/codegen 指纹；
- 每个脚本的生成头/源、metadata 和桥接契约；
- 源码/目标版本/第三方契约变化的精确失效集合。

manifest 只在完整编译成功后提交。陈旧生成单元按受控白名单清理，外部路径、符号链接和异常
清单名不能逃出输出目录。对象缓存再通过 depfile、SDK/runtime 摘要、工具链和 profile 校验。

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

客户导出不调用 CMake、Ninja、Python 或 SCons。NativeBuilder 验证 SDK schema 11/runtime
ABI 13 后，直接生成系统工具链命令：

- Windows：MSVC/Windows SDK、x86_64、静态 CRT；
- macOS：AppleClang、arm64/x86_64/Universal 2；
- Linux：GCC 或 Clang、x86_64；
- Android：NDK clang、arm64/API 28/`c++_shared`；
- iOS：device arm64、Simulator arm64/x86_64、动态 XCFramework；
- Web：Emscripten wasm32，threads/nothreads 隔离。

每个平台/架构/线程模式只有一份优化的 `template_release` godot-cpp 静态绑定。Debug 和 Release
导出都链接它；区别是 GDPP 是否保留脚本调试语义。每个翻译单元与链接命令严格串行，后台线程
执行，主线程负责 UI 刷新和导出协调。

项目库文件使用 `gdpp.<debug|release>.<platform>.<arch>` 前缀；唯一公开 C 入口仍是
`gdpp_project_library_init`。文件名与入口符号是不同契约。

## 描述符与导出事务

源项目中只有 `addons/gdpp/gdpp.gdextension` 一个活动物理描述符。导出器：

1. 备份 compiler 描述符、extension registry、Autoload 和必要的供应商扫描描述；
2. 生成并构建当前目标的一个项目库；
3. 临时把同一描述符切换为目标扫描内容；
4. 用 metadata-only Script 描述转换场景、Resource 和 Autoload；
5. 把项目运行描述符写入 PCK 同一路径并剥离客户 `.gd/.gdc`；
6. 成功、失败或下次插件启动时恢复源工程字节。

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
