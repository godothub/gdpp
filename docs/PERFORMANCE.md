# 性能与体积

GDPP 的性能承诺不是“AOT 一定更快”，而是：

1. 行为 oracle 必须与 GDScript 一致；
2. 固定工作负载、启动和帧预算中，AOT 不得比 GDScript 慢 10% 以上；
3. 可以更快，但不能用微基准外推整个游戏；
4. 首次 C++ 编译成本、动态库体积和缓存命中必须如实计入。

预算源文件是 `test/performance/runtime_matrix.json`。

## 固定运行矩阵

当前矩阵覆盖 13 类路径：

| 类别 | 主要行为 |
|---|---|
| `numeric_typed` | 强类型整数/浮点运算 |
| `branch_typed` | 强类型条件和循环 |
| `array_typed` | Array 读写与共享语义 |
| `packed_array` | PackedArray 原生元素路径 |
| `dictionary` | Dictionary 键值与写回 |
| `builtin_vector` | Vector/Color 等值类型 |
| `object_property` | Godot Object 属性 |
| `string_ops` | String/StringName |
| `method_calls` | 项目方法和调用 ABI |
| `callable` | Callable 调用 |
| `variant_ops` | 动态 Variant 运算 |
| `signal_emit` | Signal 发射 |
| `allocation` | 容器构造和分配型路径 |

每个 case、启动和固定帧 workload 都使用最大 AOT 回归 10%。行为浮点比较使用绝对/相对
`1e-9` 容差，其余值、容器和结构递归精确比较。

日志包含下列任一模式会直接失败：

```text
SCRIPT ERROR:
Parse Error:
ERROR:
Unable to open
AddressSanitizer
UndefinedBehaviorSanitizer
```

## 测量方法

`tools/run_runtime_matrix.py` 要求 GDScript 与 AOT：

- 使用同一 Godot 可执行文件和同一引擎版本；
- 使用同一项目、输入、迭代次数和导出 profile；
- 以 AB/BA 顺序交替执行，降低温度和磁盘缓存偏差；
- 分别测启动、微基准和固定帧；
- 记录 median、p95、p99、标准差和变异系数；
- 在 Linux 记录可获得的进程 peak RSS；
- 对 PCK 和完整发行目录记录文件、字节数和 SHA-256；
- 在比较性能前先验证全部行为 oracle。

CI 在 Godot 4.4.1、4.5.2、4.6.3、4.7.1 的兼容作业中执行该矩阵。报告作为构建证据上传，不进入
插件发行包。

## 当前端到端结果

发布候选在 macOS、官方 Godot 4.7.1、相同 Universal 2 Release 模板、5 轮 AB/BA、每轮
5 个样本和每个 case 10,000 次迭代下通过完整行为与性能门禁：

```text
Dictionary mean: GDS 199.49 ns / AOT 138.11 ns（AOT -30.77%）
String mean:     GDS 150.64 ns / AOT 126.35 ns（AOT -16.13%）
Variant mean:    GDS 23.07 ns / AOT 16.74 ns（AOT -27.43%）
13/13 benchmark families、启动、固定帧：全部 <= 10%
行为 oracle：PASS
```

该结果来自 fault frame 最终效应分析、Dictionary 读写合同、Variant 边界借用及单次 RHS 快照
移动提交版本，用于证明完整错误传播没有依靠热点轮询换取正确性，也没有通过放宽阈值掩盖回归。
named/keyed Variant ABI 对包括 `Nil` 在内的所有结果都用一次查找返回有效位；直接/复合、强类型
和只读写入保留官方故障边界。只有经完整函数符号身份与逃逸审计证明安全的局部非强类型字面量
已存在键才借用读取或原地更新；重赋值、别名、调用、下标、未知键、强类型、方法访问或闭包捕获
都会回到受检 getter/setter。数值会随机器和 Godot patch 变化，正式发布仍要求四个目标版本
各自在 Linux runner 独立通过同一门禁。

最近一次 Windows 联机事件补充审计在同一机器、相同输入和三轮 warm 运行下得到：

```text
AOT median: 5832 ms
GDS median: 6989 ms
AOT / GDS: 0.8345
回归预算: <= 1.10
```

该 fixture 同时处理 HTTP、WebSocket、二进制消息、网络图片、礼物/点赞/分数事件、对象池和
Shader/UI 更新。结果表明这一端到端场景未发生性能回归，AOT 中位数约快 16.55%；这不是所有
游戏的普遍加速承诺。

4996 项对象池队列、每帧 200 项的协程循环 oracle 还用于验证大批次恢复不会：

- 重复编译或执行同一批；
- 在 await 后读到过期局部值；
- 形成状态机自引用；
- 多走一轮或漏掉末批；
- 因 UI/网络对象已释放而崩溃。

## 已落地热路径

- 强类型数值和位运算使用原生 C++17，并通过无符号位模式避免宿主未定义行为。
- `range()`、整数/浮点/向量范围循环在同步路径直接生成标量循环。
- PackedArray 同步循环按原生 size/index 和静态元素类型读取。
- Array/Dictionary 保持 GDScript 共享存储，嵌套写回按接收者→索引→右值顺序只求值一次。
- 静态 Godot 调用使用 API 能力表选择 native ABI；动态调用集中到 runtime。
- 动态调用和本地 Signal 的 `StringName` 按调用点缓存。
- 本地/`self` Signal 直接进入 `emit_signal`，外部接收者保留通用错误语义。
- 同步脚本 fault frame 在调用点内联并使用线程局部活动状态；协程通过无捕获检查器读取恢复
  线程已安装的持久状态，既不捕获调用者栈，也不为每个表达式支付原子读写。
- 后端对类型化表达式、转换和赋值执行保守故障效应判定；只在动态运算、严格存储、越界、整数
  除法/取模、对象/Callable/Signal 重入等可能设置 fault state 的边界轮询。字面量、局部变量、
  精确存储、安全整数运算、String 值方法和已证明安全的三元分支不生成伪故障分支。
- 本地 Callable 把参数数量写入 C++ 类型，并保留未逃逸调用的原生参数 tuple；完全匹配时省略
  参数 Variant 往返，逃逸/赋值和动态调用仍执行完整有效性、数量和严格转换检查。
- 显式 Variant 转换先处理精确同类型，再进入 Godot 通用转换矩阵；不改变非精确转换的接受集合。
- PackedArray 参数通过专用 native argument 适配，不做多余 Variant round-trip。
- Release/Debug 项目库都启用编译优化、函数/数据 section 和链接器 dead stripping。
- Release 不生成 breakpoint/调试帧代码；Debug 只有在 `EngineDebugger` 已连接时才把帧压入线程
  局部调试栈，没有调试器时跳过变量快照。
- 简单长 await 链按 MIR 状态机生成，避免递归 lambda 导致 MSVC 模板内存爆炸。
- 异步循环共享帧只提升跨挂起且会写入的局部状态。

这些优化都必须保留 GDScript 左到右求值、副作用次数、短路和错误触发顺序。

## 编译时间

### 首次或失效构建

AOT 首次导出必须为生成 C++ 执行系统编译器和链接器，因此必然比普通 GDScript 打包多一段原生
构建时间。客户导出不构建 godot-cpp；它只链接包内预构建 `template_release` 静态库。

每次只构建当前目标/profile：

```text
N 个生成/运行时翻译单元
  -> N 次严格串行编译
  -> 1 次链接
```

严格串行是避免客户机器出现大量 C++ 子进程和不可控内存峰值的商业设计。Windows MSVC 环境只
初始化一次，所有子进程隐藏控制台。

### 增量构建

两层缓存：

1. 项目 manifest 缓存脚本实现哈希、公开 ABI、依赖边和生成文件；
2. NativeBuilder 缓存对象、depfile、SDK/runtime/bridge/工具链/profile 身份。

方法体变化只重生成/编译自身；公开 ABI 变化才传播到消费者；静态库变化只重链；无变化导出复用
全部对象。

尚未完成持久化 AST/符号摘要，因此新 Godot 进程仍会重新执行前端。跨进程并发构建未支持。

## 代码尺寸与插件包

原生 AOT 删除 `.gd/.gdc`，但增加项目动态库，不能承诺完整游戏一定更小。最终变化取决于：

- 脚本文本/字节码大小；
- 生成行为和 runtime 的 dead stripping；
- PackedScene/Resource 转换后的序列化；
- 静态调用与动态桥接数量；
- 平台动态库格式和压缩方式。

1.8.0 插件发布资产大小门禁当前使用最近的 1.7.10 正式资产作为比较基线：

| 资产 | 字节 |
|---|---:|
| `gdpp-linux.zip` | 168,875,363 |
| `gdpp-mac.zip` | 286,198,939 |
| `gdpp-win.zip` | 208,505,385 |

mac 包较大是因为同时包含 Universal 2 桌面 SDK、iOS device/Universal Simulator、Android 和
Web；三个包都含四套 Godot SDK。每个平台/模式只保留一份 `template_release`，已删除
template_debug/editor 静态绑定。

最终游戏不会携带这些 SDK，因此插件 ZIP 大小不能作为游戏包大小。

## PCK 与源码

发布门禁要求：

- PCK 内 `.gd/.gdc` 为 0；
- compiler/fallback、SDK、godot-cpp、静态库、生成 C++、对象为 0；
- 完整目录只有一个匹配项目库；
- 所有可枚举场景/资源能够加载；
- 导出摘要、库入口和 descriptor 一致。

Windows 补充审计：

```text
PCK_AUDIT_FILES=397
PCK_AUDIT_RESOURCES_LOADED=42
PCK_AUDIT_PROJECT_LIBRARIES=1
PCK_AUDIT_VIOLATIONS=0
PCK_AUDIT_UNEXPECTED_ERRORS=0
```

“没有脚本文本”是交付事实，不等于业务逻辑加密或不可逆。

## 尚未完成的性能门禁

- 手写 godot-cpp 第三基线；
- macOS/Windows 可靠 peak RSS 与跨平台分配次数；
- 编译阶段前端/语义/HIR/MIR/codegen/C++/link/资源转换的统一分段报告；
- 大规模敌群、Boss、存档、资源流送和场景实例化固定 workload；
- 多小时网络、资源和场景切换 soak；
- 全目标帧时间 p50/p95/p99 和磁盘增长；
- 逐场景序列化尺寸预算；
- Safari/Firefox/移动端的真实运行性能。

在这些完成前，商业文案只能引用明确 fixture、机器、版本和指标，不能使用“所有项目超高性能”
或“AOT 自动缩包”。
