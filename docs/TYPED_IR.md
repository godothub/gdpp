# 类型化 AST、HIR 与 MIR

GDPP 使用三层语言表示：强类型 AST、语义化 HIR 和显式控制流 MIR。每层只承担自己的职责；
C++ 后端不能重新解析 token、注解字符串或类型文本。

## AST

parser 直接构造有名节点：

- 表达式：literal、identifier、unary、await、binary、call、member、subscript、conditional、
  node reference、Array/Dictionary、lambda；
- 语句：expression、return、variable/const、assert、assignment、if、match、while、for、
  pass、break、continue、breakpoint；
- match pattern：value、wildcard、binding、rest、Array、Dictionary；
- 声明：字段、函数、Signal、enum、内部类、脚本注解。

节点载荷存放在 `std::variant`，每个节点拥有 `SourceSpan`。`kind()/value()/operand()` 是只读迁移
适配器，不决定实际布局。新增语法应增加明确结构和 visitor 分支，而不是扩充通用 children 约定。

## HIR

语义分析把 AST 降低为独立拥有数据的 HIR。每个表达式至少保留：

- 最终语义类型、存储类型和赋值类型；
- null/非空证明；
- 源码范围；
- 静态/动态成员解析类别与 owner；
- getter/setter、调用参数契约、coroutine call 标记；
- Godot intrinsic、项目/第三方脚本身份和容器签名。

局部声明、参数、迭代/match 绑定和所有解析到局部存储的 identifier 表达式还携带稳定
`FlowSymbolId`。源码名字只用于诊断和生成可读标识符；lambda 捕获、跨 await 提升、同名遮蔽和
调试器变量查找都按符号身份关联，不能把两个同名词法绑定合并为同一运行时单元。

声明保留字段所有权、Inspector 元数据、onready/static/const、Signal 参数、函数返回、rest/default
参数、RPC 配置和脚本执行模式。项目符号、ClassDB 和目标 API 的解析结果只在语义阶段产生。

## HIR 不变量

- 所有表达式操作数数量与节点种类一致。
- 变量、字段、参数和返回值拥有最终类型；`:=` 不留给后端推断。
- `is/as` 的目标是已解析 Godot/项目/Attached 类型。
- 动态方法与动态属性是不同 resolution。
- `for` 必须携带 `IterationPlan`，后端不得从 iterable 名称重新猜测。
- `Array[T]`、`Dictionary[K,V]` 保存完整递归签名与项目/第三方依赖。
- `@rpc` 已规范化为 `RpcConfiguration`，后端不再解释字符串参数。
- 常量必须有初始化器，赋值必须分别保存 target/value。
- 已知协程调用携带 coroutine ABI，消费结果时必须位于 await。
- breakpoint 保存语义阶段确定的精确可见绑定，遮蔽后的外层同名局部不能泄漏到调试器。
- 局部 identifier、声明和调试可见绑定使用同一 `FlowSymbolId`；缺失或错配身份不能退化为按名查找。
- 所有原始 await 在进入 C++ emitter 前转换为独立挂起/恢复语句。

违反不变量产生 `GDS5xxx` 编译诊断，不依赖断言或无效 C++。

## await A-normal form

HIR lowering 在挂起点前稳定求值：

- 普通赋值的接收者、索引和右值；
- 调用接收者及每个参数；
- Array/Dictionary 元素；
- 算术与比较操作数；
- if/while/for/match/assert 的控制表达式。

短路 `and/or` 和三元表达式建立分支前缀，未选分支不会执行。`while await` 的真实条件放在循环
头，每次恢复后重新计算。match 的模式绑定先发生，随后执行 guard 前缀；guard 为假继续后续
分支，正文恢复后进入 match 外层。assert 条件与消息分别降低，Release 可完整删除两者。

异步循环把跨挂起且会被写入的外层局部值、函数参数和 setter 参数按 `FlowSymbolId` 提升到共享
单元。lambda 创建时从这些单元物化 GDScript 捕获快照；每次 Callable 调用再建立独立可写调用
帧，因此标量修改不会污染下一次调用，而 Array/Dictionary/Object 仍保留共享身份。break 与
continue 选择明确的恢复出口。4996 项、每帧 200 项的批处理 oracle 用于锁定局部状态身份和循环
结束条件，避免只证明生成代码能编译。

## MIR

MIR 为方法、getter、setter 和 lambda 建立 CFG：

```text
Function
  entry: BlockId
  blocks:
    instructions[]
    terminator
    predecessors[]
```

指令记录 evaluate、declare、assign、assert、debug breakpoint、loop test、match test 和
suspend value；副作用标记区分读状态、写状态、可能失败、可能分配、挂起和观察调试器。
`observes_debugger` 使 breakpoint 即使不改变程序数据也不能被死代码删除或跨越重排。

终止指令包括 jump、branch、return、stop 和 suspend。branch 的 `BranchRole` 区分：

- 普通 truthy condition；
- iterator protocol；
- match pattern；
- match guard；
- assertion。

因此常量优化不会把迭代协议或模式测试误当普通布尔表达式。

## MIR verifier

优化前后均验证：

- 入口块存在，ID 连续且目标有效；
- 每个块只有一个合法终止指令；
- 前驱集合与所有边精确一致；
- branch 目标数量和 role 合法；
- suspend 只有一个恢复目标；
- return/stop 不残留目标；
- HIR source、SourceSpan、iteration/coroutine 载荷仍可追踪；
- 删除不可达块后完成重编号与前驱重建。

## 当前优化

已进入默认管线：

1. 整数、浮点、布尔和字符串安全常量折叠；
2. `pass` 删除；
3. return/break/continue 后同块不可达语句删除；
4. 字面量 false 循环和静态 if 未选分支删除；
5. 仅对 `BranchRole::condition` 的常量布尔边折叠；
6. 不可达块删除、密集重编号和前驱重建；
7. 稳定 Value/Operation ID、去地址版本化序列化和 source identity 验证；
8. 无效值/操作删除、所有活跃 operand 的密集重映射；
9. pass 前后 verifier、指令/块预算和失败时保持输入不变的事务回退。

`gdpp compile --no-optimize` 可关闭可选优化用于诊断。优化统计和阶段耗时通过 CompileResult 暴露。

后续性能优化，不属于语言或 runtime P0：

- 一般死存储、复制传播、CSE；
- 逃逸分析、装箱消除；
- 受预算内联、去虚拟化和循环优化；
- 随机 IR 与优化前后运行差分。

这些 pass 在拥有别名/所有权证明和官方 GDScript/AOT 等价差分前不会默认启用；缺少优化只影响
性能，不允许改变已支持输入的结果或退化成后端猜测。

## 协程表示

实例方法、静态函数和 lambda 协程在源语义上保留原返回类型，在脚本调用 ABI 边界返回一个
Variant：

- 同步完成：直接返回源值；
- 真正挂起：返回本次调用独有的引用计数 FunctionState；
- 恢复：状态对象的 `completed` Signal 携带一个返回值，`void` 映射为 `null`；
- 状态接口：`resume()`、`is_valid()`、一次性连接清理和零/一/多 Signal 参数折叠；
- 并发调用：状态对象、完成 Signal 和 lambda 调用帧不共享；
- 取消：等待对象失效时通过 ObjectID 防止悬空访问。

简单长 await 链使用 MIR 程序计数器状态机，避免递归嵌套 C++ lambda。复杂循环/match 与跨挂起
局部状态使用结构化共享帧。两条路径都由弱回指避免状态机自引用环。

静态函数由 FunctionState 自身提供生命周期宿主，不依赖实例；带返回值 lambda 和嵌套闭包复用
同一完成协议，同时在创建/调用两个边界维持捕获快照。await 默认参数在缺参分支内按声明顺序
执行，已经求值的接收者和实参跨挂起保存在同一调用帧。实例、静态、内部类及绑定/内联属性
访问器使用独立私有状态并保持跨脚本协程 ABI。异步引擎虚函数与官方 GDScript 一样立即把
FunctionState 交给 Godot 的 Variant→native 转换，而不是让引擎阻塞等待。把程序计数器与
结构化共享帧统一为单一物理表示是后续维护性重构，不是两个可观察语义。

## 强类型容器

`semantic/type` 是容器签名唯一解析入口。HIR 保存元素、键和值类型、对象类名和脚本约束。
后端映射到 godot-cpp TypedArray/TypedDictionary；对象元素使用轻量 ClassDB 标签，不通过引入
完整脚本头文件换取元数据。

从 Variant、普通容器、PackedArray 或参数化 `as` 进入强类型存储时，runtime 比较真实
Variant 类型、对象类和脚本约束。只有签名一致才进入 typed storage。编译期不变规则与 runtime
guard 共用同一类型描述。

普通/强类型 Array、Dictionary、十种 PackedArray、Object/Ref 约束和全部 Variant 值家族已经
进入官方源码/AOT fault 差分。合同固定错误类别、源码位置、求值顺序和当前函数中止；不同 Godot
patch 的自然语言诊断变化不作为 ABI。

## 后端边界

C++ emitter 只接收通过 verifier 的 MIR module。当前丰富表达式载荷仍由 MIR 绑定的 HIR 提供，
因此“已有 CFG”不等于“已有完整 SSA”。后端允许：

- 映射已解析类型和 ABI；
- 发射已选择的静态调用或版本化 runtime intrinsic；
- 实现 MIR 控制边和协程恢复；
- 注册类、属性、Signal、RPC 与 Attached 行为。

后端禁止：

- 重新进行名字/重载/类型推断；
- 根据源码字符串决定语言特性；
- 绕过 runtime 直接制造未经审计的 Variant/容器转换；
- 为未实现节点输出注释占位后继续成功；
- 在没有等价性证据时删除可能失败或有副作用的表达式。
