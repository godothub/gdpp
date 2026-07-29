## 1.8.3

- 将宿主专用压缩包收敛为面向 Godot 4.6～4.7 的 `gdpp.zip` 和面向 4.4～4.7 的 `gdpp-all.zip`；两者均包含三种桌面编辑器插件及全部支持的导出 SDK。
- 增加 Konado、Pixelorama、Godot Open RPG 和 Source of Mana 的完整项目兼容性测试。
- 统一解析跨脚本类、继承、枚举、常量、静态调用、方法覆盖和名称冲突中的类型身份。
- 完善脚本成员、Inspector 元数据、自定义访问器和 Attached 实例的反射与恢复。
- 保留绑定、内部和静态 Callable 的 vararg、默认参数、类型化返回值及 `assert` 行为。
- 对强类型数组、字典、枚举值、PackedArray 返回、嵌套写入和动态 `len` 匹配 GDScript 容器语义。
- 对数值、枚举、Variant、Object 和内建值表达式匹配 Godot 运算及转换语义。
- 保留异步初始化、`await` 中的 `super`、协程 ABI 和 Attached 生命周期，并在终止时取消等待。
- 未显式声明基类的脚本默认继承 `RefCounted`，与 GDScript 保持一致。
- 在路径、UID、`load`/`preload`、缓存、附加、继承、回滚和终止过程中保持已编译 Script 的唯一身份。
- 导出时保留外部脚本、场景、嵌套资源 owner、序列化引用、预设、诊断信息和客户源码。
- 扩展终止时安全释放生成的静态状态、已编译脚本、异步任务和 Script 缓存。
- 在生成代码和运行时中显式表达引擎生命周期静态引用，确保 GCC 严格告警构建通过。
- 让 Script 与 Callable 的编译期分支保持结构互斥，确保 MSVC 严格告警构建通过。
- 所有生成翻译单元统一使用同一套原生类型、存储和 Attached ABI。

## 1.8.2

- 支持使用上下文关键字作为迭代变量，并支持枚举声明与左花括号分行书写。
- 在类型注解、构造、成员访问和类型判断中统一解析全局命名脚本、内部类及嵌套枚举。
- 将脚本命名枚举保留为具有声明身份和标准 Dictionary 行为的只读字典值。
- 为导出的 `Variant` 属性保留正确的 Inspector、存储和运行时元数据。
- 将已编译 Script 资源保留为规范且有状态的对象，完整覆盖 `load`/`preload`、可空性、Script API、属性、信号、对象传递、类型判断和构造。
- 事务回滚和扩展终止时，所有生成的编译 Script 常量缓存都会切换到不触发物化的缺失状态，避免预加载清理重新取得 Script 资源或让 Godot 对象跨过类注销阶段继续存活。
- Attached 与非 Attached Script 的物化分支改为结构互斥，在保持 Clang、GCC 行为一致的同时，消除严格 MSVC warning-as-error 构建中的不可达尾部分支。
- 对没有公开 godot-cpp 访问器的 Godot 属性使用 Object 属性 ABI，并保持字段及静态初始化器中调用接收者和参数的求值顺序。

## 1.8.1

- MIR operation identity 现在只在控制流图完成不可达块裁剪与前驱重建后，按“块、指令、终结器”的规范顺序统一分配；嵌套 `if/else` 的所有分支都提前 `return`、`break` 或 `continue` 时，不再因已删除的临时汇合块留下编号空洞并误报 `GDS5118`。
- lowerer 与 optimizer 共用唯一的 operation ID 规范化实现，所有会改变 CFG 的路径都恢复同一确定布局；verifier 同时拒绝缺失、重复、越界、乱序和非密集身份，避免损坏输入中的超大 ID 驱动无界内存分配。

## 1.8.0

- 生成的客户 GDExtension C 入口 ABI 统一缩短为 `gdpp_library_init`，生成注册代码、运行时描述符、Apple 静态入口登记及全部受支持原生目标均强制使用这一唯一符号。
- 每次项目编译成功时自动清理已退役的 `gdpp_project.gdextension` 和 CMake 项目脚手架，原地升级不会在当前直接构建 manifest 旁残留旧入口符号或失效构建脚本。
- 支持真正挂起的静态函数，通过无实例 FunctionState 保留类型化结果和 Signal 恢复；类型化 lambda 协程也拥有逐次调用独立的挂起状态和可逆序完成的并发恢复。
- 真正挂起的协程统一返回引用计数 FunctionState 而非裸 Signal，提供 `completed(result)`、`resume(arg)`、`is_valid(extended_check)`、零/一/多 Signal 参数折叠、旧连接清理和宿主销毁防护；生成的 `await` 同时识别 GDPP 与原生 GDScript 状态对象。
- 以稳定局部符号身份匹配 GDScript lambda 捕获：Callable 创建时取得值快照，Array/Dictionary/Object 保持共享身份，每次调用建立独立可写标量帧，并在词法遮蔽下完整保留嵌套、返回、共享容器递归、异步循环和调试器可见捕获。
- 所有受支持 godot-cpp 版本的 Callable 创建快照统一通过可变类型擦除桥接实例化，避免 GCC 在 Godot 4.5/4.6 构建中把逐次调用的可写标量及共享容器帧错误限定为只读。
- 引入脚本 fault frame：致命 GDScript 操作只终止当前生成函数，调用方可继续执行；同时保留源码求值顺序、惰性分支、专用调用参数顺序、Callable 默认返回，以及 Variant、容器、对象和第三方扩展边界的精确 `.gd` 路径、行列诊断。
- 同步 fault frame 改为调用局部、线程局部状态，协程持久 fault 状态由 FunctionState 恢复互斥锁串行拥有；活动帧检查在生成调用点内联，失败标记不会跨线程、协程、嵌套调用或 GDExtension ABI 泄漏。
- 协程、协程 lambda 和恢复 continuation 的 fault 检查器统一保持无捕获，并从当前活动线程已安装的状态解析；逃逸异步闭包不会再保留同步 `ScriptFunctionScope&`，GCC、Clang 与 MSVC 运行时统一使用同一生命周期契约。
- 对每个类型化表达式、转换和赋值判定其是否可能设置活动脚本故障，只在可证明可能失败的边界生成轮询。字面量、局部值流、安全整数运算、精确存储、String 值方法和三元分支不再堆叠冗余分支；动态调用/运算、严格 Variant 存储、越界、除法/取模、Object 访问、Callable/Signal 重入及协程边界仍保留有序检查。
- `Dictionary[key]` 与 `Dictionary.key` 读取分别进入 Godot keyed/named Variant ABI 的有效性结果，以一次查找区分缺失/强类型非法键和已存储的 `null`；点号读取保留强类型 Dictionary 的声明值类型，当 `StringName` 不能进入声明键存储时在前端拒绝，并保持接收者优先的故障顺序。
- 对缺失键、强类型键/值及只读存储完整匹配 Dictionary 直接和复合赋值故障；点号写入使用原生 `StringName` ABI，非法直接写入终止当前函数，而强类型复合值被拒绝时保持原条目并像官方 GDScript 一样继续。
- 新增基于符号身份的全函数证明，只对局部、非强类型 Dictionary 字面量中已存在的槽位借用读取并执行原地复合更新；已证明读取保留 `const Variant&`，不再重复 named 有效性检查与复制，重赋值、别名、调用、下标、未知键、强类型存储、方法访问和闭包捕获都会撤销证明并保留完整受检路径。
- 对现有 `Variant::INT` 槽位直接执行受支持的精确整数复合运算，不再构造并赋回结果 Variant；原生整数和动态 Variant 右值共用同一套可移植 64 位溢出、移位、除法与取模合同，不支持或非整数运算仍进入 Godot 通用 Variant 路径，所有失败均携带原始 `.gd` 源码位置。
- 已经是 `Variant` 的值跨越 native-to-Variant 边界时直接借用，不再构造相同临时值；编译器拥有且只使用一次的赋值 RHS 快照通过 godot-cpp 移动构造/赋值提交。接收者先于 RHS、故障检查、自赋值、共享容器身份及受检转换保持不变，String 与动态标量循环不再承担重复引用计数复制。
- 在不放宽阈值的前提下，将 fault-safe 后端恢复到相对 GDScript 最大回退 10% 的商业性能合同。
- 将本地 lambda 的参数数量和变参身份编码进生成 C++ 类型，并仅在具体 Callable 仍处于创建调用栈内时保留原生参数 tuple；精确参数不再往返 Variant，赋值/逃逸、失效宿主、缺省参数、结构化容器和动态路径仍执行完整 Godot Callable 与严格 Variant 校验。
- 精确本地 Callable 的原生参数分支与严格 Variant 回退改为结构互斥，在保留快速路径的同时通过 MSVC 的 warning-as-error 不可达代码分析，并继续兼容 Clang 与 GCC。
- 精确同类型的显式 Variant 转换先于 Godot 通用转换矩阵执行快速路径。
- 动态标量、Object/Ref、Array/Dictionary、PackedArray 和 Attached 属性写入统一执行 Godot 严格运行时存储转换，不再接受 godot-cpp 更宽松的 cast，也不会把非法值静默变成默认值。
- 静态/preload 初始化以及 Attached 字段、`_init`、`@onready` 阶段分别采用事务和故障隔离；失败的部分状态不会发布，后续对象构造与调用方仍能按 GDScript 默认值语义继续。
- 异步引擎虚函数严格匹配 Godot ABI：挂起回调立即把 FunctionState 交给引擎执行 Variant→native 转换；同步类型化结果、完成、手动恢复、旧连接清理、有效性和 continuation 均保留严格语义。
- `@static_unload` 意图从 AST 贯通 HIR、生成 metadata、项目 manifest、缓存身份和编辑器 Script 描述，并匹配 Godot 4.4～4.7 脚本静态状态保持到语言/扩展关闭的可观察行为。
- 完整实现 GDScript `breakpoint` 语句，从词法、语法、语义模型、HIR、类型化 MIR、verifier、C++17 生成一直贯通到 Godot 原生调试器桥接，合法的生产脚本不再因该语句而在 AOT 前端失败关闭。
- 通过 `ScriptLanguageExtension` 向调试器报告生成代码的原始 `.gd` 路径、函数、当前源码行、精确处理遮蔽关系的词法局部变量，以及当前脚本与继承脚本成员；调试表达式使用 Attached 脚本相同的帧宿主求值。
- 普通方法、访问器、静态函数、lambda、附着到第三方 GDExtension 基类的脚本和协程恢复点统一保留调试行为；Release 产物完全移除断点插桩，没有连接 Godot 调试器时也不创建无效调试帧。
- Attached 原生实例返回 Godot 公开的规范 `ScriptInstance` 句柄，使调试器、引擎回调、属性访问和生命周期追踪始终指向同一个引擎对象，不再泄漏内部实现指针。
- 非平凡的线程调试状态改为首次进入 Godot 回调时按需构造，不再把 Godot 值放入 Windows DLL 的静态 TLS 初始化表，避免在 GDExtension 入口执行前触发 Windows 装载错误 1114。
- 在 Clang、GCC 和 MSVC 严格 warning-as-error 构建下保留 GDScript 合法的词法遮蔽，同时不关闭其他原生编译诊断。
- macOS Universal Debug 导出可以复用经过验证、仅提供 Release 的第三方 provider fat binary；GDPP 只在导出包内的描述符字节中增加 Debug 别名，不修改客户源码目录中的描述符或动态库。
- 前端新增有界多错误恢复；未闭合分组或尾随空白之后缺失语法节点时，仍保留完整、单调的源码范围，包括单行 lambda 内的 cast 等嵌套路径。
- 所有单文件与项目编译入口统一运行在 GDPP 自有的 16 MiB 工作线程栈上，拆分占用最大的调用/成员语义分析帧，并保留可重入编译，不再继承 Godot 最小约 512 KiB 的脚本工作线程栈。
- MIR 的 value/operation 使用稳定且不含地址的身份，支持版本化控制流快照序列化、source ownership 与密集前驱校验；优化预算和失败均保持事务，不会破坏输入。
- 在完整重映射 operand 的前提下删除无效 MIR value，同时保留调试器、分配、故障和挂起等可观察操作；CSE、内联、逃逸与循环变换在取得独立等价证明前不会默认启用。
- 对 Godot 4.4～4.7 API 表统一执行元数据、参数范围、属性读写 ABI、Signal 契约、enum/bitfield、标量返回和原生指针边界校验。
- 无源码成品为完全由运行时拼接的脚本路径保留规范 Script 身份，覆盖相对路径、UID、同步/线程 ResourceLoader、缓存相等、`exists()`、`get_script()` 和 `.new()`；编译项目清单外路径确定失败。
- 真实 ENet 重连与退出统一遵循确定的节点/缓存、peer、SceneTree 根所有权顺序：复制节点先离树，再替换或关闭 peer，清空 API 的 peer 引用，最后注销并释放自定义 multiplayer 根；成功与中止清理均可重复执行，消除 Godot 4.4～4.7 的重复 `tree_exited` 缓存诊断。
- await 赋值先以 A-normal form 固定接收者与下标，再求右值；局部、字段、属性和动态下标在挂起前后保持单次求值、写回所有权、惰性分支和生命周期。
- 缺失的默认参数在有序调用 prologue 内求值，并允许默认表达式真正挂起；接收者、此前显式/默认参数、vararg 和逐次调用状态均跨恢复保留。
- 内联及方法绑定属性 getter/setter 在实例、静态、内部类、跨脚本、继承和并发调用中统一保留协程 ABI，并对项目缓存执行传递式依赖失效。
- 静态协程的无 owner 状态与嵌套协程 lambda continuation 完全隔离于外层 emitter 上下文，避免捕获实例、丢失类型化返回或在内部失败路径生成非法 C++ return。
- 注解常量参数中的 `await` 在语义阶段稳定拒绝，不让不可能的编译期挂起进入 HIR 或 C++ 生成。
- 项目脚本 `Object.free()` 改经 Godot Variant 调度而非直接 `memdelete`，保留普通 Object 销毁、RefCounted 拒绝、锁定对象保护、已释放身份、源码位置和当前函数失败语义。
- 空对象及已释放对象身份统一通过 godot-cpp 精确的 `Variant::operator ObjectID()` ABI 读取，消除 MSVC 的有符号/无符号歧义转换，同时不放宽生命周期校验。
- custom/double precision 插件由目标引擎导出的精确 `extension_api.json` 生产，compiler、editor/fallback、godot-cpp、SDK、描述符 feature、API SHA-256、precision 与 runtime ABI 全部绑定。
- SDK 升级到 schema 12、发行 runtime ABI 升级到 18；缺少完整调试器、FunctionState、动态 Script、严格 Variant、协程访问器、await 默认参数、自定义精度或 Object 生命周期契约的旧 SDK 会在创建任何客户编译命令前失败关闭。

## 1.7.10

- 附着式脚本现严格匹配 GDScript 独立的 implicit-ready 生命周期：在正常 `_ready` 分派前按基类到派生类顺序初始化 `@onready` 字段，覆盖未声明 `_ready`、继承回调、内部类以及 `request_ready()` 再次触发的场景。
- implicit-ready 初始化与用户方法反射彻底分离，所有生命周期钩子均使用编译器保留的 `_gdpp_` 命名空间，客户方法不会再与运行时初始化冲突；派生脚本仅声明 `@onready` 字段时也不再伪造 `_ready`，因此不会遮蔽基类真实的 `_ready` 回调。
- onready-only 附着式类不再生成伪 `_ready` 的声明或定义，避免客户项目最终链接阶段出现未解析虚函数符号。
- 强类型 `Array` 与全部 `PackedArray` 的下标读取、写入及复合赋值统一经过支持负下标的边界检查，同时保持先求接收者、再求下标的执行顺序；异常消息数据现在产生包含原始 `.gd` 路径、行、列和读写动作的受控诊断，不再直接解引用原生存储并终止进程。
- 强类型对象属性写入、脚本 setter 与内建分量回写统一检查空对象及已释放接收者；消息驱动的 UI 更新遇到无效原生对象时会产生带源码位置的 Godot 错误并停止当前路径，不再直接解引用导致进程崩溃。属性复合赋值严格按源码顺序各求值一次接收者、当前值与右值，消除礼物驱动状态变化中的副作用重复执行及释放后使用窗口。
- 所有动态 `Variant` 方法调用、属性/键读写与迭代步骤都会先拒绝空对象或已释放对象，畸形协议对象不再携带无效原生指针进入 Godot 底层 Variant 分派。
- 强类型 `Array` 与全部 `PackedArray` 下标读取在完成边界校验后直接返回原生元素表示，消除逐次读取的 `Variant` 往返，同时保留附着对象转换、共享容器身份、负下标及带源码位置的失败行为。
- Signal、Callable、Object 动态调用及工具函数可变参数统一经过单次构造适配器，由 godot-cpp 对每个必要参数只创建一次 `Variant`；共享 `PackedArray` 参数仍暴露其保留身份，不复制原生存储。
- 全部 PackedArray 元素族的普通读取与复合赋值在最终生成边界继续保持原生元素类型，不再把已完成边界检查的值误分类为 `Variant` 并二次往返。
- 本地声明信号通过缓存的信号名 `Variant` 与公开 Object MethodBind ABI 发射，在保留 Godot 连接语义和实参源码求值顺序的同时，不再为每个事件重复构造固定调用元数据。
- 每个附着脚本反射方法同时保存对应的受检生成入口；ScriptInstance 完成首次方法解析后直接分派引擎到脚本的信号、生命周期、Callable、动画和定时器回调，不再把隐藏 behavior 包成临时 `Variant` 并重复执行 ClassDB 查找。
- 商业性能合同收紧为：启动、稳态帧工作及每个基准族相对 GDScript 的 AOT 回退均不得超过 10%。
- 直接原生构建器改为跟踪每个生成翻译单元的传递式引号 include 图，不再让每个对象文件依赖全部生成头文件；只修改实现时仅重编自身对象，公开脚本结构变化时也只重编真实依赖者与注册单元后重新链接。
- 附着式脚本的 `self` 跨越强类型原生调用边界时保留类型正确的 `RefCounted` 强引用，覆盖嵌套消息对象和生成的协议模型，不再执行不安全的 owner 转换。
- 强类型引擎对象、`RefCounted` 值与供应商对象拥有的附着式 `self` 统一通过 Godot Variant 对象身份比较，避免协议与模型类在导出时生成非法的 `Ref<T> == Object*` C++。
- 发行运行时 ABI 升级至 13，旧 SDK 或复制不完整的 SDK 会在导出预检阶段被拒绝，不再链接生命周期或信号调用契约不兼容的生成代码。
- 在生成异步 `while` 条件或 `for` 可迭代对象之前先安装协程循环携带存储，确保条件、循环体及恢复后的 continuation 在跨越 process frame 或 Signal 挂起后始终观察同一个原生值，不再让分批后台任务停留在过期副本上。
- 在保持 C++ 警告洁净的同时完整保留源码绑定：未使用的方法、setter、lambda、剩余参数、局部变量、迭代及 match 绑定和 await 载荷均继续具备准确的 GDScript 生命周期与求值语义。

## 1.7.9

- 移除仍声明 Node.js 20 的 `ilammy/msvc-dev-cmd` 依赖，改用仓库自有、零第三方运行依赖的 Node.js 24 Action：通过 `vswhere` 定位 Visual Studio，初始化受支持的 MSVC x64 工具链，仅导出发生变化的环境变量，保留包含 `=` 的值，去重 Windows 工具路径，并对不完整或架构不匹配的工具链状态实施失败关闭。
- 工作流语义校验器升级至原生支持 Node.js 24 Action 元数据的 actionlint 1.7.12；编译器核心、原生集成和商业 Windows 宿主构建统一使用仓库自有的 Node.js 24 初始化。
- Node.js 24 的 MSVC 初始化命令改为不经通用 Windows 参数二次转义、原样交给 `cmd.exe`，确保 `Program Files` 下带引号的 Visual Studio 路径可正确执行。
- Windows、macOS、Linux、Android、iOS 与 Web 的客户运行库文件名前缀由 `gdpp_project` 统一缩短为 `gdpp`，同步覆盖 Windows 导入库、Mach-O install name、iOS 切片动态库及 XCFramework 内部布局；GDExtension 入口 ABI `gdpp_project_library_init` 保持稳定。
- 原生构建与导出转换修订号同步推进；下一次 AOT 导出前事务式清除旧命名产物（包括目录型 iOS XCFramework），无法清理时失败关闭，避免旧库残留或误入客户包。
- 桌面运行、APK、XCFramework、Wasm、PCK、插件组包及清理逻辑全部切换为新命名；商业插件包同时拒绝客户生成产物与旧命名产物，并通过精确分类保留编译器和 fallback 库。
- 三个桌面插件 ZIP 全部恢复标准 `addons/gdpp/` 根目录，用户直接解压到 Godot 项目根目录即可得到可发现的 EditorPlugin，不再误装为无效的 `res://gdpp/`；发行包清单升级至 schema 5。
- 编辑器兼容性更新至官方 Godot 4.7.1，并消除会在客户严格项目设置下变成错误的 Variant 推断警告。
- Windows 生产导出器改用 `vswhere` 查找 Visual Studio 并验证已安装 x64 C++ 工具组件，覆盖 Preview 与非默认安装路径且保留显式编译器覆盖，同时移除硬编码的用户目录兜底；无法满足条件时给出可操作的工具链诊断，不再把已安装的 MSVC 环境误判为缺失。
- 原生构建规划前将选定的 MSVC `cl.exe` 解析为 Visual Studio 工具目录中的绝对路径，并显式调用同目录 `link.exe`；客户即使从继承开发环境的 Godot 启动，链接阶段也不会再误入 Git/MSYS 的同名无关工具，直接进程入口同时保留等价的失败关闭防线。
- 在成功的 AOT 导出期间保持编辑器编译器描述符不可变，并将其标记为不可热重载以匹配实际 godot-cpp 构建契约，消除把已加载编译器扩展替换为项目运行时所导致的 GDExtension 实例重建失败。
- 让 GDPP 导出插件先于 Godot 内置 GDExtension 扫描器执行，只在包内稳定的 `addons/gdpp/gdpp.gdextension` 路径替换描述符字节，并通过公开导出 API 对生成的项目库进行且仅进行一次登记。
- 在单一登记路径下保留各平台原生打包契约：Android 携带准确 ABI 标签，Web 携带选定线程特性，桌面库保持平台落位规则，iOS 根据 Godot 4.4 或 4.5+ 注入 XCFramework 所需的静态入口回调与未定义符号链接参数。
- 完全在内存中解析 macOS Universal 2 第三方 GDExtension 库及依赖，验证胖 Mach-O 载荷，按字节原样打包 provider 描述符，不再为架构发现改写客户扩展文件。
- 将剩余扩展注册表事务限制在有意的纯源码导出或非桌面源码回退场景，因为 Godot 的强制元数据阶段必须在这些场景排除运行时描述符；正常 AOT 导出的注册表和全部扩展描述符均保持字节级不变。
- 编辑器启动时恢复 1.7.9 之前版本遗留的中断描述符事务；新导出不再创建编译器或 provider 描述符备份。
- 在不使用 C++20 `std::string::starts_with` 的前提下处理带 UTF-8 BOM 的 `vswhere` 输出，保持 Visual Studio 发现逻辑符合 C++17 合同。
- `gdpp-mac.zip` 从仅 arm64 的宿主契约升级为真正的 Universal 2 compiler、fallback 及 Godot 4.4～4.7 桌面 SDK；Apple Silicon 与 Intel 编辑器现可加载同一插件，标准官方 Universal 2 导出模板无需自定义模板或改写预设即可工作。
- 每套 compiler、fallback 及桌面 godot-cpp 归档均强制包含 arm64、x86_64 切片与 macOS 11.0 最低部署版本。
- 精确分类 Godot 4.6.2 官方 iOS 模板对已移除可选项 `application/boot_splash/fullsize` 的上游警告，不修改客户项目，也不接受其他无关警告或错误。

## 1.7.8

- 附着脚本描述符注册严格保持为纯元数据操作：脚本常量（包括嵌套资源容器和其他纯值）只登记无捕获延迟求值器，不再于 GDExtension 类注册期间执行求值。
- 仅在 Godot 请求编译后 Script 常量表时解析延迟常量，同时保留本地与继承常量反射、派生类遮蔽、确定性描述符身份、生成常量的线程安全缓存及显式卸载清理。
- 修复导出游戏在原生扩展启动期间因脚本常量预加载场景或资源而直接退出的问题；覆盖依赖尚未就绪的 2D/3D 物理、渲染、音频、导航及第三方 GDExtension 服务的资源构造。
- 对缺失的延迟求值器、立即/延迟常量重名、继承合并及重复描述符实施失败关闭校验，并确保注册表锁内不执行客户代码或引擎资源服务。
- 为生成项目注册阶段剩余的编辑器专用 Engine 查询增加空值保护，必需单例不可用时返回受控初始化错误，避免原生空指针解引用。
- 修复根脚本及内部类的运行期构造：每个生成 Script 在附着前写入已解析描述符的契约哈希；`.new()` 构造失败时现会给出可操作诊断，不再延迟退化为一连串空值属性错误。
- 附着式 ScriptInstance 字段改由描述符携带的强类型 getter/setter 回调分派，完整保留自定义访问器、Variant 转换、继承属性及强类型容器键语义，不再依赖 ClassDB 对 GDExtension behavior 对象的属性反射。
- 发行运行时 ABI 升级至 11，使旧 SDK 在预检阶段明确失败，避免按旧的立即求值描述符布局继续编译。
- Godot 属性类型改为依据 getter/setter 的真实 ABI 解析，不再错误选取 Inspector 资源候选列表中的第一个具体类型；Godot 4.4～4.7 的 Shader、粒子、天空、雾、几何体、灯光、贴花、相机属性及同类多态资源读写均保留正确基类契约。
- 无所有者的静态函数 Callable 在 Callable 生命周期内保持为有效信号目标，实例绑定 lambda 仍执行 Object 生命周期校验；由此恢复 `HTTPRequest.request_completed` 等异步信号的静态绑定回调。
- 附着式根脚本及内部类进入强类型 Array/Dictionary 时同时保留 provider 持有的真实原生基类与唯一规范 Script 资源，避免有效编译对象被误判为普通 `RefCounted` 或第三方 GDExtension 实例而遭容器拒绝。
- 强类型容器形成 C++ 身份前统一同一脚本类的短名、限定名及生成原生名，使字段、函数返回值、链式调用与局部变量始终使用同一个 ABI 稳定的 Array/Dictionary 特化。
- 规范强类型容器 Script 资源不再依赖生成描述符的登记先后顺序；跨脚本默认值可在目标描述符登记前预留精确身份，随后在同一资源上绑定契约，不产生启动错误，也不退化为无类型容器。
- 对 Godot 4.4～4.7 中所有兼容 ShaderMaterial 的 Canvas、粒子、雾、天空、几何体、CSG、Mesh 与 Tile 材质槽位统一执行多态资源属性访问器契约。
- 将 Godot 属性 getter 返回 ABI 与 setter 参数 ABI 拆分为独立的语义及 HIR 契约；赋值诊断、复合写回和 C++ 参数实体化均使用写入侧真实类型，不再假定当前引擎的读写访问器永远对称。
- macOS、Linux 与 Windows 商业宿主组件改为和编译器、Godot、Android、Web、iOS 任务并行构建；仅在全部必需产物成功后启动最终组包。
- 将桌面组件生产、发布编排及三平台组包拆分为具有明确依赖、单一发布入口的可复用工作流。
- 每个 Godot 版本的发行 SDK 改为共享一份运行时、godot-cpp 头文件树、源码树和 `lib` 目录；Android、Web 线程/无线程、iOS 与当前桌面宿主只再提供各自不同的优化 Release 库，不再重复携带公共 SDK 内容。
- 从共享 SDK 中按平台、架构、构建配置及 Web 线程模式精确选择目标清单和 godot-cpp 库，同时保留对旧组件目录的兼容，并优先使用精确 macOS 架构库而非 Universal 回退库。
- 三个桌面压缩包的顶层目录由 `addons/` 包装改为直接的 `gdpp/`。

## 1.7.7

- 原生编译进度改为直接挂载到当前导出对话框视口的最前层覆盖界面，在所有桌面主机上均保持位于 Godot 模态导出界面之前。
- 项目扫描、解析、语义分析、原生代码生成、编译与链接统一移入单个后台构建工作线程；编辑器线程仅负责窗口事件、绘制和导出协调。
- 启动工作线程前先在主线程快照第三方 GDExtension ClassDB 契约，并通过互斥保护的进度邮箱传递状态，后台构建不再触碰活动编辑器场景树或界面。
- 仅在启动 AOT 导出时编译项目中兼容的 GDScript；常规编辑、导入和编辑器内运行继续使用原始脚本，不生成或加载客户开发库。
- 每次导出只构建所选的 Debug 或 Release 单一目标，移除 editor/development 预构建、客户 CMake 工程生成、第二套运行时描述符以及旧动态库热重载链。
- 所有生成脚本统一使用附着式行为后端：原始 Godot 或第三方 GDExtension Node/Resource 始终是真实对象所有者，生成的 C++ 只提供其 ScriptInstance 行为。
- 通过 `ScriptLanguageExtension` / `ScriptExtension` 建立仅限导出阶段的元数据桥接，覆盖外部脚本及 `.tres` / `.tscn` 内嵌子资源，使场景转换和 ClassDB 校验不再依赖宿主项目库。
- 直接从编译器项目语义图生成声明本地的导出反射，包括继承脚本身份、存储属性使用情况、方法参数、信号和缓存命中；导出不再加载全部客户 `.gd`、运行静态初始化或滞留循环引用的 GDScript 资源。
- 每个仅含元数据的 ScriptInstance 只序列化从源 SceneState/Resource 明确复制的字段，未触碰字段继续由目标 C++ 行为构造器提供默认值，避免空强类型容器或 `nil` 覆盖 AOT 默认值。
- 跨脚本字段与方法、Autoload、`is` / `as`、内部类、RefCounted 对象及显式 `self` 统一经过同一套附着式脚本身份与分派契约；静态类型直接访问保持不变，不再把 C++ 类名转换成 Variant。
- ABI 兼容的附着式脚本 `self` 调用改走生成 C++ 虚继承链；跨对象调用及 ABI 变化的 override 仍保留 ScriptLanguage 动态分派。
- 继承的 ClassDB 调用继续作用于提供方持有的 Godot 对象，生成脚本分派则作用于附着 behavior，确保原生 GDExtension 方法能够反向调用客户脚本 override。
- 附着式或动态属性读取跨越 Variant 边界时保留语义值类型，包括强类型 Dictionary 和跨脚本访问器。
- 每个客户目标 SDK 只发布一套优化后的 `template_release` godot-cpp 归档，并同时用于 Debug 与 Release 导出；编译器的 editor 绑定只保留在预构建插件内部，不再分发第二套客户静态库。
- 将原有 16 个按版本单宿主包与完整包收敛为三个跨版本桌面包：`gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip`。每个包包含本宿主 compiler/fallback 和 Godot 4.4～4.7 全部桌面 Release SDK；三个包均包含 Android 与 Web Release SDK，仅 mac 包包含 iOS。
- 通过显式运行时白名单暂存宿主 compiler/fallback，使 Windows 发布包排除 MSVC `.lib`/`.exp` 导入产物。
- 每个生成的客户翻译单元仅针对所选导出目标编译一次；Debug/Release 对象缓存彼此隔离，同时共享确定性的前端结果和生成源码状态。
- `Dictionary` 接收者跨越 `Variant` 边界时完整保留 GDScript 的 `Dictionary.key` 读写语义，覆盖 JSON/HTTP 响应字典与嵌套复合赋值；异步认证和网络回调不再因响应字段被静默读成空值而中断后续流程。
- 为全部十种 `PackedArray` 建立共享存储语义，局部别名、字段、参数、返回值、默认参数、`Callable`、lambda、Signal 与动态调用均与 GDScript 一致；显式构造复制和参数重绑定仍保持独立。
- 公共脚本方法统一通过 Variant ABI 绑定，避免 godot-cpp 指针调用在 GDExtension 边界触发写时复制；下标读写、迭代、方法调用及引擎 API 转换均通过同一共享存储契约。
- 将解析后的参数契约从语义分析贯通至 HIR 与项目接口，确保同脚本、继承、嵌套类和跨脚本调用均按被调用方真实原生 ABI 实体化参数，不再依赖调用方推断的表达式类型。
- 所有生成的原生到 Variant 边界统一进入与重载无关的运行时适配器，覆盖数组、字典、动态调用、信号、Callable、match 绑定、外部 provider、工具函数和引擎可变参数。
- 原生 `PackedArray` 转换改为显式执行，固定引擎参数独立适配，消除 MSVC 重载歧义且不重新引入值复制，也不削弱反射 Variant ABI。
- 发行 SDK 升级至 schema 11 / runtime ABI 10，强制执行单绑定导出契约，并在桌面、Android、iOS 与 Web 清单中完整校验附着式及引用语义运行时。
- Windows 插件发布构建对并行 MSVC SDK 编译启用受协调的 PDB 写入，避免大型 godot-cpp 目标因编译数据库争用而随机失败；客户导出仍保持逐文件串行。
- Visual Studio 多配置构建现显式选择 Release SDK、记录实际 editor 优化配置并把插件 DLL 直接写入可安装的 `binary` 目录。
- 将 `GDPP AOT Build` 覆盖界面精简为标题与当前任务两行，并在项目源码编译期间追加实时逐文件计数。
- 进度条几何和动态任务文字改为直接提交渲染服务器，并在每次强制呈现前完成同步；Windows 无需移动窗口即可同时刷新文字与进度。
- 构建专用编译器描述符移入 Godot 不会递归扫描的 `.godot` 元数据目录，并在配置时清理旧的插件内描述符命名族，避免复用构建树时重复注册 ClassDB 类及破坏 Android 场景转换。

## 1.7.6

- 原生编译进度改用绑定当前焦点导出对话框的独占临时窗口，在所有桌面主机上均保持位于 Godot 模态导出界面之前。
- 移除列式伪连续填充与构建 profile 切换时的进度归零，整个 AOT 操作统一使用一条全程单调递增的连续进度条。
- development 与分发构建等分总进度；每个大阶段内再由扫描、解析、语义分析、脚本预编译、原生文件生成、项目编译和链接等分，支持文件回调的阶段继续按单文件细分。
- 独立显示实时文件计数，界面文案不再暴露后端转译术语，并统一使用简洁的 AOT、Debug 与 Release 构建提示。

## 1.7.5

- 修正 Windows MSVC 环境初始化：`cmd.exe /c` 原始负载可正确进入 `vcvars64.bat` 与 `cl.exe`，不再被 C 运行时参数转义破坏嵌套引号。
- 生成翻译单元的编译与链接严格改为单命令串行执行，消除导出期子进程突发，并避免大型项目中的工具链内存争用。
- 每条串行原生命令运行期间持续刷新 Godot 编辑器，并在每个翻译单元完成后推进编译进度。
- Windows、macOS 与 Linux 的隐藏工具链进程现会捕获 stdout/stderr，保留失败文件、阶段与退出码，并将有上限的原始诊断显示在 Godot 导出结果中。

## 1.7.4

- 在全部商业插件压缩包中包含原生构建进度界面，确保全新安装可在导出前正常加载编辑器插件。
- 当暂存插件或最终确定性 ZIP 缺少进度界面时让发布组装失败。
- Debug 与 Release 游戏导出统一使用优化后的 `template_release` godot-cpp 绑定，同时保留 Godot 编辑器进程所必需的独立 `editor` 绑定。
- 从主机、Android、iOS 和 Web SDK 包中移除全部 `template_debug` 归档；每个平台及线程 ABI 仅携带一套分发绑定，不再重复发布数百 MB 的调试对象。
- 通过 GDPP 自有编译宏保留 GDScript Debug 导出的 `assert()` 求值语义，使其不再依赖 godot-cpp 绑定目标和原生优化配置。
- Debug 导出的项目动态库现与 Release 导出使用相同的商业级优化、死代码裁剪和符号隐藏流程。
- 发行 SDK 升级至 schema 9，显式声明分发绑定及优化配置，并在编译前拒绝陈旧、Debug 绑定或混合安装的 SDK。
- 新增按 SDK 版本区分的完整插件包 `gdpp-4.4.zip`～`gdpp-4.7.zip`；每个包都组合 macOS arm64、Linux x86_64、Windows x86_64 三套编辑器二进制及桌面 SDK，并包含 Android arm64、iOS 真机/通用模拟器、Web threads/nothreads 导出 SDK。
- 插件可从完整包的平台隔离布局中自动选择当前桌面主机 SDK，同时继续兼容体积更小的单主机包。
- 完整包采用可复现组装，并在主机或目标缺失、插件静态文件冲突、运行时 ABI/哈希不一致、混入其他 SDK 版本、嵌套压缩包、生成产物或 Debug 绑定时失败关闭。
- 从已安装插件及全部发行压缩包中移除独立的第三方声明汇总文件。

## 1.7.3

- Windows 导出时的编译器、环境初始化和链接器子进程不再创建可见控制台窗口。
- 通过明确的直接构建 API 保留有界并行编译和分阶段顺序链接；Godot 导出路径不再携带可配置的 CMake 生成开关。
- 在 Godot 打包前显示独立的连续原生构建进度条，按真实文件进度覆盖项目扫描、GDScript 解析、语义分析、GDScript 到 C++ 转译、C++ 编译和原生链接。
- 开发版与分发版构建中的前端、编译和链接进度保持分阶段有序且单调递增，并在进入 Godot 打包进度前自动移除。

## 1.7.2

- 完成第三方 GDExtension 的无损反射，覆盖 Variant、强类型 Array/Dictionary、Signal、对象类、引擎类型化 Dictionary 契约和编码容器元数据，无需修改源码即可保留 provider API。
- 按实际生成的 C++ ABI 重构内部类 override：兼容覆盖进入原生虚槽，不兼容的 GDScript 契约使用私有实现符号，Variant 接收者继续保持动态分派。
- 将内部方法契约贯通 receiver、`super`、默认参数、可变参数与协程调用，继承行为不再退化为仅按名称或语义类型猜测。
- 统一嵌套枚举在生成声明、参数、返回值、容器和跨脚本引用中的规范身份，消除大型多脚本构建中的非法 C++ 类型名。
- 附着式脚本描述符替换现会完整比较属性、方法、信号、常量、默认值、元数据和 RPC 身份；Callable 复制/移动赋值也可安全处理自别名。
- 发行 SDK 升级至 schema 8；Windows 创建构建命令前必须通过 MSVC frontend、19.x 工具集声明、静态 CRT 和清单元数据一致性校验。
- 集中管理已交付目标矩阵，在生成项目前拒绝尚不可用的 Windows arm64、Linux arm64 与 Android x86_64 导出；生成描述符不再声明缺失的桌面 ARM64 库。
- 保持零源码改动且仅在导出阶段接管：常规编辑和运行继续使用项目现有 `.gd`；扩展或全新 `.gdpp` 语法不属于本次发布范围。

## 1.7.1

- 完成强类型可变参数函数、构造器、方法、lambda、反射元数据、调用 thunk、缓存指纹和附着式脚本分派，并贯通 Godot 4.4～4.7；语法始终由 GDPP 自有前端解析，不依赖宿主 GDScript parser。
- 完成跨脚本 preload 命名空间，覆盖根类与嵌套类、枚举、常量、静态字段/函数、强类型资源引用、转换、类型测试、继承和规范化 Inspector 枚举身份。
- 加固跨版本原生对象 lowering，覆盖 RefCounted 值、裸 Object 指针、singleton 包装、属性 getter 真实返回类型、显式类型化 null 分支、动态调用和协程返回 ABI。
- 运行时与导出发现链现可解析脚本及场景 Autoload UID，事务式重写被剥离的 Autoload，拒绝 editor-only 运行资源图，并隔离 editor-only 生成注册。
- 统一以受保护赋值处理 Dictionary、强类型容器、String、StringName、NodePath、Array、PackedArray、Callable 和 Signal，消除原生资源滞留与自赋值损坏；附着式脚本描述符遵循同一规则。
- 集中传递嵌套 CMake 的 compiler、toolchain file、sysroot、target triple、Apple deployment、MSVC CRT、RC/MT、generator platform 和 toolset；独立构建的 provider 扩展可与打包 SDK 保持一致 CRT 并完成链接。
- 发行 SDK 升级至 schema 7 / runtime ABI 8，对 C++17、异常模型、MSVC 静态 CRT、Android `c++_shared`、源码完整性、平台、架构、profile 和运行时契约执行失败关闭校验。
- 仅允许由 GDPP 新语法触发且精确匹配的 Godot 4.4 parser 提示，所有其他导入、导出、运行时与 PCK 诊断仍严格失败。

## 1.7.0

- 新增 GDScript 继承独立第三方 GDExtension 类型的零源码改动 AOT 支持，无需重新构建、重新链接或修改供应商插件。
- 引入基于 `ScriptLanguageExtension`、`ScriptExtension` 与 `ScriptInstance` 的附着式 AOT 后端：原生 Object 仍由供应商创建和持有，生成的 C++17 行为负责脚本字段、方法、属性、信号、通知和 RPC 元数据。
- 保留附着式原生根类之上的脚本继承，包括字段初始化、`_init`、方法/属性分派、虚回调、信号行为，以及 RPC 配置的继承与覆盖。
- 外部 `super` 调用使用供应商反射出的 MethodBind 精确兼容哈希和稳定 GDExtension ABI，不再采用不安全的跨动态库 C++ 继承或猜测签名。
- 扩展 ClassDB 采集与 `gdpp_bridge.json` 校验，纳入供应商身份、精确可调用元数据、与加载顺序无关的供应商解析、缓存失效，并在关键反射缺失时失败关闭。
- 二进制导出期间把场景、独立资源、内嵌资源和 Autoload 转换为附着式编译脚本，同时保留供应商原生类型与序列化状态。
- 导出游戏逐字节保留第三方描述符和目标平台动态库，provider-first 与 project-first 两种注册表顺序均可安全启动，不要求客户项目或供应商源码改动。
- 加固 macOS Universal 2 导出：事务式规范化供应商扫描描述符、逐 Mach-O 切片验证、崩溃恢复，并在导出后恢复所有源描述符。
- 发行 SDK 升级至 schema 6 / runtime ABI 7，对宿主、桌面、移动与 Web 目标中的全部附着式运行时头文件和源文件执行摘要校验。

## 1.6.0

- 完成 Godot Variant 完整类型域、可空性模型和零值真值语义，并贯通语义分析、类型化 HIR、生成的 C++17 与原生运行时。
- 集中定义数值、字符串、内建值、PackedArray、对象、Ref、RID 和容器的严格赋值、显式转换、分析器兼容、运行时可构造与原生存储规则。
- 支持 `Array[T]` 与 `Dictionary[K,V]` 参数化 `as` 目标、跨脚本限定枚举转换目标、常量严格转换检查和运行时受检转换；对原生存储无法保持的确定性 String/RID 路径在生成前失败关闭。
- 强类型容器存储改为精确校验运行时元素、键、值、对象类和脚本元数据，不再依赖 godot-cpp 的隐式转换构造器，同时保持 GDScript 分析器对未约束容器和 PackedArray 边界的接受行为。
- 为必然失败的常量转换、直接违反不变规则的强类型容器、确定性运行时转换失败及 Object 背书 RID 等原生存储不可表示路径增加稳定诊断。
- 隔离嵌套警告控制作用域并按固定格式化器统一分析器源码，使生成代码在 Clang、GCC 与 MSVC 下保持警告洁净。
- 将编译器的抽象脚本元数据贯通至 ClassDB 导出校验，并为宿主、Android、Web 与 iOS SDK 统一补充所需运行时注册接口，使二进制导出正确保留原生抽象契约。
- 原生运行时 ABI 升级至 5，发布新的受检转换和强类型存储契约。

## 1.5.0

- 新增端到端流敏感类型系统，覆盖 `is`/`is not`、空值与真值检查、短路表达式、`if`/`while`、三元表达式、守卫后支配路径、结构化 `match` 和带守卫绑定。
- 引入稳定符号身份与分支状态格，提供保守汇合、直接赋值失效、Callable 分析隔离和大型逻辑链有界分析。
- 类型化 HIR 同时保留有效类型、真实存储类型和非空证明，使 C++ 后端能够专门化 Variant 存储读取且不改变原生存储 ABI。
- 为静态解析的方法调用和属性读取加入携带源码位置的 null/已释放对象检查，避免生成未经保护的原生解引用。
- 完成默认参数降级与 Callable 元数据，覆盖省略实参分派、override 兼容性、Callable 参数数量校验及精确 Godot 虚函数 ABI 适配。
- 实现 GDScript 语言工具的常量与运行时契约，包括 `assert`、`is_instance_of`、`type_exists`、`convert`、`str_to_var` 和 `var_to_str`。
- 对未解析值、类型和注解常量执行失败关闭，并常量折叠导出属性 hint/usage 元数据，同时保留 Godot 脚本变量标识。
- 在字段、方法、访问器、`super`、节点简写和嵌套 lambda 中统一执行静态上下文归属规则，同时保留合法静态访问和内部类访问。
- 显式顺序化所有急切求值的二元操作数，使生成的 C++17 在原生值、Variant、成员测试、幂和比较表达式中保持 GDScript 从左到右的副作用顺序。

## 1.4.0

- 将前端常量求值、HIR 优化、生成的强类型 C++ 与动态运行时整数运算统一到同一套可移植 64 位契约。
- 明确定义回绕溢出、归一化移位、有符号除余边界、复合赋值和防溢出的原生范围终止，不再触发 C++ 未定义行为。
- 将发行 SDK 升级到 schema 5 / runtime ABI 4，为所有宿主和导出目标打包并校验共享整数语义头的哈希。

## 1.3.0

- 完成浮点计数及 Vector2/Vector2i、Vector3/Vector3i 数学范围的原生 `for` 循环支持，方向、步长和边界行为与 Godot 保持一致。
- 新增基于 Godot `_iter_init`、`_iter_next`、`_iter_get` 协议的静态对象迭代支持，在编译期验证协议并保留迭代结果类型。
- 强化类型化循环安全性，对数组和字典字面量执行上下文类型化，精确诊断非法元素、键和循环变量声明。

## 1.2.0

- 新增强类型数组与字典的完整原生支持，在语义分析、跨脚本接口和生成的 C++ 中保留元素、键与值类型契约。
- 完善强类型容器安全检查，覆盖赋值、参数、返回值、导出属性、嵌套容器及项目依赖边界。
- 优化生成代码，直接构造原生强类型容器字面量并消除冗余包装拷贝，降低分配与转换开销。

## 1.1.0

- 大幅提升最新版 GDScript 兼容性，完善字符串、数字、Unicode 标识符、局部常量、尾随逗号和 lambda 等语法支持。
- 完整支持数组、字典、嵌套及剩余项 `match` 模式，并贯通类型检查、优化与原生代码生成。
- 完善 `await` 与协程编译，支持返回值、立即完成、并发调用及继承场景下的动态分派。
- 强化多脚本工程编译，改进类型与调用关系分析，实现更精确的依赖失效和增量重建。
- 提升编译器安全性与诊断质量，加入 Unicode 关键字防伪、非法输入处理及前端资源上限保护。
- 改善跨平台导出兼容性，继承 Godot 官方模板设置。

## 1.0.0

- 首个版本，提供 GDScript AOT 编译器和 Godot 编辑器插件。
