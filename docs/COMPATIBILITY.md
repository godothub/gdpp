# 最新版 GDScript 兼容性与商业验收规范

本文记录 GDPP 当前对最新版稳定 GDScript 的真实实现边界，并定义后续商业版本的验收口径。
语言前端只有一条持续前移的最新版基线，不向用户暴露 4.4/4.5/4.6/4.7 等语法方言选择；这些
版本只用于目标 Godot API、SDK 与 ABI。本文不把“能够分词”“元数据中存在”或“偶然生成过
C++”计为完整兼容，所有结论都应能由仓库中的测试、固定版本语料和原生构建复现。

审计基线：

- [x] 最低运行目标为 Godot 4.4；前端跟进最新版稳定 GDScript，当前同步和审计来源为 Godot 4.7。
- [x] Godot 4.4、4.5、4.6 与 4.7 使用独立 `extension_api.json` 能力表，编译时显式选择目标版本。
- [x] 审计日期更新为 2026-07-21。
- [x] 官方项目语料固定为 `godotengine/godot-demo-projects` 提交
  `540ef657b792237cf981fa8783f1896715cd699c`，不会跟随上游分支静默变化。
- [x] 官方 parser 语料固定为 Godot 4.7 stable 提交
  `5b4e0cb0fd279832bbdd69fed5354d4e5ad26f88`；合法与非法集合及文件数量都由 CI 失败关闭。
- [x] 旧 `godotengine/tps-demo` 属于 Godot 3 项目，不作为最新版兼容率样本；当前选择同一
  官方组织下的 3D platformer、3D voxel、rhythm game 和 role playing game。

## 状态口径

| 标记 | 含义 |
|---|---|
| ✅ | 声明范围已经进入词法、AST、语义、类型化 IR 和 C++ 生成主链，并有自动化回归测试 |
| 🟡 | 主路径可用，但仍缺语法分支、严格语义、运行差分、平台验证或完整测试矩阵 |
| ⛔ | 尚未实现，编译器会拒绝，或当前不能安全生成等价代码 |

表格中的 ✅ 不是“整个 GDScript 已达到商业稳定版”的宣传标记。运行时行为要达到商业承诺，
还必须通过 Godot 内执行差分、导出包审计和全平台认证。

## 当前可复现结论

| 指标 | 当前结果 | 判定 |
|---|---:|---|
| C++ 编译器单元测试 | 419 / 419 | ✅ 本轮实机 |
| 前端资源与恶意输入边界 | 8 类可配置上限、诊断抑制、512 组固定种子随机字节、项目级重复失败事务全部通过 | ✅ 本轮实机 |
| 本机开发 CTest 项目门禁 | 16 / 16（单元、CLI、addon、SDK、架构、发行打包、Release 预处理、官方项目与 parser 语料） | ✅ 本轮实机 |
| Godot 4.7 官方 parser 合法语料 | 114 / 114 未被 GDPP lexer/parser 拒绝，110 / 114 可在无项目上下文的逐文件模式完成代码生成；其余 4 项只缺同目录 `.notest.gd`/全局类上下文，不是语法拒绝 | ✅ 固定提交、逐文件报告 |
| Godot 4.7 官方 parser 非法语料 | 76 / 76 被编译器拒绝，其中 60 个在 lexer/parser 阶段拒绝、16 个在后续语义阶段拒绝；接受、超时、崩溃与异常退出均阻断 CI | ✅ 固定提交、失败关闭 |
| Godot 4.7 最新字面量差分 | 官方 GDS oracle、生成 C++ 编译、原生 GDExtension 加载与调用均通过；覆盖整数基数/边界、Godot 18 位浮点算法、INF/NAN/下溢、raw/三引号、控制/Unicode 转义与 U+FFFD | ✅ 本轮实机 |
| Godot 4.7 Unicode/布局差分 | 使用官方 Unicode 17.0 XID 表；15 组多语言及组合字符 oracle 通过，emoji 起始、非法 UTF-8/NUL、混合缩进和游离 CR 失败关闭；生成 C++ 标识符纯 ASCII 且无碰撞 | ✅ 本轮实机 |
| 最低系统构建契约 | Windows 10、macOS 11.0（Universal 2）、Ubuntu 22.04/glibc 2.35、Android 9/API 28、iOS 16.0；Web 无固定版本下限；编译参数、SDK schema 11 / runtime ABI 13 清单与 CI 使用同一基线，C++17、异常关闭、MSVC 19.x 与静态 CRT、Android `c++_shared` 和嵌套 CMake 工具链均失败关闭校验 | ✅ 规则与失败关闭测试 |
| Godot 4.5.2 插件集成门禁 | macOS、Ubuntu 24.04、Windows 11 各 16 / 16；测试描述符按构建目录哈希与目标架构隔离，不读取其他构建树或发行目录陈旧库 | ✅ 本轮实机 |
| 独立第三方 GDExtension 继承 | 供应商库与 GDPP 项目库完全独立；Godot 4.6.2 Universal 2 真实导出/运行通过，PCK 共 14 个文件、2 个场景、1 个资源、违规 0；Godot 4.7.1 独立加载与运行通过 | ✅ 本轮实机 |
| RPC 原生运行门禁 | `@rpc` 的默认值、权限、本地调用、可靠性和通道贯通语义、HIR、C++17、Godot 4.4 ABI 编译及 Godot 4.5.2 运行时配置查询；`call_local` 原生端点经 `Node.rpc()` 实际执行并校验参数与字段副作用 | ✅ 本轮实机 |
| 常量控制流优化门禁 | HIR 删除未选 `if` 分支和字面量假循环，MIR 按显式分支角色折叠边、删除不可达块并重建前驱；优化开关、match 负例、verifier、生成 C++ 内容和 Godot GDScript/AOT 结果差分通过 | ✅ 本轮实机 |
| 64 位整数语义门禁 | 前端常量、HIR 折叠、静态 C++、复合赋值和动态 Variant 共用纯 C++17 位模式契约；溢出、负值/超宽移位、`INT64_MIN / -1` 与边界 range 经 UBSan、严格 C++ 和 Godot 4.5.2/4.7.1 差分通过 | ✅ 本轮实机 |
| 静态迭代语义门禁 | `IterationPlan` 贯穿语义、HIR、verifier 和后端；覆盖 int/float/range、Vector2/2i/3/3i、String、Array、Dictionary、十种 PackedArray 和对象 `_iter_*` 协议。官方 Godot 4.7 接受/拒绝语料及 GDScript/AOT 运行差分一致 | ✅ 本轮实机 |
| macOS Universal 2 | AppleClang：arm64+x86_64 compiler、SDK、项目库、GDS→AOT→GDS Release 导出与运行通过；每个 Mach-O 切片单独检查唯一入口，PCK 违规为 0 | ✅ 本轮实机 |
| Ubuntu 24.04 x86_64 | GCC 13：Release 编译、插件加载、GDS→AOT→GDS 导出/运行、PCK/ELF 审计和固定性能预算通过 | ✅ 本轮实机 |
| Windows 11 x86_64 | MSVC 19.50：Release 编译、插件加载、GDS→AOT→GDS 导出/运行、PCK/PE 审计和固定性能预算通过；普通 Godot 进程可自动初始化 VS 工具链 | ✅ 本轮实机 |
| Android arm64-v8a | macOS + NDK 29 生成 26,262,814 B Release APK；脚本/compiler/SDK 泄漏 0，项目库 1，唯一入口 1，普通符号表已剥离 | ✅ 构建/导出；无连接设备，未认证真机运行 |
| Web wasm32 | Emscripten 5.0.7、Godot 4.5 API；`nothreads`/`threads` SDK 与 debug/Release side module 均真实构建，Release 大小 1,015,600 / 1,026,366 B，dylink 与入口通过，shared memory 差分正确，SDK/项目绝对路径泄漏为 0 | ✅ 编译/链接/Wasm；官方模板与 Chromium CI 待首次留档 |
| 官方语料脚本总数 | 46 | ✅ 固定输入 |
| 脱离项目上下文的单文件编译 | 22 / 46（47.83%） | 🟡 |
| 3D platformer 项目编译 | 8 / 8 脚本 | ✅ |
| 3D platformer 生成 C++ | 全部翻译单元通过真实 C++17 语法编译 | ✅ |
| 3D voxel 项目编译 | 12 / 12 脚本 | ✅ |
| 3D voxel 生成 C++ | 全部翻译单元通过真实 C++17 语法编译 | ✅ |
| rhythm game 项目编译 | 12 / 12 脚本 | ✅ |
| rhythm game 生成 C++ | 全部翻译单元通过真实 C++17 语法编译 | ✅ |
| role playing game 项目编译 | 14 / 14 脚本 | ✅ |
| role playing game 生成 C++ | 全部翻译单元通过真实 C++17 语法编译 | ✅ |
| role playing game 降级到 Godot 4.6 SDK | 同一源码以 4.6 能力表重新生成并通过真实 C++17 语法编译 | ✅ 本地审计，待纳入 CI |
| Dungeon Rush 4.5 商业项目转译 | 195 个外部脚本和 2 个场景内嵌脚本，共 197 / 197 编译单元 | ✅ 本轮实机 |
| Windows 4.5 商业语料原生构建 | Godot 4.5.2、MSVC、C++17；Release DLL 链接、导出与运行成功 | ✅ 实机 |
| Dungeon Rush 4.5 无源码导出 | AOT PCK 1953 个项目文件、331 个场景/资源全部可加载；源码/编译器/SDK/中间物泄漏为 0，完整目录仅一个项目 DLL | ✅ 本轮实机 |
| Dungeon Rush 4.5 AOT 导出日志 | 退出码 0；317 个场景、431 个脚本节点、15,931 个存储属性完成转换；无转换错误 | ✅ 本轮实机 |
| Dungeon Rush 4.5 Windows 交互运行 | 越过黑屏/启动画面后独立验证连续菜单背景动效，两轮菜单/关卡切换覆盖设置、移动、翻滚、攻击、背包、暂停/恢复和退出；窗口始终响应，运行错误为 0 | ✅ 本轮实机 |
| Dungeon Rush 4.5 Windows 声音 | 菜单音乐峰值约 0.142，关卡内背包 UI 音效峰值约 0.418；会话未静音且音量非零 | ✅ 本轮实机 |
| Castle Defense 4.5 商业项目转译 | 178 个外部脚本和 2 个场景内嵌脚本，共 180 / 180 编译单元 | ✅ 本轮实机 |
| Castle Defense 4.5 macOS 无源码导出 | 隔离审计 1557 个 PCK 文件、288 个场景/资源全部可加载；项目库 1 个，源码/编译器/SDK/中间物违规为 0 | ✅ 本轮实机 |
| Castle Defense 4.5 macOS 功能门禁 | 19 个断言、5 个输入动作、失败 0；覆盖主菜单 BGM、解谜未解锁弹窗、普通模式、教程和暂停/恢复 | ✅ 本轮实机 |
| NoahEngine 项目编译 | 修正 3 处项目语义笔误并为 `WaveformData` 提供 Runtime 类型契约；项目与 addon 的 122 / 122 GDScript 单元全部原生链接成功 | ✅ 本轮实机 |
| NoahEngine 第三方扩展边界 | `WaveformData` 由仅含 Windows 二进制的 GDExtension 提供；Runtime 桥接可完成类型检查和生成，真实运行仍要求供应商二进制存在并先注册 ClassDB | ✅ 失败关闭边界 |
| 固定 GDScript/AOT 运行差分 | macOS、Ubuntu 24.04、Windows 11，官方 Godot 4.5.2；每平台 3 轮 AB/BA、13 类工作负载各 15 个样本、每种实现 360 帧；行为 oracle 与全部性能预算通过 | ✅ 本轮实机 |
| 示例项目无源码导出 | 三桌面平台实际 Release PCK/完整发行物审计通过；Android arm64 Release APK 包内容审计通过 | ✅ 当前固定平台范围 |

单文件成功率和项目成功率必须分开理解。单文件编译没有项目级 `class_name`、脚本继承、Autoload
和资源路径图，因此 platformer 的 5 / 8、voxel 的 9 / 12、RPG 的 3 / 14 单文件成功不代表
项目只能编译这些脚本；项目编译器建立符号图后，四者分别达到 8 / 8、12 / 12、12 / 12 和
14 / 14，
并且生成代码均通过真实 C++17 编译与动态库链接。

三桌面平台现已不只是编译审计：固定示例实际完成官方 Godot 4.5.2 插件加载、Release 导出、
独立运行、行为 oracle 和发行内容检查。该范围仍不等价于 GUI 编辑器热重载、debug 导出、签名、
最低系统、长时间运行或任意客户项目认证。Android 只完成 macOS 主机交叉构建和 APK 审计，
ADB 没有连接设备，因此真机输入、音频和生命周期仍未验证。Web 当前完成原生编译和 Wasm
结构认证，官方导出模板、PCK 与浏览器运行由新建的独立 CI 门禁负责，首次流水线结果落档前
不算多浏览器运行认证。完整商业承诺继续以 C4/C6 和 M7
门禁为准，详细环境与负面性能结果见[跨平台实测报告](PLATFORM_TEST_REPORT.md)。

当前可以准确表述为：

- [x] GDPP 已能把四个固定版本的官方中等复杂 Godot 4 项目完整转译为可链接的 GDExtension
  原生库。
- [x] GDPP 已能把 Dungeon Rush 的 197 个项目编译单元完整转译并链接为 Godot 4.5 原生库；
  该门禁覆盖场景 Autoload、场景内嵌脚本、结构化 `await`、第三方 GDExtension 单例和跨脚本
  字段初始化。
- [x] Dungeon Rush 已在 Windows 11、官方 Godot 4.5.2 和 MSVC 下完成无源码 Release 导出、
  PCK/完整目录内容审计以及两轮菜单/关卡真实交互；该结论只覆盖此项目和这组已记录环境，不外推为
  任意项目兼容。
- [x] Castle Defense 已在 macOS、官方 Godot 4.5.2 和 AppleClang 下完成 180 单元原生化、
  无源码 Release 导出、全 PCK 资源加载、90 帧主界面捕获及关键菜单/关卡功能门禁；该结论同样
  只覆盖当前固定项目快照。
- [x] NoahEngine 已按商业集成方式直接修正项目自身的 `match` 目标笔误和类型判断误写，并给
  `WaveformData` 增加不猜测 C++ 布局的 Runtime 类型契约；项目与原生 addon 内的 122 / 122
  GDScript 单元全部完成转译和原生链接。
  供应商当前只交付 Windows 二进制，所以其他平台只能做编译审计，不能把缺少供应商库的平台
  宣称为运行通过。
- [x] 未支持语法以稳定诊断结束，不输出可被误认为成功的半成品。
- [x] 固定 Godot 4.5.2 GDScript/AOT 矩阵已在 macOS、Linux、Windows 比较值、嵌套写回、
  lambda、`match`、字符串、信号、64 位溢出/位运算、13 类热路径、启动、帧工作量和发行体积；
  行为差异为 0，阈值失败为 0。Linux 另完成 RSS；其他两平台没有可靠 RSS，未伪报结果。
- [ ] 尚不能表述为“任意最新版 GDScript 无修改直接 AOT”。
- [ ] 尚不能表述为“运行语义与最新版 Godot 完全一致”或“所有游戏都达到超高性能”；固定矩阵
  已可重复，但还没有覆盖完整语言、全部引擎版本/平台和真实游戏内容。

## Dungeon Rush 4.5 商业项目门禁

`build/dungeon_rush` 是本地商业项目语料，不进入 Git 或公开 CI。2026-07-14 的审计输入包含
195 个外部 GDScript 文件和 2 个场景内嵌脚本，共 197 个编译单元；外部脚本约 9,881 行。
项目源码中两处写法由 Godot 4.5 规则判定为非法，且也不是任何当前支持的更高版本才提供的
新语法：

- [x] `skill_6_bollet.gd` 曾用单引号包住跨行代码模拟注释。普通单引号字符串不能跨行；该段本意
  是停用代码，已改成逐行 `#` 注释。
- [x] `props_generate.gd` 曾声明 `@export var kind = null`。导出属性必须提供可推断或显式的
  Inspector 类型；该字段按项目实际类别数据改为 `@export var kind: String = ""`。

修正项目源码后，门禁不是只检查“前端返回成功”，而是完整执行：

- [x] 使用 Godot 4.5 能力表扫描和转译 197 / 197 个外部及内嵌脚本单元。
- [x] 游戏内容目录统一为跨平台安全的 `lower_snake_case`，143 个目录和 1009 个文本资源引用
  完成迁移；插件发布契约内的 `addons/` 目录不做破坏性改名。
- [x] 解析 `Transition="*res://global/transition.tscn"`，从场景根节点解析真实脚本 Autoload。
- [x] 目录移动后生成文件名复用不会被旧清单误删，并以“移动全局命名脚本”回归测试锁定。
- [x] 将实例字段初始化下沉到生成 `.cpp` 构造函数，避免跨脚本头文件循环和不完整类型访问。
- [x] 将 GodotSteam 注册的 `Steam` 识别为第三方引擎单例，通过运行时 `Engine` 单例表调用。
- [x] 使用 AppleClang 与 Windows MSVC 的 C++17 工具链和 Godot 4.5 godot-cpp 完成全部生成
  代码编译及动态库链接。
- [x] 普通 Godot 4.5.2 Windows 进程可发现 Visual Studio/Build Tools 并初始化 `vcvars`，无需
  客户从 Developer Command Prompt 启动编辑器。
- [x] 导出转换保留场景层级、owner、持久信号连接、Unicode NodePath 和 `%UniqueNode` 语义；
  继承场景与嵌套 PackedScene 的脚本覆盖、显式 `script = null` 和连接归属均按 SceneState 链
  转换，不重复展开或重复序列化连接。
- [x] 普通实例字段注册为原生脚本属性，跨脚本动态读写 `open_bag`、`delet_pressing`、
  `stuff_list`、`can_buy` 等字段不再因只注册 `@export` 字段而失败。
- [x] Variant 动态左值按完整访问链只求值一次，并在修改叶节点后逐层反向回写；Dungeon Rush 的
  78 处活动多段赋值候选已统一审计，包含
  主菜单 `position.y`、激光/碰撞体 `scale.x`、武器朝向以及
  `Main.props_data[key].exist/strengthen/exist_times`、符文等级等对象和值类型/容器混合路径均进入
  同一生成逻辑，不再只修改临时 `Vector2`、`Color` 或 Dictionary 元素副本。
- [x] Release 导出只携带 4,206,592 B（约 4.01 MiB）的项目原生库；AOT PCK 1953 个文件和
  331 个场景/资源的审计违规数为 0，compiler、SDK 和 fallback 均未进入成品。
- [x] 场景替换采用“先创建并复制全部节点、全部成功后再交换”的事务；Godot 动态 metadata
  单独复制，任何普通存储属性缺失都会阻断，不会交付部分转换场景。
- [x] Godot 4.5 导出缓存使用转换修订与项目构建 ID 隔离，失败导出留下的旧 `.scn` 不会在修复后
  被复用；该问题由 PCK 全资源加载门禁真实检出。
- [x] 生成的预加载缓存和脚本静态存储在 scene 级 terminator 中按注册逆序释放；Windows 成品
  直接运行关卡 1800 帧后退出，Resource、RID、脚本和服务器生命周期诊断均为 0。
- [x] Windows Release 成品通过 DPI 感知的桌面交互门禁：等待黑屏和工作室启动画面结束后，
  主菜单非 UI 背景区域在 0.8 秒内产生 11.80 的连续帧差值；设置开关、进入关卡、移动、翻滚、
  攻击、背包开关、暂停冻结、暂停内设置、恢复、退出到主菜单、再次进入关卡和再次移动均产生
  预期画面变化，所有阶段窗口均响应。
- [x] 菜单音乐和关卡内背包 UI 音效均检测到非零 Windows 音频峰值，会话未静音且音量非零；
  完整运行日志中的脚本、NodePath、动态属性/调用、重复连接和资源加载错误均为 0。
- [x] 修复客户场景中暂停后菜单继承可暂停处理模式导致按钮不接收输入的问题，暂停设置与退出
  均通过真实鼠标命中测试。
- [ ] 尚未建立解释执行版与 AOT 版的完整行为 oracle，也未走完正式战斗波次、存档、联网和
  长时间游戏流程，因此不能把本次 C5 项目交付门禁表述为完整 C4 语义认证。

## Castle Defense 4.5 商业项目门禁

`build/castle_defense` 是第二个本地商业项目语料，不进入公开发行包。项目包含 178 个外部脚本
与 2 个场景内嵌脚本。它暴露的问题不是用项目专用分支绕过，而是收敛为以下编译器、导出器和
运行时通用契约：

- [x] 180 / 180 个编译单元生成 C++17 并链接为 release 项目库；8 个 Node Autoload 在字段
  初始化前登记 `ObjectID`，消除原生节点尚未命名或挂入 SceneTree 时读取前置 Autoload 的崩溃。
- [x] 代码生成缓存改为 v3 manifest 和自动契约指纹，分离实现、公开 ABI、脚本引用和第三方
  provider ABI。前端、语义、IR、后端、runtime、公共头文件或 API 表变化都会拒绝不兼容旧 C++。
- [x] 项目原有 `spawn_enemy()` 实际只被 fire-and-forget 调用，却声明动态返回并返回对象；按
  Godot 4.5 的 GDS4103 规则直接改为 `-> void` 并删除无消费方的返回值，不在编译器里屏蔽告警。
- [x] `Array[TimelineAction]` 与同名 `.tres` 嵌入脚本曾使元素类型退化为 Variant；项目全局
  `class_name` 现在优先于路径显示名，泛型元素类型进入真实依赖图，且有 ABI 精确失效回归。
- [x] Godot 4.4/4.5 不再启用会尝试反序列化每个导出文件的全局 resource customizer；导出器
  在 `_export_file` 只处理 `.tscn/.scn/.tres/.res`，并只读取 SceneState 或
  `PROPERTY_USAGE_STORAGE` 声明的序列化值，避免触发与当前对象形态无关的可选 getter。
- [x] 资源图转换递归处理内嵌 Resource、Array、Dictionary 和 PackedScene，用对象身份表保留
  自引用、循环与共享子资源；场景替换按 SceneState 精确保留继承覆盖、owner、分组、metadata
  及持久连接的 flags、binds 与 unbind。
- [x] C++ 后端区分 godot-cpp 的 `Node::get_node<T>()` 与非模板
  `Node::get_node_or_null()`，并用生成代码回归测试锁定，避免真实项目编译阶段才暴露模板 API
  误用。
- [x] release 导出摘要为 201 个场景、201 个脚本节点、37 个资源和 1279 个存储属性；AOT PCK
  为 38,931,144 B；审计器先清除宿主残留的全部架构项目库，再隔离审计 1557 个包内文件、加载
  288 个场景/资源、发现 1 个项目动态库，违规数为 0。
- [x] macOS Universal 2 导出将 Godot 的 `universal` 导出扫描目标与最终进程的
  `arm64`/`x86_64` 运行 feature 分离；compiler 与项目描述符在导出期间事务式切换并恢复，
  单个项目 dylib 同时含两种切片，连续导出没有架构选择警告。
- [x] 官方 Godot 4.5.2 AOT 运行退出码为 0，脚本、资源和 GDExtension 加载错误为 0；强制短时
  退出残留的 2 个 BGM 音频资源在原始 GDScript 版同样存在，不归类为 AOT 差异。
- [x] 外置 QA 以全新用户目录执行 19 个断言和 5 个输入动作，覆盖主菜单 BGM、解谜未解锁
  提示、普通模式进入、教程初始化、暂停与恢复，失败为 0；90 帧 AOT 画面捕获包含完整主界面和
  连续动效。QA 文件仅位于根 `build/`，不会污染客户源码或 PCK。
- [ ] 尚未覆盖完整战斗波次、所有塔/装备组合、存档升级、长时间压力和解释器/AOT 逐帧 oracle；
  因此该项目通过的是当前 C5 交付路径，不代表 C4/C6 已整体完成。

## 商业兼容等级

| 等级 | 验收内容 | 当前状态 |
|---|---|---|
| C0 词法 | Token、源码范围、非法输入和错误恢复 | 🟡 数字、字符串、Unicode 标识符/关键字防伪、布局和可配置资源上限已完成；固定官方 parser 与恶意输入通过，仍缺持续 coverage-guided fuzz |
| C1 语法 | AST、优先级、结合性、声明和控制结构 | 🟡 Godot 4.7 官方合法 parser 语料 114 / 114 前端接受，官方非法语料 76 / 76 最终拒绝；`await` 已按官方优先级建模为强类型表达式，短路、三元、`while` 条件、match 守卫、match 分支正文和 debug-only assert 操作数均进入控制流感知 lowering |
| C2 语义 | 名字解析、类型、作用域、项目符号和版本能力 | 🟡 四个官方项目主路径通过 |
| C3 生成 | 有效 C++17、稳定求值顺序、GDExtension ABI | 🟡 四个官方项目原生链接通过 |
| C4 运行 | 值、错误、副作用、信号和生命周期与 Godot 差分一致 | 🟡 固定 4.5.2 oracle 已在三桌面平台通过，完整语言、4.4/4.6/4.7、移动端与长时间矩阵未完成 |
| C5 交付 | 场景/资源替换、Autoload、导出、源码剥离和失败回滚 | 🟡 示例与 Dungeon Rush Windows 4.5 闭环已建立，任意项目未认证 |
| C6 商业认证 | 全平台 CI、性能预算、压力测试、升级和回滚承诺 | ⛔ 未达到 |

## 最新版 GDScript 语言能力

### 词法、字面量和声明

| 能力 | 状态 | 已实现边界与剩余缺口 |
|---|---|---|
| 缩进、换行、注释、括号内换行 | ✅ | 生成 `INDENT`/`DEDENT`；CRLF/LF、BOM、文件末尾、空白/注释行、显式续行、混合缩进、全文件缩进字符一致性均有回归 |
| 十进制、浮点、科学计数法、数字分隔符 | ✅ | 校验下划线与 int64 边界；浮点复现 Godot 的 18 位尾数和指数算法，稳定生成最大有限值、INF、NAN 与下溢结果，不依赖宿主 `strtod` |
| 十六进制与二进制整数 | ✅ | 支持 `0x`/`0X`、`0b`/`0B` 及分隔符检查；当前 GDScript 不支持的 `0o` 八进制明确报错 |
| 普通、raw 与三引号字符串 | ✅ | 支持单/双引号、raw、三引号、raw 三引号、控制转义、续行、UTF-16/UTF-32 和代理对；未知转义失败关闭，NUL 与 Godot 一致替换为 U+FFFD |
| `StringName`、`NodePath` 字面量 | ✅ | 支持 `&"name"`、`^"path"` 以及官方允许的三引号形式 |
| 节点/唯一节点简写 | ✅ | 支持 `$Path`、`$"Path"` 和 `%UniqueNode` |
| Unicode 标识符与关键字防伪 | ✅ | 固定 Godot 4.7 stable 的 Unicode 17.0 `XID_Start`/`XID_Continue` 表；按 ICU/UTS #39 可观察规则做 NFD、忽略码点过滤与 confusable skeleton，拒绝伪装关键字；非法 UTF-8/NUL 失败关闭，后端使用单射 ASCII 编码避免工具链差异与符号碰撞 |
| `extends`、`class_name` | ✅ | 支持引擎类型、项目全局类、`res://` 与相对脚本路径 |
| `var`、`const`、`:=`、静态字段 | ✅ | 字段和局部声明、类型推断、常量写保护和 C++17 静态字段已接通；静态初始化器和静态访问器不能读取实例状态，带 getter/setter 的静态字段仍通过静态访问器而非绕过 backing 规则。非工具脚本的静态存储和 `_static_init` 不在编辑器执行；工具脚本跨模式读取其静态字段得到 null，写入不污染游戏态存储 |
| 普通/静态函数、参数、默认值、返回类型 | ✅ | 参数顺序、重名、默认值类型及覆盖区间/逆变/协变均在生成前检查；默认表达式在 Semantic/HIR 中区分可折叠常量和调用时求值，可变容器、字段及函数依赖每次省略实参都会重新计算；静态函数及其 lambda 对 `self`、实例字段/信号、节点简写、脚本与引擎实例成员统一失败关闭，静态 `super` 调用保持可用 |
| 信号声明 | 🟡 | 生成 `ADD_SIGNAL`，支持常用发射、方法引用和顶层等待路径；连接生命周期尚无完整差分 |
| 命名与匿名枚举 | ✅ | 常量表达式、跨脚本/继承访问、Inspector 枚举 hint 和 `match` 已接通；匿名枚举值与普通常量使用独立符号种类 |
| 内部类 `class` | ✅ | 顶层及递归嵌套内部类的字段、常量、枚举、信号、方法、`_init`、继承、`super`、限定访问、强类型 `Ref<T>` 构造、依赖顺序、ClassDB 注册和项目缓存身份已贯通；嵌套源身份以限定名保存，C++ 原生名稳定扁平化 |
| lambda 与捕获 | 🟡 | 支持多行 `func(...):` 表达式、类型/默认参数、返回类型、值捕获、参数数量检查和生命周期受检原生 `CallableCustom`；递归 lambda、lambda 内 `await` 与完整捕获差分尚未认证 |

### 类型、表达式和调用

| 能力 | 状态 | 已实现边界与剩余缺口 |
|---|---|---|
| 基础类型和 `Variant` | ✅ | 精确建模 Godot 的完整 Variant 类型域、`null`/`void`/对象/Ref 可空性与零值真值规则；32 类 Variant 条件值已通过 Godot 4.7.1 GDScript/AOT 差分 |
| `Array[T]` | ✅ | 字段、局部变量、参数、返回值、Signal、属性和跨脚本公开签名均生成真正的 `godot::TypedArray<T>` ABI；内建值、引擎对象、内部类、项目脚本和 ClassDB 反射的第三方 GDExtension 类均保留运行时元素类型。对象元素使用只携带 `get_class_static()` 的轻量标签，避免互相引用脚本产生 C++ 头文件环。`Array[Variant]` 按 Godot 规则等价降低为未约束 `Array` |
| `Dictionary[K, V]` | ✅ | 生成真正的 `godot::TypedDictionary<K,V>` ABI，并在 ClassDB 属性、方法和 Signal 元数据中发布 `PROPERTY_HINT_DICTIONARY_TYPE`；键、值、下标和常用变更方法参与静态检查。`Dictionary[Variant,Variant]` 保持 Godot 的未约束运行语义，单侧 `Variant` 仍保留另一侧约束 |
| 数组/字典字面量 | ✅ | 按源码顺序构造；进入强类型字段、局部变量、默认值、返回值、赋值或调用参数时执行上下文类型化并逐元素/键值检查。不同元素签名的强类型容器按 Godot 不变规则拒绝；官方禁止的嵌套强类型容器和 `void` 元素在生成 C++ 前失败关闭 |
| 未约束容器的运行时转换 | 🟡 | 字面量、`Variant`、普通 Array/Dictionary、PackedArray 和参数化 `as` 边界均遵循 Godot 的分析器接受规则并使用精确元素/键值元数据守卫；动态值携带完全相同元数据时已通过 Godot 4.7.1 GDScript/AOT 差分。运行时失败的报错位置和函数中止时序仍需完整复现 |
| 调用与运算求值顺序 | ✅ | 函数、方法、工具函数、动态方法和 Callable 参数均先按源码顺序保存临时值；全部 eager 二元运算先物化左操作数再物化右操作数，短路与三元只执行选中分支。浮点、字符串、Variant、内建值、membership、幂、比较、整数及链式比较通过 19 步 GDScript/AOT 副作用轨迹 |
| 成员、属性和下标 | ✅ | 静态 API 走直接调用，动态值集中走受检 `Variant` runtime |
| 引擎对象 `.new()` | ✅ | 区分 Object 与 RefCounted，分别生成裸对象或 `Ref<T>` |
| 项目脚本 `.new()` | ✅ | 校验 `_init` 参数并生成强类型原生实例；工具脚本在编辑器中构造非工具脚本会按 GDScript 语义返回 null，不执行原生构造、字段初始化或 `_init` |
| `preload`/`load` | 🟡 | 项目 `.gd` 字面量路径、普通资源和动态 `load` 主路径可用；成员预加载、`const preload` 与脚本静态存储会在 Godot scene 级终止前释放，完整 Resource 语义仍未差分 |
| 算术、比较、位运算、移位、幂 | ✅ | 由版本化内建运算符表校验；整数加减乘、取负、位运算、移位及非零除余在常量、优化、静态 C++ 与动态 Variant 路径共用模 2^64 契约。官方 x86_64 Godot 4.4.1/4.5.2 解释器会在 `INT64_MIN / -1` 处触发 SIGFPE，因此 x86_64 只与原版 GDScript 差分其余安全子集，AOT 仍在所有目标独立验证完整边界；arm64 保留完整 GDS/AOT 极值差分，除零的当前函数中止时序尚未认证 |
| `and`/`or`/`not` 与 `&&`/`||`/`!` | ✅ | 静态和动态路径均保留短路；RefCounted 真值使用 `Ref::is_valid()` |
| `in` | ✅ | 由静态运算符表或动态 `Variant` 路径处理 |
| `is` / `is not` | ✅ | 支持引擎类型、项目脚本类型和内建 Variant 类型，`not` 优先级按 GDScript 解析；类型事实贯通真假分支、短路、三元、循环、后支配路径和结构化 `match`，赋值时保守失效 |
| `as` | ✅ | 支持对象/Ref 降型、参数化 `Array[T]`/`Dictionary[K,V]` 与内建 Variant 类型；常量表达式按严格构造矩阵检查，运行时表达式生成源类型和目标构造能力守卫，分析器接受但 VM 必然失败的 String/RID 等存储路径在生成前拒绝 |
| 三元表达式 | ✅ | 支持 `value if condition else fallback` 和结果类型合并 |
| 复合赋值 | ✅ | 支持算术、幂、位与移位复合赋值，并先验证对应运算符；类型化整数左值、索引和右值均单次且按源码顺序求值，边界结果不依赖宿主 C++ 溢出行为 |
| 嵌套值属性写回 | ✅ | 动态对象属性、Array/Dictionary 下标和值类型成员使用单次求值访问链并逐层反向回写；已在 Godot 中运行验证 `Node2D.position.y`、脚本成员/局部 Variant `Color.a`、`Transform3D → Vector3.x` 和 `Array → Dictionary → Color.a`，逐值类型差分矩阵仍归入完整 oracle 工作 |
| Callable | 🟡 | 脚本方法引用、继承的引擎方法、原生 lambda、`.call(...)` 和 `.bind(...)` 主路径已实现；完整 bind/unbind 与捕获/连接生命周期差分未认证 |

### 控制流和高级语义

| 能力 | 状态 | 已实现边界与剩余缺口 |
|---|---|---|
| `if`/`elif`/`else` | ✅ | 作用域、控制流合并和部分不可达检查已实现；无副作用常量条件会删除未选 HIR 分支，并在 MIR 折叠对应边 |
| `while` | ✅ | 支持循环控制和 `while true` 的返回分析；字面量 `while false` 在保留官方静态语义检查后从生成代码删除 |
| `for`、显式迭代变量类型 | 🟡 | int/range、float、Vector2/Vector2i、Vector3/Vector3i、String、Array、Dictionary、动态协议、静态对象 `_iter_init/_iter_next/_iter_get` 和十种 PackedArray 均有显式语义策略。同步数学范围生成零装箱原生循环并严格匹配方向、步长和边界；类型化变量会约束 Array/Dictionary 字面量并逐元素/键诊断。String 使用 Unicode 码点快照，Array/PackedArray 观察循环内增长，Dictionary 精确推断 key。官方 4.7 迭代接受 3/3、拒绝 2/2 及 Godot GDScript/AOT 运行差分已通过；异步续体仍统一走正确但未专门化的 Variant 协议 |
| `return`/`pass`/`break`/`continue` | ✅ | 检查循环位置和返回类型 |
| `assert` | ✅ | 条件与可选消息各自拥有专用 HIR 前缀和 MIR 控制边；debug 支持二者中的 Signal `await`，条件成功不求值消息，失败才进入消息续体；release 经 C++ 预处理器移除全部条件、消息、信号连接和失败报告，仅执行断言后的业务续体 |
| 基本 `match` | ✅ | 常量、多备选、通配符、`var` 绑定和 `when` 守卫可用；目标只求值一次；异步守卫在模式绑定后挂起，恢复为假会继续下一分支，分支正文挂起后仍回到 match 外层续体 |
| 集合/嵌套/rest `match` 模式 | ✅ | Array/Dictionary 模式、递归嵌套、尾部 `..`、字典常量键和类型化绑定已贯通 AST、语义、HIR、验证、优化及 C++17 后端 |
| 属性 `get`/`set` 块 | ✅ | backing 字段、递归保护、ClassDB 属性和自定义访问器已接通；普通与静态访问器共享同一可见性规则，内部类静态属性的限定读写生成 `Class::_gdpp_get/_gdpp_set` |
| `set = method`/`get = method` 绑定形式 | ✅ | 支持同行与缩进布局，验证方法存在性、静态性、参数和返回类型，并避免绑定方法递归访问属性 |
| `super` 与父方法显式调用 | ✅ | `super(args)` 调用当前方法的父实现，`super.method(args)` 显式调用指定父方法；引擎父类和项目脚本父类均生成限定 C++ 调用。GDScript 不支持以 `super.property` 直接访问父属性，因此不把它列为兼容缺口 |
| 引擎虚函数覆盖 | 🟡 | 使用 API 虚函数表、精确 C++ `override` 和 const bridge 检查；仍需覆盖全部虚函数与平台 ABI 差分矩阵 |
| `await`/协程 | 🟡 | `await` 是具有官方优先级的强类型 AST 表达式；HIR 以 A-normal form 将变量初始化、普通赋值右值、算术、容器元素、调用参数以及普通 `if`/`for`/`match` 主体中的挂起点统一降低为有序续体临时值，并在挂起前稳定先求值的接收者、索引和参数。短路 `and/or` 与三元表达式使用分支 ANF，只执行选中的挂起分支；`while await` 每轮重新计算条件。match 分支使用显式 `guard_prefix`：模式匹配和绑定先完成，守卫挂起恢复为假后继续下一分支，正文挂起恢复后进入 match 外层续体；后端为每个异步 match 生成一个 `weak_ptr` 回指的索引调度器，每个分支和外层续体只生成一次，代码体积随分支数线性增长且不形成自引用环。`assert` 的条件和消息使用两个独立的 debug-only 前缀，消息只在失败边挂起，公共业务续体只生成一次，Release 预处理后不存在任何断言求值或 Signal 连接。实例协程现以 `Variant` 原生入口承载“立即完成值或每次调用独有的完成 Signal”：同步分支直接返回原值，挂起分支在恢复后发射单个结果，`void` 恢复为 `null`；并发调用不会共用状态或串值，完成后回收动态 Signal。已知协程直接调用会以 GDS4132 失败关闭；AOT→AOT、GDScript→AOT、跨脚本调用、同步/异步返回和同步基类/异步派生覆盖均有 Godot 4.7 原生差分。项目符号图以语义固定点区分真正挂起与 `await 42` 立即表达式，协程身份进入公开 ABI 和精确增量失效；ABI 不同的覆盖方法使用独立 C++ 符号并经 ClassDB 动态派发。异步 `for/while` 将循环携带且会被写入的外层局部值、函数入口参数和属性 setter 参数提升到共享单元，挂起后的 `break`/`continue` 走显式恢复边；循环函数使用 `weak_ptr`，仅由当前恢复链持有强引用，不形成自引用环。Signal 使用一次性连接和 `ObjectID` 取消失效对象。静态函数、构造函数、同名 shadow 局部的统一 SymbolID 帧、完整 lambda 协程及异步引擎虚函数的可等待外部调用仍失败关闭或尚未认证 |
| `breakpoint` | ⛔ | 尚无语句节点和调试器桥接 |
| RPC | ✅ | `@rpc` 的 `authority`/`any_peer`、`call_remote`/`call_local`、`unreliable`/`unreliable_ordered`/`reliable` 和整数 channel 按 Godot 默认值规范化为强类型配置；参数类别可按官方规则交换顺序，重复类别、非法值、非常量和超量参数稳定报错。Node 派生类在原生构造阶段调用 `rpc_config()`，已通过 Godot 4.4 ABI 编译与 Godot 4.5.2 运行查询；非 Node 脚本与官方 parser 一样允许声明，但没有可执行的 Node RPC 端点 |

## 注解能力

| 注解范围 | 状态 | 当前支持 |
|---|---|---|
| 基础导出 | ✅ | `@export` |
| 数值、枚举、标志 | ✅ | `@export_range`、`@export_enum`、`@export_flags` |
| 集合元素与层掩码 | ✅ | 上述数值/枚举/标志注解可校验 Array、PackedArray 元素类型；支持 2D/3D render、physics、navigation 与 avoidance layer flags，以及 `@export_exp_easing` |
| 路径 | ✅ | `@export_file`、`@export_global_file`、`@export_dir`、`@export_global_dir` |
| 文本、颜色、节点路径 | ✅ | `@export_multiline`、`@export_color_no_alpha`、`@export_node_path` |
| Inspector 分组 | ✅ | `@export_group`、`@export_subgroup`、`@export_category` |
| 生命周期 | ✅ | `@onready`，并在 `_ready` 用户代码之前初始化；必要时自动生成 `_ready` |
| 警告控制 | ✅ | `@warning_ignore`、`@warning_ignore_start`、`@warning_ignore_restore` 支持全部 Godot 4.7 warning 名称、多参数范围和块尾恢复；函数、内部类、字段初始化器、属性访问器及嵌套语句使用可恢复作用域。未知 warning、非字符串参数、重复 start 和无对应 restore 均稳定诊断；内部 `GDSxxxx` 诊断码不能替代官方 warning 名 |
| 抽象类与抽象方法 | ✅ | `@abstract` 支持脚本、递归内部类和无函数体方法；具体类必须实现本地、跨脚本及本地根基类的全部抽象契约。抽象构造与未实现的 `super` 调用在语义阶段失败；HIR 保留非执行契约，C++ 生成纯虚方法并以 `GDREGISTER_ABSTRACT_CLASS` 注册。Godot 4.4 与 4.7 已验证抽象 ClassDB 类不可实例化、具体派生类可动态分派 |
| 工具脚本 | ✅ | `@tool` 贯通 AST、项目元数据、ClassDB 注册和 C++17 后端；编辑器中执行工具脚本的构造、字段初始化、`_init` 与惰性 `_static_init`，非工具脚本使用 runtime placeholder。跨模式允许常量和静态方法，实例构造及静态字段按 Godot 隔离；`EditorPlugin`/`EditorScript` 必须声明 `@tool`，继承图不得混合工具与非工具执行模式。Godot 4.7.1 编辑器态与游戏态原生测试通过 |
| 类图标 | ✅ | `@icon` 要求非空字符串字面量并保留到 HIR/编译结果；项目阶段将脚本相对路径规范化为 `res://`，接受 `uid://`，拒绝根目录逃逸、外部 scheme 和反斜杠路径。生成描述符按实际原生 ClassDB 类名写入 `[icons]`，开发构建重定向和增量缓存不会丢失；Godot 4.7.1 已加载带图标节的原生扩展 |
| 静态卸载 | ⛔ | `@static_unload` 尚未实现；Godot 4.7 官方文档说明当前脚本即使带注解也不会实际释放。GDPP 已在扩展 scene terminator 释放所有静态资源，但在建立可观测的原生脚本资源引用生命周期前，不用最后实例计数近似冒充该注解语义 |
| 网络 | ✅ | `@rpc` 参数验证、默认值、类型化 HIR、C++ 配置和 Node 运行时注册已实现，单 peer `call_local` 已通过 `Node.rpc()` 实际调用；真实多 peer 传输、权限拒绝、排序/丢包和对象序列化压力仍归入网络运行差分矩阵，不以单机调用替代端到端认证 |

- [x] 支持的导出注解检查参数数量、参数字面量类型和字段可序列化类型。
- [x] `@rpc` 参数只在语义阶段规范化一次，HIR 和后端消费同一强类型契约；常量字符串、常量
  整数 channel、默认值及重复类别均有失败关闭测试。
- [x] 未支持注解产生诊断，不会被无声丢弃。
- [ ] 仍需从 Godot 4.7 注解注册表生成完整位置、参数和版本测试矩阵。

## GDScript 标准库与版本能力表

语言标准库包括全局工具函数、语言常量、全局枚举、内建 `Variant` 类型及其构造器、方法、
成员、常量和运算符。引擎对象类与单例也被版本化索引，但不计入下表的语言标准库数量。

| Godot 4.7 数据 | 官方表项 | 当前编译器接入 | 状态 |
|---|---:|---|---|
| 非空内建类型 | 38 | 全部索引并可解析 | 🟡 缺逐类型运行差分 |
| 内建类型方法 | 999 | 全部索引，执行参数数量/类型检查 | 🟡 缺逐重载运行矩阵 |
| 内建构造器 | 156 | 全部索引并进行重载匹配 | 🟡 |
| 内建成员 | 62 | 全部索引，常用直接成员可生成 | 🟡 |
| 内建常量 | 210 | 全部索引，常用值类型常量可生成 | 🟡 |
| 内建运算符 | 749 | 全部进入静态语义检查和返回类型推导 | ✅ 编译链完成 |
| 全局工具函数 | 114 | 全部进入名字解析、参数检查和 `UtilityFunctions` 生成路径 | 🟡 缺 114 项原生/运行矩阵 |
| 全局枚举 | 22 类、517 个值 | 全部进入名字解析和代码生成 | 🟡 缺逐项运行矩阵 |
| 全局整数常量 | 11 | 4.7 表项全部索引 | 🟡 缺逐项矩阵 |
| GDScript 语言常量 | `PI`、`TAU`、`INF`、`NAN` | 专门解析并生成 | ✅ |
| 语言辅助函数 | `range`、`len`、`load`、`preload`、`convert`、`type_exists`、`char`、`ord`、`Color8`、`is_instance_of` | 集中签名、类型检查、常量分类和 runtime lowering；Godot 4.4 未注册的 `ord` 以固定 Unicode 码点验证 AOT，4.5～4.7 执行真实 GDScript/AOT 差分 | 🟡 其余非恒定语言函数待实现 |

版本规则：

- [x] `--target-godot 4.4|4.5|4.6|4.7` 只使用对应能力表；引用目标版本不存在的 API 会在语义阶段失败。
- [x] `--target-godot 4.7` 使用 4.7 类型、方法、属性、单例、工具函数、枚举、常量和运算符表。
- [x] 前端只维护最新版 GDScript 语法；目标 API 版本独立选择，可把最新版纯语法降低为只依赖
  Godot 4.4 的代码，不维护 4.4～4.7 多套 parser 方言。
- [ ] 每个标准库表项仍需自动生成“合法参数、非法参数、边界值、原生编译、4.4～4.7 运行”测试。

## 项目级 AOT 能力

| 能力 | 状态 | 当前实现 |
|---|---|---|
| 全项目脚本扫描 | ✅ | 排除插件生成目录，稳定排序并构建内容哈希 |
| `class_name` 和脚本路径继承 | ✅ | 建图、循环检测、父类优先生成和原生头文件依赖 |
| 跨脚本字段、方法、常量和枚举 | ✅ | 建立签名表并验证静态/实例访问和参数 |
| 跨脚本构造 | ✅ | `_init` 签名进入项目符号图，生成强类型 C++ 实例 |
| 内部类与 lambda | 🟡 | 递归嵌套内部类生成独立稳定原生类型，按继承拓扑先于宿主注册，限定身份与缓存 ABI 一致；lambda 生成带参数/宿主生命周期检查的 `CallableCustom`；递归 lambda 和高级闭包仍待差分 |
| 未命名脚本原生身份 | ✅ | 以规范化完整资源路径和稳定摘要命名；不同目录同名脚本不会发生 C++ 类名或头文件碰撞 |
| 重复全局类保护 | ✅ | 重复 `class_name` 在提交生成产物前以项目诊断拒绝 |
| 显式父方法调用 | ✅ | 符号图记录直接父类，`super.method(...)` 只分派到已解析父实现 |
| 场景节点脚本成员 | 🟡 | 保留 API 表校验后，对引擎静态类型上未知的场景脚本属性/方法走受检动态分派 |
| Autoload 名字解析 | ✅ | 解析 `project.godot` 的 `[autoload]`；`.tscn` 解析根节点脚本；原生构造在字段初始化前登记 `ObjectID`，正常走 SceneTree、启动窗口回退注册表 |
| 第三方引擎单例 | 🟡 | GodotSteam `Steam` 等不在官方 API JSON 中的 PascalCase GDExtension 单例通过 `Engine` 运行时表查找并走动态调用；发布前仍需显式依赖清单和拼写诊断 |
| 第三方 GDExtension 类型 | 🟡 | 原生库由 Godot 原样加载，addon GDScript 与其他脚本一样 AOT；ClassDB/离线清单提供类型、成员、枚举和精确 MethodBind 哈希；`extends` 使用 ScriptLanguage/Script/ScriptInstance 附着式 AOT，已覆盖独立 Node/Resource provider、多级脚本继承、外部 `super`、回调、信号、RPC、序列化和无源码导出。编辑器/发行能力域拆分、更多供应商兼容矩阵与全平台实机认证仍未完成 |
| Autoload 缓存失效 | ✅ | Autoload 别名进入符号图哈希，配置变化不会复用错误代码 |
| 增量生成正确性 | ✅ | manifest v3 为每个脚本分别保存实现哈希、公开 ABI 哈希和依赖边；编译器代码生成契约或 schema 不匹配时安全全量重建 |
| 精确增量失效 | ✅ | 方法体变化只重建自身；公开签名、Inspector 元数据或常量契约变化只传播到实际引用方；第三方桥接只失效引用对应供应商的脚本，C++ 对象再按真实 include/depfile 失效 |
| 原生对象缓存身份 | ✅ | API、平台、架构、profile、构建策略修订和编译器绝对路径共同校验，配置变化强制重编译 |
| 跨平台 UTF-8 路径 | ✅ | 项目资源/脚本、Autoload、场景、CMake、原生命令和桥接锁显式转换；Windows 中文目录与 `.import` UID 冷热生成回归通过 |
| 事务式编译 | ✅ | 语义或生成失败时不提交新的 manifest 和不完整输出 |
| 生成原生工程 | ✅ | 单一根 CMake 生成逻辑，项目产物落入插件目录约定位置 |
| 原生 ABI 校验 | 🟡 | 构造器、引擎方法、属性访问器与工具函数在重载选择后保留 `real_t`/定宽整数 meta 并生成显式 ABI 转换；仍缺全部 API 与平台运行矩阵 |
| 场景/资源脚本替换 | 🟡 | Dungeon Rush 与 Castle Defense 已覆盖继承/嵌套场景、持久连接，以及序列化 Resource/容器/循环/共享/内嵌 PackedScene 图；运行期动态图和全平台仍未认证 |
| Godot 内运行差分 | 🟡 | 固定 4.5.2 GDScript/AOT oracle 已在 macOS、Linux、Windows 覆盖值、边界整数、嵌套写回、lambda、match、字符串、信号及 13 类性能路径；Dungeon Rush/Castle 另有 AOT 黑盒门禁，完整语言、引擎版本和移动端仍未覆盖 |
| 无源码发布 | 🟡 | 示例项目三桌面 PCK/完整发行物、Android arm64 APK、Dungeon Rush Windows 和 Castle Defense macOS 已通过源码、compiler、SDK、生成物零泄漏审计；iOS、Web、AAB 和其他架构尚未覆盖 |

Autoload 当前生成了编译期强类型访问和运行时查找能力，并在实例字段初始化前登记只含
`ObjectID` 的启动映射，解决前置 Autoload 尚未完全挂入 SceneTree 时的构造期读取。导出器只有在目标库构建、原生类加载、
场景/资源替换和运行时描述符准备全部成功后才剥离 `.gd`；任一步失败都会保留脚本，商业预设
默认阻断。只有显式允许 source fallback 或关闭剥离时才交付普通 GDScript 包。Dungeon Rush
已验证成功、属性缺失失败和陈旧导出缓存三条路径，Castle Defense 进一步验证了多 Autoload
启动顺序与嵌套资源图；但其他项目仍必须按同样门禁验收，不能只以
“原生库链接成功”推断源码可以安全删除。

## 官方复杂项目门禁

官方语料由 [test/compatibility/official_projects.json](../test/compatibility/official_projects.json)
固定版本，由 [tools/fetch_compatibility_corpus.py](../tools/fetch_compatibility_corpus.py) 获取，且只
允许放在根目录 `build/` 下。执行器
[tools/run_compatibility_corpus.py](../tools/run_compatibility_corpus.py) 分三层验证：

1. 每个 `.gd` 脱离项目单独编译，用于发现纯语言和诊断回退；
2. 整个 Godot 项目编译，用于验证项目符号、继承、Autoload 和资源路径；
3. 只要项目编译成功，就强制配置并编译生成的 CMake 工程，原生编译或链接失败均为 CI 硬失败。

当前基线已经写入 manifest，任何项目的单文件成功数低于 5、9、5、3，或四个项目中任一项目的
全项目转译、原生配置、C++17 编译或动态库链接失败，CI 都会报兼容性回归。rhythm game 已通过
内部类、lambda、跨脚本枚举、Inspector category 和 RefCounted 脚本工厂路径，并升级为原生硬
门禁。成功率只能向上调整，不能用降低阈值掩盖回归。

| 官方项目 | 单文件编译 | 全项目转译 | 原生动态库 | 当前硬门禁 |
|---|---:|---|---|---|
| 3D platformer | 5 / 8 | ✅ | ✅ | ✅ |
| 3D voxel | 9 / 12 | ✅ | ✅ | ✅ |
| rhythm game | 5 / 12 | ✅ | ✅ | ✅ |
| role playing game | 3 / 14 | ✅ | ✅ | ✅ |

本地复现：

```bash
cmake --preset dev -DGDPP_ENABLE_OFFICIAL_CORPUS_TESTS=ON
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

报告生成在 `build/dev/compatibility/results/report.json`。官方源码、生成 C++、CMake 缓存、对象和
动态库均属于构建产物，不进入 Git。

## 低版本运行最新版语法

GDPP 自己解析源码，前端始终跟进当前声明支持的最新版稳定 GDScript。纯语法糖可以在编译期
降低后运行于 Godot 4.4；Godot 4.4 不需要认识原始最新版语法。但引擎 API 和运行时行为不能靠
parser 伪造。

| 最新版语言能力 | 4.4 目标策略 | 结论 |
|---|---|---|
| 三元、符号逻辑、幂、类型化声明等纯语法 | 生成 C++17 或 4.4 已有 Variant 操作 | ✅ 可行 |
| `range`、`len` 等可等价模拟行为 | GDPP runtime shim | 🟡 已实现主路径，待差分 |
| 4.5～4.7 新增引擎类、方法或枚举 | 4.4 能力表在编译期拒绝 | ✅ 安全失败 |
| 可以基于 4.4 基础 API 实现的未来标准库能力 | 明确版本化 shim | 🟡 逐项实现 |
| 无法等价降低的最新版行为 | 稳定诊断并拒绝 | ✅ 策略明确 |
| 原始高版本 `.gd` 仍进入发布包 | 4.4 会调用自己的 parser | ⛔ 不可接受 |

- [x] 单一最新版语法前端和目标 SDK/API 表是两个独立维度，不为旧 Godot 目标保留多套语法表。
- [x] 目标版本在语义分析、代码生成、godot-cpp 配置和扩展描述符中保持一致。
- [ ] 建立“当前最新版语法 → 4.4 原生库 → Godot 4.4 实际运行”的独立 CI 套件；最新版基线
  更新时同一套门禁直接前移，不新增旧语法模式。
- [ ] 为每个 shim 记录 runtime ABI、最低 Godot 版本、差分用例和性能预算。

## 语言核心架构状态

| 架构能力 | 当前状态 | 结论 |
|---|---|---|
| Lexer → Parser → AST → Semantic → Typed IR → C++ 分层 | ✅ | 核心不依赖 Godot，阶段失败不会提交半成品 |
| 单一最新版官方语法策略 | ✅ | 不维护 4.4～4.7 多套 parser；Godot 版本只约束目标 API/SDK/ABI |
| GDPP 扩展登记模型 | 🟡 | `LanguageFeatureRegistry` 已集中登记最新版注解的目标、参数范围和行为类别；增强特性的成熟度、lowering、shim 与 runtime ABI 尚待扩充 |
| 强类型 AST 节点与统一 visitor | ✅ | 表达式、语句和 match pattern 已由 `std::variant` 强类型载荷存储；旧 kind/operand API 只作为只读迁移适配器 |
| 分层 HIR/MIR 与显式 CFG | ✅ | 方法、访问器、内部类和 lambda 均生成基本块、前驱、终止指令、挂起边和副作用分类，并在后端前验证 |
| 安全 CFG 优化 | ✅ | 分支角色显式区分普通条件、迭代协议、match 模式/守卫和断言；常量布尔边折叠后删除不可达块、密集重编号并重建前驱，优化关闭路径和 Godot 运行差分均有门禁 |
| 统一整数数值契约 | ✅ | header-only `numeric` 模块是前端、HIR、runtime 与生成代码的唯一 64 位整数语义来源，并作为带 SHA-256 的 SDK schema 8 / runtime ABI 8 输入分发 |
| 迭代策略契约 | ✅ | 语义阶段生成 `IterationPlan`，HIR verifier 拒绝 range/容器策略漂移；后端不再仅从最终类型猜测循环实现，Unicode、容器增长、Dictionary key/value 与默认局部值均有 GDScript/AOT 差分 |
| `await` 表达式 ANF | ✅ | AST 只保留一种 `AwaitExpression`；HIR 在 MIR 构建前按左到右求值顺序拆出挂起点和稳定临时值，有效模块不允许未降低的 await 表达式进入 C++ 后端 |
| 长协程有界 lowering | ✅ | 安全子集的长 `await` 链由 MIR 直接生成扁平状态机；复杂跨挂起局部变量仍由结构化路径处理，不误报为一般协程帧完成 |
| 标准库 intrinsic 注册表 | ✅ | `IntrinsicRegistry` 集中登记资源/范围函数及 `convert`、`type_exists`、`char`、`ord`、`Color8`、`is_instance_of` 的逐参数规则、返回类型、常量属性与 lowering 身份 |
| 语义与后端模块化 | 🟡 | 阶段边界清晰，但 semantic/codegen 实现文件已过大，尚未按职责拆分 |
| 精确脚本/对象依赖图 | ✅ | v3 manifest 保存实际脚本/桥接引用边，公开 ABI 与实现哈希分离；NativeBuilder 使用真实头文件依赖，不把全项目符号图作为每个脚本共同输入 |

这些状态区分“核心架构已经进入主链”和“高级优化器已经完成”。强类型 AST、显式 CFG/MIR 和
精确增量已经落地；SSA/所有权数据流、semantic/codegen 职责拆分及更广的运行矩阵仍未完成。
新增语法必须继续走统一注册、语义、HIR/MIR、验证和后端链路，不能向大型 switch 追加项目
专用旁路。

## 性能与正确性门禁

当前生成器已经优先生成直接 C++，动态值才进入集中式 Variant runtime。固定 Godot 4.5.2
矩阵已形成可重复基准，但范围仍不足以支持“任意游戏都超高性能”的宣传。

- [x] 静态引擎方法、属性、项目字段和项目方法优先生成直接 C++ 调用。
- [x] 动态运算、方法、属性、下标和迭代集中在 `gdpp::runtime`，便于统一优化和审计。
- [x] 调用参数先按源码顺序保存，避免依赖 C++17 参数求值顺序。
- [x] 整数常量、优化、静态/动态执行和复合赋值共用无 UB 的 64 位位模式契约；生成 C++ 已通过
  `-fsanitize=undefined` 全套核心测试及 Godot 4.7.1 端到端差分。
- [x] 顶层 Signal 等待不轮询、不阻塞线程；生成一次性 Callable 续体，并以 `ObjectID` 检查宿主生命周期。
- [x] 实例协程返回值使用每次调用独立的完成 Signal；立即结果不调度，挂起结果、`void`/`null`、并发隔离、GDScript/AOT 双向等待、信号回收和 ABI 变化缓存失效均有自动化门禁。
- [x] lambda 生成 `CallableCustom`，在调用边界检查参数数量，并以 `ObjectID` 阻止已释放宿主回调。
- [x] 项目使用内容哈希、实际引用图、自动代码生成契约指纹和目标 API 进行增量失效。
- [x] 区分脚本实现哈希、公开 ABI ID 和项目内容 Build ID，方法体修改不再重命名原生类。
- [x] 使用脚本引用图和真实 C++ include/depfile 精确失效，取消每个对象对全部生成头文件的依赖。
- [x] 建立 13 类固定 GDScript/AOT 工作负载，以 AB/BA 交替执行比较行为、启动、峰值 RSS、
  当前每平台 360 个计时帧和完整发行目录；Godot 版本/CPU/样本/分位数进入 JSON 报告，CI
  强制执行预算。
- [x] 强类型数值、分支、Array/PackedArray、向量、对象属性、字符串、方法、Callable、Variant、
  信号和分配工作负载在三平台本轮均快于 GDScript；Linux `Dictionary` 约慢 4%，保留为已知
  热路径。首轮 Windows 信号慢 43.81% 已通过直接发射与 `StringName` 缓存修复，而非放宽预算。
- [ ] 持久化 AST/符号摘要，缓存命中不重新解析，缓存未命中不重复解析同一文件。
- [ ] 为 lexer、parser、semantic、IR、codegen、原生构建和 runtime 分别建立基准。
- [ ] 静态数值热路径必须证明无 Variant 装箱、堆分配和字符串方法查找。
- [ ] 在现有 GDScript/GDPP AOT 双方 oracle 上增加手写 godot-cpp 第三方基线。
- [x] 建立 Linux UBSan 核心门禁，未定义行为立即失败。
- [ ] 建立 ASan、TSan、长时间运行、场景重复加载和多线程构建门禁。

建议的 Stable 预算在基准设施完成后启用，当前不代表已达到：

| 指标 | Stable 目标 |
|---|---:|
| 5,000 行文件前端验证 P95 | ≤ 100 ms |
| 100,000 行项目无变更检查 P95 | ≤ 500 ms |
| 100,000 行冷前端分析与 C++ 生成 P95 | ≤ 3 s |
| 静态 CPU 基准相对手写 godot-cpp | 几何平均不慢于 1.25 倍 |
| 引擎调用密集基准相对手写 godot-cpp | 额外开销 ≤ 10% |
| 已发布基准的单版本回退 | ≤ 5% |

## `.gdpp` 编辑器语言

Tree-sitter 不是编译或运行 `.gdpp` 的必要条件。GDPP 已有 lexer、parser、AST、语义和项目符号
系统，编辑器支持应复用这套前端，避免高亮器与实际编译器维护两套语法。

| 编辑器能力 | 状态 | 计划接口 |
|---|---|---|
| 独立 `.gdpp` 注册 | ⛔ | `ScriptLanguageExtension` |
| 脚本资源与实例 | ⛔ | `ScriptExtension` |
| loader/saver | ⛔ | 自定义 ResourceFormatLoader/Saver |
| 实时诊断 | ⛔ | 复用 compiler parser/semantic |
| 补全、定义和文档 | ⛔ | 项目符号图 + 目标版本 API 表 |
| 外部 IDE | ⛔ | LSP；Tree-sitter 仅作为可选编辑器语法包 |

- [x] 安装插件不会让 Godot 4.4 内置 GDScript parser 自动理解未来语法。
- [x] 仅引入 Tree-sitter 不能解决脚本资源、补全、运行、导出和 AOT 类映射。
- [ ] 先完成 `.gd` 兼容模式的运行差分，再让 `.gdpp` 承载明确登记、分级发布的增强语法；
  GDPP 扩展可以独立演进，但不复制多套官方语法版本。

## 下一阶段阻断项

- [ ] P0：把已运行的 Godot 4.5.2 固定 oracle 扩展到 4.4～4.7、三桌面平台、错误顺序、完整
  场景生命周期和协程，不为缺失目标复用其他版本结果。
- [ ] P0：完成场景、Resource、Autoload 与导出配置的原生类替换，成功后才允许剥离源码。
- [x] 已完成：官方 API 构造器、普通方法、属性访问器和工具函数的参数 meta 进入重载结果并生成
  `real_t`、float/double 与定宽整数显式 ABI 转换，严格 C++ 编译不依赖隐式窄化。
- [ ] P0：从 API meta 生成精确的虚函数 C++ ABI，包括 int32/uint64、enum/bitfield、const、引用和
  返回类型 thunk。
- [ ] P0：为 114 个工具函数、38 个内建类型、749 个运算符生成版本化原生编译与运行矩阵。
- [x] 已完成：旧式访问器绑定、显式父方法调用、同名未命名脚本隔离和安全对象降型，RPG 已进入项目级原生硬门禁。
- [x] 已完成：实现非构造 `void` 实例方法的 Signal await 续体、一次性连接、对象释放取消语义，
  并对安全长链生成有界 MIR 状态机，消除深层嵌套 lambda 的 MSVC 编译内存爆炸。
- [x] 已完成：将 `await` 迁移为强类型表达式，并通过 HIR ANF 支持变量/赋值、算术、容器、
  调用参数和普通条件主体中的有序挂起；Godot 4.7 原生 oracle 覆盖多次恢复和跨挂起局部值。
- [x] 已完成：实现内部类与 lambda 主路径，官方 rhythm game 12 / 12 项目转译并进入原生动态库硬门禁。
- [x] 已完成：项目脚本静态方法 ClassDB 注册，以及跨脚本和继承匿名枚举值的独立符号解析与
  C++ 生成；二者均有项目级严格原生编译回归。
- [x] P1：完成 `await` 短路/三元分支、循环条件、循环携带局部与函数/setter 入口参数共享单元、
  挂起后 `break`/`continue` 与无引用环生命周期的控制流 lowering。
- [x] P1：完成异步 match 守卫和分支正文 lowering；模式绑定发生在守卫前，守卫失败继续后续
  分支，正文恢复进入 match 外层续体；单一弱回指索引调度器保证分支和外层续体只生成一次，
  并以 HIR/MIR/10 分支线性规模/严格 C++/Godot 运行差分共同验证。
- [ ] P1：以稳定 SymbolID 和完整活跃变量分析替换保守的名称级循环共享单元，并把当前已支持的
  实例方法返回协程协议扩展到完整 lambda、异步引擎虚函数外部调用和统一帧；debug-only assert
  条件与惰性消息已经接入同一恢复模型。
- [x] P1：实现十种 PackedArray 的静态可迭代检查、元素类型推断和代码生成。
- [x] P1：实现同步 PackedArray 和 `range()` 专用零装箱/零临时 Array 循环。
- [x] P1：为同步 String、Array、Dictionary 建立显式迭代策略；Unicode 字符、实时容器增长、
  Dictionary 键/值类型和默认局部初始化通过 Godot 4.7 GDS oracle 与 Godot 4.5.2
  GDScript/AOT 差分。
- [x] P1：实现同步 float、Vector2/Vector2i、Vector3/Vector3i 原生范围循环，以及项目脚本、
  内部类、第三方运行契约和引擎已知对象的 `_iter_*` 协议解析；无协议对象和非法签名在语义阶段
  失败关闭。
- [ ] P1：实现异步续体中的 PackedArray/静态容器/数学范围专用循环；当前异步路径使用 Godot
  Variant 迭代协议保持行为正确，但尚未获得同步路径的零装箱性能。
- [x] P1：建立固定运行性能、行为 oracle、日志黑名单、包体统计和 CI 预算。
- [ ] P1：继续建立 fuzz、sanitizer、长时间压力和完整包内容白名单测试。
- [ ] P2：完成 `.gdpp` 的 ScriptLanguage/ScriptExtension、调试和 LSP。

在 C4 运行差分与 C5 无源码交付闭环完成前，对外应使用“已支持文档列明的 GDScript 子集，并可
将通过项目编译的脚本生成原生 GDExtension”这一表述；不得使用“完整兼容任意 GDScript”、
“绝对无法逆向”或没有公开基准支撑的“超高性能”。
