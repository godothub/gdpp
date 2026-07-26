# 当前状态与功能缺口

本文是 `docs/` 的状态入口。其他文档说明设计、使用和验证细节；如果状态描述冲突，以本文和
当前源码/发布门禁为准。

## 审计基线

- 审计版本：GDPP 1.8.0。
- 功能审计范围：1.8.0 发布分支当前提交；正式发布门禁由同一提交的 release workflow 执行。
- 最近正式发布门禁：1.7.10 / GitHub Actions `30170732292`，46 个作业全部成功。
- 本地编译器单元测试：543 / 543。
- Godot 目标：4.4、4.5、4.6、4.7；CI 使用 4.4.1、4.5.2、4.6.3、4.7.1。
- SDK schema：12；生成项目 runtime ABI：18。

状态标记只表达已经存在的证据：

| 标记 | 含义 |
|---|---|
| 已交付 | 发行包包含该能力，发布门禁会构建并验证 |
| 已实现，认证受限 | 主路径存在，但尚未覆盖所有设备、平台组合或长时行为 |
| 失败关闭 | 编译器能在生成/打包前明确拒绝，不能静默回退或生成猜测语义 |
| 未实现 | 合法 GDScript 或产品流程尚无等价实现 |
| 非目标 | 当前产品设计明确不提供 |

## 已交付主路径

| 领域 | 当前状态 |
|---|---|
| 普通 `.gd` 导出期 AOT | 已交付；平时编辑、导入和编辑器运行仍由 Godot 原生 GDScript 负责 |
| 前端 | Godot 4.7 stable 语法基线；官方合法 parser 语料 114 / 114 前端接受，非法语料 76 / 76 最终拒绝 |
| 语义与生成 | 类型化 AST/HIR、显式 CFG/MIR、C++17、确定求值顺序、强类型容器、项目符号图和增量失效 |
| 协程 | 实例/静态/lambda、FunctionState、返回值、跨脚本调用、访问器、短路/三元、循环、match、assert 和大批次恢复 |
| Callable/lambda | 创建时捕获快照、共享容器身份、嵌套/递归闭包、静态/实例 Callable、类型化 lambda 协程和并发恢复 |
| Signal 生命周期 | bind/unbind、一次性/延迟/引用计数连接、发射期连接变更、宿主销毁和多线程 Callable 调用矩阵 |
| Godot API | 4.4～4.7 独立能力表；高版本语法与目标引擎 API 版本分离 |
| 场景与资源 | PackedScene、Resource、Autoload、内嵌/共享/循环资源图的 Attached AOT 转换 |
| 第三方 GDExtension | 零源码修改的 ClassDB 契约采集、Attached 继承、外部 `super`、Signal、RPC 和序列化 |
| 动态运行路径 | Variant 运算、动态属性/方法、网络消息分派、网络图片、运行时 Shader/Material 赋值和对象池批处理 |
| 原生调试 | `breakpoint` 贯通 AST/HIR/MIR/C++17 与 ScriptLanguageExtension；报告源码行、词法局部变量和继承成员 |
| 桌面目标 | Windows x86_64、Linux x86_64、macOS Universal 2 |
| 其他目标 | Android arm64、iOS device arm64 + Universal Simulator、Web wasm32 threads/nothreads |
| 无源码交付 | 成功 AOT 包剥离 `.gd/.gdc`、compiler、SDK、静态库、生成 C++ 与对象文件 |
| 发行物 | `gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip`，每个包含 Godot 4.4～4.7 SDK |

## P0 语言与运行语义结论

1.8.0 已关闭当前声明兼容面的 M1～M4 P0 阻断项：

- 前端具有可恢复多错误解析、完整 AST/`SourceSpan` 序列化 golden、coverage-guided fuzz、
  资源预算以及 Godot 4.7.1 官方合法/非法语料漂移门禁。
- 38 个非 `MAX` Variant 家族、PackedArray、强类型 Array/Dictionary、Callable、空值、失效
  Object/Ref、错误键、越界和整数故障均进入统一 source/AOT fault 差分；故障携带源码位置并
  只中止当前脚本函数。
- 真正挂起的实例/静态/lambda/内部类/属性访问器、await 默认参数和赋值目标都使用同一
  FunctionState 契约；项目 ABI 变化会传递失效所有消费者。
- 真实 ENet 多 peer RPC 门禁覆盖 authority/any-peer、call-local/remote、transfer mode、
  channel、排序、拒绝和导出后运行，不再以单进程反射代替网络语义。
- 项目中的每个可交付 `.gd` 都登记为无源码 `Script` 身份。运行时拼接路径、相对路径、UID、
  `ResourceLoader` 同步/线程加载、缓存、`exists()` 和 `.new()` 都从编译清单解析；清单之外的
  动态路径确定失败，不会在成品中偷偷依赖源码。
- 标准及 custom/double 引擎 SDK 都绑定精确 `extension_api.json` SHA-256、precision、
  Godot 版本、godot-cpp 和 runtime ABI；不匹配在生成编译命令前失败。
- MIR 已有稳定 Value/Operation ID、去地址序列化、事务 verifier、优化预算和死值密集重映射；
  native meta、符号映射、唯一入口和生成二进制审计均有发布门禁。
- 项目脚本 `Object.free()` 通过 Godot 原生 Variant 调度保留普通 Object 销毁、RefCounted
  拒绝、锁定对象保护和当前函数失败边界，不直接 `memdelete` 绕过引擎生命周期。

挂起调用返回具备 `completed`、`resume()`、`is_valid()` 和一次性连接清理的引用计数状态对象；
引擎虚函数按 Godot 自身规则立即把该对象转换为声明的原生返回类型，不虚构引擎会等待脚本协程。
`_init` 挂起仍按 GDScript 构造边界稳定拒绝，这是语言规则而不是功能缺失。

`@static_unload` 的语法、HIR、缓存和 Script metadata 已完整保留。当前目标 Godot 4.4～4.7 的
可观察行为是脚本静态状态保持到语言/扩展关闭；Godot 4.7 官方实现和文档也明确记录脚本目前不会
因引用归零被释放。GDPP 与该目标行为一致，并在扩展终止时确定清理；未来 Godot 修复上游卸载
行为时必须新增目标版本差分，不能在现有目标上提前实现一个与引擎不同的重置时机。

## 仍存在的 P1 功能与认证缺口

以下内容不改变当前声明语言面的运行结果，但会影响构建体验、性能上限、供应链保证或支持矩阵。

### 项目构建与编辑器体验

1. 同一项目被多个 Godot 编辑器或 CLI 同时构建时，没有跨进程排他锁；单进程内事务和缓存是
   安全的，但多进程并发写同一 `addons/gdpp/build` 尚未认证。
2. 后台构建没有用户取消协议；关闭编辑器、切换项目或禁用插件时的中止/恢复仍需故障注入。
3. 诊断尚未完整接入 Godot 问题面板，缺少点击跳转、工具链 doctor、缓存命中/失效报告和脱敏
   诊断包。
4. `breakpoint` 语句已经进入 Godot 调试器，但编辑器 gutter 行断点、单步进入/越过/跳出、
   所有父帧局部变量和原生崩溃符号反查尚未形成完整源码级调试体验。
5. 描述符与场景转换具备事务恢复，仍缺断电、低磁盘、只读目录、跨卷 rename、超长路径和
   连续崩溃恢复矩阵。
6. 前端摘要尚未跨进程持久化；生成物和对象有精确缓存，但新进程仍需重新词法、解析和语义分析。
7. 编译命令按翻译单元严格串行是当前商业设计，不把“并行编译”列为缺失。后续优化必须先有
   内存预算、确定诊断顺序和可取消调度。

### P1：第三方扩展边界

1. 只支持供应商公开注册到 ClassDB/GDExtension ABI 的契约；私有 C++ API、未注册回调和需要
   供应商头文件的行为不猜测实现。
2. 编辑器专用类已经标记并阻止进入 runtime 场景，但编辑器/发行能力域仍缺更多供应商矩阵。
3. 构建身份包含所消费 ClassDB 契约哈希；供应商目标动态库本身的内容摘要还没有成为统一的
   导出前锁定项。库在采集契约后被替换的供应链场景仍需阻断。
4. provider 与 GDPP 项目库的两种加载顺序已覆盖；更多真实供应商、所有平台和升级/降级组合
   尚未认证。

## 仍存在的商业认证缺口

这些不是当前主路径“没有代码”，但在完成前不能扩大正式支持声明：

- Android：缺真机安装/启动、触控、音频、前后台、低内存、AAB、ABI split、商店签名和
  Android x86_64。
- iOS：缺签名真机、前后台/音频会话、Archive、TestFlight 与 App Store 上传。
- Web：Chromium threads/nothreads 已成为发布门禁；Safari、Firefox、移动浏览器、真实 CDN、
  SIMD/PWA 和长时内存增长尚未认证。
- 桌面架构：Windows arm64、Linux arm64 未交付；macOS 已交付 Universal 2。
- 稳定性：缺多小时网络/场景切换/资源流送、重复导出、磁盘增长、内存峰值和恢复压力门禁。
- 性能：已有 GDScript/AOT 固定 oracle 和 Windows 网络事件场景对照，但还缺手写 godot-cpp
  第三基线、可靠跨平台 RSS/分配统计及长时 p50/p95/p99。
- 供应链：发行包有 SHA-256、固定 Action 提交和内容审计；代码签名、公证、SBOM、provenance、
  符号归档与撤回演练尚未完成。

## 明确非目标

- 不在普通编辑、导入或“运行项目”期间替代 Godot 的 GDScript；AOT 只在启用插件的导出预设中
  工作。
- 不提供运行期热重载。生成项目 GDExtension 标记为不可热重载，重新导出/重启是安全边界。
- 不要求或修改客户源码、场景、资源、Autoload、第三方 GDExtension 源码。
- 不在客户导出时调用 CMake、Ninja、Python 或 SCons。
- 不承诺二进制不可逆、业务字符串加密或客户端逻辑绝对保密。
- `.gdpp` 增强语法当前不在交付范围；普通 `.gd` 始终以兼容 GDScript 为目标。

## 发布判定

当前可以对外承诺的是“已声明目标和已覆盖语法的失败关闭 AOT 交付”，不能承诺“任意 Godot
项目、全部 GDScript、全部设备无条件兼容”。任何新增公开能力必须同时具备：

1. 前端、语义、HIR/MIR、C++17 与 runtime 契约；
2. 正例、拒绝例和至少一个真实 Godot 运行差分；
3. 对应平台/版本的导出与无源码审计；
4. 更新本页、`COMPATIBILITY.md`、`ROADMAP.md` 和相关专项文档。
