# GDPP 商业路线图

路线图只记录尚未关闭的产品目标和完成定义，不再充当历史 changelog。已发布内容见根目录
`CHANGELOG.md` / `CHANGELOG-ZH.md`，当前事实见[状态审计](STATUS.md)，语法细节见
[兼容性矩阵](COMPATIBILITY.md)。

## 路线图原则

1. M1～M4 的语言、语义、IR、后端和 runtime 正确性优先于新增平台、界面和增强语法。
2. 未支持行为必须在生成 C++ 前失败关闭，不能靠运行时崩溃暴露。
3. 普通 `.gd` 保持 GDScript 兼容；增强语法只允许通过未来独立 `.gdpp` 模式进入。
4. 客户平时继续使用 Godot GDScript；GDPP 只在启用 AOT 的导出期间编译、转换和构建。
5. 客户源码、场景、资源、Autoload 和第三方 GDExtension 不需要修改。
6. 编译翻译单元严格串行是当前受控内存策略，不以无预算并行作为性能目标。
7. 每个“完成”状态必须同时拥有实现、拒绝路径、Godot 运行证据和交付审计。

## 当前里程碑状态

| 里程碑 | 目标 | 当前判断 | 退出前剩余 |
|---|---|---|---|
| M0 | 工程基础 | 主路径完成 | 覆盖率、公共头独立编译、最低/最新编译器矩阵 |
| M1 | 词法与语法 | P0 完成 | 持续跟踪 Godot stable 语法漂移 |
| M2 | 类型与语义 | P0 完成 | 扩充认证组合，不保留已知语义占位 |
| M3 | HIR/MIR 与优化 | P0 语义完成 | CSE、内联、逃逸和循环优化属于性能路线 |
| M4 | C++17 后端/runtime | P0 完成 | 扩充平台故障注入与源码调试体验 |
| M5 | 项目构建/编辑器 | 生产主路径可用 | 跨进程锁、取消、doctor、问题面板、断电/低磁盘恢复 |
| M6 | 场景/资源/交付 | 生产主路径可用 | 动态脚本路径保留策略、更多动态图与故障注入 |
| M7 | 平台 | 声明目标可构建 | 移动真机、更多浏览器、Windows/Linux arm64、商店流程 |
| M8 | QA/性能 | 发布门禁已建立 | 持续 fuzz、长时 soak、分配/RSS、手写 godot-cpp 基线 |
| M9 | CI/CD/供应链 | 自动发布已建立 | 签名、公证、SBOM、provenance、符号和撤回演练 |
| M10 | 开发者体验 | 基础文档/CLI | doctor、结构化报告、IDE/诊断跳转 |
| M11 | `.gdpp` 增强语言 | 暂缓 | 独立 ScriptLanguage、feature set、迁移与稳定性分级 |
| M12 | 商业运营 | 未完成 | 支持周期、弃用、安全响应、隐私和客户通知 |
| M13 | 可观测性 | 部分完成 | trace、脱敏诊断包、崩溃索引和恢复报告 |
| M14 | 企业集成 | 未完成 | 离线制品、企业 CI、私有 SDK、混合语言和仓库集成 |

“生产主路径可用”不等于 1.0 Stable；它表示当前 1.8.x 发布门禁覆盖的目标可以失败关闭交付。

## 已关闭：M1～M4 P0 功能闭环

1.8.0 以“实现、官方源/AOT 差分、生成 C++17、真实 Godot 运行和发行审计”同时成立作为关闭
条件，而不是以单个样例成功作为完成证据。

### M1：前端

- 可恢复 parser 会在同一文件中给出独立有界诊断；缺失节点、未闭合分组和尾随空白也只能生成
  单调且位于源码内的范围，失败事务不会产生半合法 AST。
- 每类 AST 节点和 `SourceSpan` 进入确定性完整序列化 golden。
- coverage-guided fuzz、失败样本归档、非法 UTF-8/NUL、递归/链长/诊断预算及最小语料进入 CI。
- Godot 4.7.1 的 114 个合法和 76 个非法 parser fixture、Unicode 与 warning/annotation
  注册表具有固定快照和 stable drift 报告。
- `Compiler` 与 `ProjectCompiler` 拥有固定 16 MiB 工作线程栈，最大调用/成员分析帧已经拆分；
  可重入调用不重复建线程，宿主嵌入线程的栈大小不能再改变合法输入的编译结果。

### M2：类型与运行语义

- Variant 全家族、强类型/普通容器、PackedArray、Callable、除零、越界、错误键、空值和失效
  Object/Ref 使用统一 fault frame，并比较官方源码与 AOT 的失败位置和函数中止边界。
- 字典点号/下标读取共享受检有效性合同，以 named/keyed Variant ABI 的一次查找区分缺失/非法
  键与合法 `null`；强类型点号键/值、直接/复合写入和只读故障均匹配官方函数中止边界。
- 真正挂起的实例、静态、lambda、内部类、属性访问器、await 默认参数及赋值目标共享
  FunctionState、捕获和跨脚本 ABI。
- Signal/Callable 覆盖 bind/unbind、one-shot/deferred/reference-counted、发射期变更、宿主销毁、
  并发调用及嵌套/递归共享容器。
- 同步调用的 fault frame 是线程局部栈状态；协程恢复由 FunctionState 互斥串行拥有同一持久
  状态。本地 lambda 的参数数量、变参身份和原生参数快照进入生成 C++ 类型，逃逸或赋值后自动
  降级到完整 Callable/Variant ABI，保持失效对象与错误参数诊断。
- 真实 ENet 多 peer 覆盖 RPC authority/any-peer、local/remote、传输模式、channel、排序、
  拒绝、同一脚本节点跨 peer 重连，以及节点/缓存、peer、SceneTree 根的确定退出。
- 全项目编译清单提供动态拼接 `.gd` 路径、相对路径、UID、同步/线程 ResourceLoader、缓存、
  `exists()` 和 Script `.new()` 的无源码身份；清单外路径确定失败。
- custom/double SDK 由精确 API SHA-256、precision、版本、godot-cpp 和 ABI 生产并审计。

`@static_unload` 在 4.4～4.7 保留完整注解身份并匹配当前 Godot“脚本保持到语言关闭”的可观察
行为；未来引擎修复引用归零卸载时必须按目标版本增加差分。

### M3：IR 语义基础

- HIR 的局部、引用、捕获、await 提升与调试绑定共享稳定 `FlowSymbolId`。
- MIR 具有稳定 Value/Operation ID、版本化去地址序列化、精确 CFG/前驱和 source identity。
- 优化前后 verifier、事务回退、预算、死值删除与密集重映射已经关闭运行正确性风险。

一般 CSE、受预算内联、去虚拟化、逃逸/装箱消除、循环不变量和强度削弱会继续作为 M8 性能
任务；它们不能以“P0 未完成”为理由混淆语言正确性，也不能在没有等价差分时启用。

### M4：C++17 后端与 runtime

- 所有可达 HIR/MIR 节点要么生成有效 C++17，要么在前端/验证阶段失败，不输出注释占位。
- runtime failure 携带 `.gd` 路径、行列、实际/期望类型并维持当前函数中止。
- 同步 fault 检查在生成调用点内联；类型化表达式/转换/赋值的保守故障效应只保留真正可能设置
  fault state 的轮询，现有 Variant 边界借用、单次 RHS 快照移动、精确同类型 Variant 转换和
  未逃逸本地 Callable 进一步消除可证明的重复装箱/复制/全局对象查询；官方 4.7.1 行为 oracle
  与 10% 性能门禁同时通过。
- 静态 `Dictionary.key` 与下标分别使用 named/keyed Variant ABI 的有效位，所有结果都保持一次
  键查找；局部非强类型字面量槽位只有经符号身份、已知键和无逃逸证明才原地复合更新，source/AOT
  fault 序列锁定缺键、强类型、只读及合法空值的函数中止边界。
- `.gd`→C++/native 符号图、全部 native meta/enum/bitfield/real_t/precision ABI 和 API 范围
  进入自动审计。
- 项目脚本 `Object.free()` 通过 Godot Variant 调度，保留 RefCounted 和锁定对象保护。
- 生成库只导出 `gdpp_library_init`；桌面、移动、Wasm 的入口、依赖、路径、切片和无源码内容
  均由发布门禁校验。

后续 codegen 文件拆分、完整单步调试、更多供应商二进制故障注入和符号服务属于工程性 P1/P2，
不代表当前声明语言面存在已知占位语义。

## P1：构建、交付与平台认证

### M5：构建与编辑器

- `M5-001` 增加项目级跨进程锁，协调多个编辑器、CLI 和导出任务。
- `M5-002` 建立取消/超时协议，子进程、临时产物、描述符和缓存都能确定恢复。
- `M5-003` 缓存写入使用临时文件、持久化和原子提交；补齐断电、低磁盘、只读、跨卷和崩溃测试。
- `M5-004` 持久化 AST/符号摘要；新进程在源码和能力表未变时不重复完整前端。
- `M5-005` 增加 toolchain doctor、问题面板跳转、缓存原因报告和脱敏诊断包。
- `M5-006` 完成空格、非 ASCII、Unicode 规范化、Windows 长路径、符号链接和大小写矩阵。

导出翻译单元继续严格串行。只有在提供内存预算、稳定诊断顺序和取消语义后，才评估有限并发。

### M6：资源与无源码交付

- `M6-001` 对动态脚本路径、动态对象图和运行期资源构造建立可配置保留/失败策略。
- `M6-002` 扩展场景/Resource 图矩阵：继承场景、占位脚本、工具资源、跨包 UID 和运行期加载。
- `M6-003` 为编译、链接、加载、替换、打包、审计和恢复每个阶段加入故障注入。
- `M6-004` 统一 ZIP、PCK、APK、XCFramework、Wasm 和桌面目录的结构化审计报告。
- `M6-005` 对业务字符串、资源路径和反射元数据提供可见性报告，不承诺自动加密。

### M7：平台

- `M7-001` Android arm64 真机：安装、触控/手柄、音频、前后台、低内存、网络与 30 分钟稳定性。
- `M7-002` Android AAB、ABI split、商店签名、minSdk 矩阵和 x86_64。
- `M7-003` iOS 签名真机、前后台/音频会话、Archive、TestFlight 与 App Store。
- `M7-004` Web Safari、Firefox、移动浏览器、真实 CDN、SIMD/PWA 和长时内存增长。
- `M7-005` Windows arm64 与 Linux arm64 的 SDK、发行 runner 和实机门禁。
- `M7-006` 每个 Godot 4.4～4.7 patch 基线定期刷新，不能只证明一个历史 patch。

## P1：第三方 GDExtension

- `GDE-001` 将 provider 目标描述符和实际动态库 SHA-256 纳入 bridge/export 锁，阻断采集后替换。
- `GDE-002` 扩充编辑器专用与 runtime 能力域测试，确保 editor-only 类不会进入导出图。
- `GDE-003` 覆盖更多供应商：Node、Resource、RefCounted、单例、enum、回调、RPC 和序列化组合。
- `GDE-004` 在 Windows、Linux、macOS、Android、iOS、Web 验证 provider 与项目库加载/升级矩阵。
- `GDE-005` 定义供应商版本变化、契约兼容和缓存迁移报告；不依赖用户清理目录解决冲突。

私有 C++ API 和供应商未注册 ABI 不属于自动兼容范围；GDPP 必须明确报告，而不是生成跨库 C++
继承或要求客户修改供应商源码。

## P2：质量、性能和开发者体验

### M8：持续验证

- 持续 lexer/parser/IR fuzz，ASan、UBSan、TSan 和生成 C++ 严格 warning。
- 场景切换、网络事件、资源流送、协程、对象池和重复导出多小时 soak。
- GDScript/GDPP AOT/手写 godot-cpp 三方行为与性能 oracle。
- 冷/热构建、p50/p95/p99、分配次数、峰值 RSS、包体和代码尺寸预算。
- 为每个完成能力生成机器可读 feature manifest，文档状态由证据生成或校验。

### M9：发布与供应链

- 三桌面包签名；macOS notarization；可验证 SBOM 和 provenance。
- 独立调试符号归档、崩溃索引、保留周期和客户符号流程。
- 可重复归档时间戳/排序/权限，发布撤回、热修复和灾难恢复演练。
- SemVer、Godot 支持周期、SDK/runtime ABI 兼容与弃用策略。

### M10/M13：体验与可观测性

- `gdpp doctor` 和图形化环境检查。
- JSON/SARIF 诊断、Godot 问题面板跳转和稳定错误码说明。
- 在语句断点基础上完成 gutter 行断点、单步控制、全部父帧变量和 native 崩溃符号反查。
- 分阶段 trace、缓存命中/失效、命令耗时和脱敏失败包。
- 在不上传 `.gd`、生成 C++、用户路径和凭据的前提下导出诊断证据。

## 暂缓：M11 增强语言

`.gdpp` 不进入当前 1.8.x 功能闭环。启动该里程碑前必须先完成：

1. 独立 `ScriptLanguageExtension`、后缀和资源加载策略；
2. `language_mode`、feature set 和 runtime ABI 版本；
3. experimental/preview/stable/deprecated 成熟度；
4. 与普通 `.gd` 的互操作、迁移、回滚和错误隔离；
5. 目标 Godot 不抢先解析 `.gdpp`，导出和编辑器体验均有专门门禁。

## 版本与完成规则

- 修复现有承诺或文档：patch。
- 新增向后兼容能力或平台：minor。
- 改变客户配置、缓存/SDK/runtime ABI 或公开语义：必须记录迁移；不兼容时按 SemVer 处理。
- 不因某个样例成功关闭任务；证据必须进入可重复测试或发布门禁。
- 路线图项目关闭时，同一变更必须更新状态、兼容性、专项文档和 changelog。
- 正式发布只在 compiler、平台、包组装和最终 ZIP 安装门禁全部成功后创建。
