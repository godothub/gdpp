## 1.8.0

- 生成的客户 GDExtension C 入口 ABI 统一缩短为 `gdpp_library_init`，生成注册代码、运行时描述符、Apple 静态入口登记、ELF/Mach-O 导出表、Wasm 检查、PCK 审计及各平台发布门禁均强制校验这一唯一符号。
- 完整实现 GDScript `breakpoint` 语句，从词法、语法、语义模型、HIR、类型化 MIR、verifier、C++17 生成一直贯通到 Godot 原生调试器桥接，合法的生产脚本不再因该语句而在 AOT 前端失败关闭。
- 通过 `ScriptLanguageExtension` 向调试器报告生成代码的原始 `.gd` 路径、函数、当前源码行、精确处理遮蔽关系的词法局部变量，以及当前脚本与继承脚本成员；调试表达式使用 Attached 脚本相同的帧宿主求值。
- 普通方法、访问器、静态函数、lambda、附着到第三方 GDExtension 基类的脚本和协程恢复点统一保留调试行为；Release 产物完全移除断点插桩，没有连接 Godot 调试器时也不创建无效调试帧。
- Attached 原生实例返回 Godot 公开的规范 `ScriptInstance` 句柄，使调试器、引擎回调、属性访问和生命周期追踪始终指向同一个引擎对象，不再泄漏内部实现指针。
- 在 Clang、GCC 和 MSVC 严格 warning-as-error 构建下保留 GDScript 合法的词法遮蔽，同时不关闭其他原生编译诊断。
- macOS Universal Debug 导出可以复用经过验证、仅提供 Release 的第三方 provider fat binary；GDPP 只在导出包内的描述符字节中增加 Debug 别名，不修改客户源码目录中的描述符或动态库。
- 生成的 breakpoint fixture 会与真实 godot-cpp 一同编译，并使用官方 Godot 4.6.2 对附着第三方 GDExtension 的 Debug、Release 项目执行导出和运行验证。
- 发行 runtime ABI 升级到 14；缺少调试帧或规范 ScriptInstance 契约的旧 SDK 会在预检阶段失败关闭，不会生成 ABI 不兼容的客户库。

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
- 商业性能合同收紧为：启动、稳态帧工作及每个基准族相对 GDScript 的 AOT 回退均不得超过 10%；仓库质量校验会阻止后续放宽该阈值。
- 直接原生构建器改为跟踪每个生成翻译单元的传递式引号 include 图，不再让每个对象文件依赖全部生成头文件；只修改实现时仅重编自身对象，公开脚本结构变化时也只重编真实依赖者与注册单元后重新链接。
- 附着式脚本的 `self` 跨越强类型原生调用边界时保留类型正确的 `RefCounted` 强引用，覆盖嵌套消息对象和生成的协议模型，不再执行不安全的 owner 转换。
- 强类型引擎对象、`RefCounted` 值与供应商对象拥有的附着式 `self` 统一通过 Godot Variant 对象身份比较，避免协议与模型类在导出时生成非法的 `Ref<T> == Object*` C++。
- 发行运行时 ABI 升级至 13，旧 SDK 或复制不完整的 SDK 会在导出预检阶段被拒绝，不再链接生命周期或信号调用契约不兼容的生成代码。
- 导出运行回归新增二进制消息头、嵌套分发表、`Callable` 处理器、预连接的脚本与供应商信号、供应商强类型容器、运行时实例化附着节点、`_init`/`_enter_tree`/`_notification`/`_ready`/`_exit_tree` 完整生命周期、继承与内部类 ready 行为以及 128 条连续消息压力路径。
- 每个 Godot 集成测试使用独立的构建树日志，并对共享编辑器用户数据的测试实施串行资源锁，避免并发日志轮转被误判为 GDPP 运行时崩溃。
- 在生成异步 `while` 条件或 `for` 可迭代对象之前先安装协程循环携带存储，确保条件、循环体及恢复后的 continuation 在跨越 process frame 或 Signal 挂起后始终观察同一个原生值，不再让分批后台任务停留在过期副本上。
- 在保持 C++ 警告洁净的同时完整保留源码绑定：未使用的方法、setter、lambda、剩余参数、局部变量、迭代及 match 绑定和 await 载荷均继续具备准确的 GDScript 生命周期与求值语义。
- 将“4,996 个对象、每个 process frame 处理 200 个”的真实挂起批处理循环加入 Godot 版本矩阵、桌面导出宿主门禁及已安装发行包冒烟测试，阻止过期协程状态从其他编译器、SDK 或组包路径重新出现。
- 使用本机回环服务验证 Windows 导出运行时的 HTTP 鉴权、四份 JSON 下载、远程图片解码、WebSocket 二进制分派、两个受支持礼物平台、点赞、英雄触发、积分与分组同步、重复响应、97 包礼物突发、服务端正常关闭、短帧、截断载荷及畸形消息；全部有效流程均在不修改客户源码的前提下完成。

## 1.7.9

- 移除仍声明 Node.js 20 的 `ilammy/msvc-dev-cmd` 依赖，改用仓库自有、零第三方运行依赖的 Node.js 24 Action：通过 `vswhere` 定位 Visual Studio，初始化受支持的 MSVC x64 工具链，仅导出发生变化的环境变量，保留包含 `=` 的值，去重 Windows 工具路径，并对不完整或架构不匹配的工具链状态实施失败关闭。
- 工作流语义校验器升级至原生支持 Node.js 24 Action 元数据的 actionlint 1.7.12，并对编译器核心、原生集成和商业 Windows 宿主构建统一执行本地 Node.js 24 初始化契约门禁。
- Node.js 24 的 MSVC 初始化命令改为不经通用 Windows 参数二次转义、原样交给 `cmd.exe`，确保 `Program Files` 下带引号的 Visual Studio 路径可正确执行；新增回归测试锁定精确 `/c` 载荷与 verbatim 进程契约。
- Windows、macOS、Linux、Android、iOS 与 Web 的客户运行库文件名前缀由 `gdpp_project` 统一缩短为 `gdpp`，同步覆盖 Windows 导入库、Mach-O install name、iOS 切片动态库及 XCFramework 内部布局；GDExtension 入口 ABI `gdpp_project_library_init` 保持稳定。
- 原生构建与导出转换修订号同步推进；下一次 AOT 导出前事务式清除旧命名产物（包括目录型 iOS XCFramework），无法清理时失败关闭，避免旧库残留或误入客户包。
- 桌面运行、APK、XCFramework、Wasm、PCK、插件组包、清理及工作流门禁全部切换为新命名；商业插件包同时拒绝当前客户构建产物与旧命名产物，并通过精确分类保留编译器和 fallback 库。
- 三个桌面插件 ZIP 全部恢复标准 `addons/gdpp/` 根目录，用户直接解压到 Godot 项目根目录即可得到可发现的 EditorPlugin，不再误装为无效的 `res://gdpp/`；发行包清单升级至 schema 5，并对精确安装目录执行门禁。
- 编辑器兼容门禁更新至官方 Godot 4.7.1 修复版本，并让集成项目把 Variant 推断警告按错误处理，覆盖此次错误安装目录在客户严格项目设置下暴露的故障。
- Windows 生产导出器改用 `vswhere` 查找 Visual Studio 并验证已安装 x64 C++ 工具组件，覆盖 Preview 与非默认安装路径且保留显式编译器覆盖，同时移除测试机专用的用户目录兜底；无法满足条件时给出可操作的工具链诊断，不再把已安装的 MSVC 环境误判为缺失。
- 原生构建规划前将选定的 MSVC `cl.exe` 解析为 Visual Studio 工具目录中的绝对路径，并显式调用同目录 `link.exe`；客户即使从继承开发环境的 Godot 启动，链接阶段也不会再误入 Git/MSYS 的同名无关工具，直接进程入口同时保留等价的失败关闭防线。
- 在成功的 AOT 导出期间保持编辑器编译器描述符不可变，并将其标记为不可热重载以匹配实际 godot-cpp 构建契约，消除把已加载编译器扩展替换为项目运行时所导致的 GDExtension 实例重建失败。
- 让 GDPP 导出插件先于 Godot 内置 GDExtension 扫描器执行，只在包内稳定的 `addons/gdpp/gdpp.gdextension` 路径替换描述符字节，并通过公开导出 API 对生成的项目库进行且仅进行一次登记。
- 在单一登记路径下保留各平台原生打包契约：Android 携带准确 ABI 标签，Web 携带选定线程特性，桌面库保持平台落位规则，iOS 根据 Godot 4.4 或 4.5+ 注入 XCFramework 所需的静态入口回调与未定义符号链接参数。
- 完全在内存中解析 macOS Universal 2 第三方 GDExtension 库及依赖，验证胖 Mach-O 载荷，按字节原样打包 provider 描述符，不再为架构发现改写客户扩展文件。
- 将剩余扩展注册表事务限制在有意的纯源码导出或非桌面源码回退场景，因为 Godot 的强制元数据阶段必须在这些场景排除运行时描述符；正常 AOT 导出的注册表和全部扩展描述符均保持字节级不变。
- 编辑器启动时恢复 1.7.9 之前版本遗留的中断描述符事务；新导出不再创建编译器或 provider 描述符备份。
- 增加 SHA-256 导出状态门禁，在成功、回退及失败关闭导出后拒绝编辑器/provider 描述符变化、扩展注册表变化、客户扩展增删以及事务备份残留。
- 复用已构建的 host components，在 macOS、Linux 与 Windows 上通过官方 Godot 4.7.1 实际导出并运行桌面包；同时让 Android APK、iOS/Xcode、Web 线程/无线程、Godot 4.4-4.7 Linux 及独立 Universal 2 provider 路径统一接受描述符不可变与原生产物单次登记门禁。
- 在不使用 C++20 `std::string::starts_with` 的前提下处理带 UTF-8 BOM 的 `vswhere` 输出，保持 Visual Studio 发现逻辑符合 C++17 合同，并由 Windows 编译器与 host-component 门禁实际编译覆盖。
- `gdpp-mac.zip` 从仅 arm64 的宿主契约升级为真正的 Universal 2 compiler、fallback 及 Godot 4.4～4.7 桌面 SDK；Apple Silicon 与 Intel 编辑器现可加载同一插件，标准官方 Universal 2 导出模板无需自定义模板或改写预设即可工作。
- macOS 发行组件新增未修改 `macOS Universal` 预设的真实导出与运行门禁，并强制每套 compiler、fallback 及桌面 godot-cpp 归档同时包含 arm64、x86_64 切片与 macOS 11.0 最低部署版本。
- 新增 macOS 与 Windows 组包后发布门禁：将最终 `gdpp-mac.zip`、`gdpp-win.zip` 安装到全新客户项目，使用官方 Godot 4.7.1 与官方模板完成导入、AOT 导出及成品运行，并审计 PCK 源码剥离、项目库单次登记和不可变状态；出现任何诊断或契约违规都会阻止 readiness 与发布。
- 两处 macOS 部署目标审计均改为完整读取 `otool` 输出后再匹配 load command，在继续强制 Universal 2 与 macOS 11 基线的同时，避免 `pipefail` 将已成功的导出及运行误判为断管失败。
- 精确分类 Godot 4.6.2 官方 iOS 模板对已移除可选项 `application/boot_splash/fullsize` 的上游警告且不修改客户项目；Xcode 导出门禁仍完整收集全部诊断，并对除此之外的任何警告或错误失败关闭。
- 桌面包的纯导入门禁先让官方编辑器执行有界帧循环预热，再将预热日志与导入、导出、运行日志一并审计；由此规避 Godot 4.7.1 首次扫描后立即自动退出的关闭竞态，不重试失败命令，也不放宽诊断门禁。

## 1.7.8

- 附着脚本描述符注册严格保持为纯元数据操作：脚本常量（包括嵌套资源容器和其他纯值）只登记无捕获延迟求值器，不再于 GDExtension 类注册期间执行求值。
- 仅在 Godot 请求编译后 Script 常量表时解析延迟常量，同时保留本地与继承常量反射、派生类遮蔽、确定性描述符身份、生成常量的线程安全缓存及显式卸载清理。
- 修复导出游戏在原生扩展启动期间因脚本常量预加载场景或资源而直接退出的问题；覆盖依赖尚未就绪的 2D/3D 物理、渲染、音频、导航及第三方 GDExtension 服务的资源构造。
- 对缺失的延迟求值器、立即/延迟常量重名、继承合并及重复描述符实施失败关闭校验，并确保注册表锁内不执行客户代码或引擎资源服务。
- 为生成项目注册阶段剩余的编辑器专用 Engine 查询增加空值保护，必需单例不可用时返回受控初始化错误，避免原生空指针解引用。
- 增加生成代码纯度测试，覆盖资源常量、嵌套 preload 容器、资源字段默认值、服务型实例字段、静态 preload 与资源默认参数。
- 增加 Godot 4.4–4.7 真实独立 GDExtension 导出/运行回归，验证本地及继承脚本预加载含 `CircleShape2D` 的场景，并校验编译 Script 常量表反射。
- 修复根脚本及内部类的运行期构造：每个生成 Script 在附着前写入已解析描述符的契约哈希；`.new()` 构造失败时现会给出可操作诊断，不再延迟退化为一连串空值属性错误。
- 附着式 ScriptInstance 字段改由描述符携带的强类型 getter/setter 回调分派，完整保留自定义访问器、Variant 转换、继承属性及强类型容器键语义，不再依赖 ClassDB 对 GDExtension behavior 对象的属性反射。
- 独立 provider 运行夹具增加动态类型内部类构造、读写及强类型 Dictionary 下标回归，并为同一二进制导出路径增加 Windows x86_64 预设。
- 原生工具链顺序测试统一规范化 LF/CRLF，使 Windows 串行门禁只验证真实进程顺序，不再受行尾格式误报影响。
- 发行运行时 ABI 升级至 11，使旧 SDK 在预检阶段明确失败，避免按旧的立即求值描述符布局继续编译。
- Godot 属性类型改为依据 getter/setter 的真实 ABI 解析，不再错误选取 Inspector 资源候选列表中的第一个具体类型；Godot 4.4～4.7 的 Shader、粒子、天空、雾、几何体、灯光、贴花、相机属性及同类多态资源读写均保留正确基类契约。
- 无所有者的静态函数 Callable 在 Callable 生命周期内保持为有效信号目标，实例绑定 lambda 仍执行 Object 生命周期校验；由此恢复 `HTTPRequest.request_completed` 等异步信号的静态绑定回调。
- 附着式根脚本及内部类进入强类型 Array/Dictionary 时同时保留 provider 持有的真实原生基类与唯一规范 Script 资源，避免有效编译对象被误判为普通 `RefCounted` 或第三方 GDExtension 实例而遭容器拒绝。
- 强类型容器形成 C++ 身份前统一同一脚本类的短名、限定名及生成原生名，使字段、函数返回值、链式调用与局部变量始终使用同一个 ABI 稳定的 Array/Dictionary 特化。
- 规范强类型容器 Script 资源不再依赖生成描述符的登记先后顺序；跨脚本默认值可在目标描述符登记前预留精确身份，随后在同一资源上绑定契约，不产生启动错误，也不退化为无类型容器。
- 穷举校验 Godot 4.4～4.7 元数据中的全部多态资源属性访问器契约，覆盖所有兼容 ShaderMaterial 的 Canvas、粒子、雾、天空、几何体、CSG、Mesh 与 Tile 材质槽位。
- 独立导出运行夹具增加动态 `ShaderMaterial` 赋值、逐帧 Shader uniform 更新、本机回环 HTTP 图片传输、响应头校验、PNG 解码及 `ImageTexture` 节点赋值的端到端验证。
- 将 Godot 属性 getter 返回 ABI 与 setter 参数 ABI 拆分为独立的语义及 HIR 契约；赋值诊断、复合写回和 C++ 参数实体化均使用写入侧真实类型，不再假定当前引擎的读写访问器永远对称。
- 对 Godot 4.4～4.7 的每一条属性元数据自动核验读写访问器，并增加 Canvas、Tile、2D/3D 粒子、雾、天空、几何体、CSG 与 Mesh 全部材质家族的生成代码门禁。
- 独立二进制导出夹具覆盖场景内联 `ShaderMaterial`、外部 `.tres` 材质、`.gdshader` 依赖、序列化 uniform 和运行期 uniform 写入，确保 AOT 场景/资源重写保留预绑定与动态绑定的同一资源契约。
- 网络图片回归扩展至 PNG、JPEG 与 WebP，验证 Content-Type 分派、文件签名回退、`PackedByteArray` 解码、静态异步回调及 `ImageTexture` 创建的完整路径。
- macOS、Linux 与 Windows 商业宿主组件改为和编译器、Godot、Android、Web、iOS 全部门禁并行构建；仅在全部产物与测试成功后启动最终组包，移除原有“先完成测试、再构建桌面插件”的串行关键路径。
- 将桌面组件生产、发布编排及三平台组包拆分为职责独立且可复用的工作流，并增加结构化拓扑门禁，防止后续误加串行依赖、遗漏组包前置条件、脱离产物生产阶段单独组包或产生第二个发布入口。
- 每个 Godot 版本的发行 SDK 改为共享一份运行时、godot-cpp 头文件树、源码树和 `lib` 目录；Android、Web 线程/无线程、iOS 与当前桌面宿主只再提供各自不同的优化 Release 库，不再重复携带公共 SDK 内容。
- 从共享 SDK 中按平台、架构、构建配置及 Web 线程模式精确选择目标清单和 godot-cpp 库，同时保留对旧组件目录的兼容，并优先使用精确 macOS 架构库而非 Universal 回退库。
- 三个桌面压缩包的顶层目录由 `addons/` 包装改为直接的 `gdpp/`，发布前严格校验最终 ZIP 目录、目标清单集合、库数量、已废弃平台目录及跨目标运行时契约。
- Godot 4.4 强类型可变参数解析诊断的位置改为从兼容夹具源码动态推导；无关代码行变化不再使严格白名单失效，同时仍不会掩盖其他导入或导出错误。

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
- ABI 兼容的附着式脚本 `self` 调用改走生成 C++ 虚继承链；跨对象调用及 ABI 变化的 override 仍保留 ScriptLanguage 动态分派。强制执行的 GDScript/AOT 运行矩阵直接测量优化路径，且不放宽性能回归阈值。
- 继承的 ClassDB 调用继续作用于提供方持有的 Godot 对象，生成脚本分派则作用于附着 behavior，确保原生 GDExtension 方法能够反向调用客户脚本 override。
- 附着式或动态属性读取跨越 Variant 边界时保留语义值类型，包括强类型 Dictionary 和跨脚本访问器。
- 每个客户目标 SDK 只发布一套优化后的 `template_release` godot-cpp 归档，并同时用于 Debug 与 Release 导出；编译器的 editor 绑定只保留在预构建插件内部，不再分发第二套客户静态库。
- 将原有 16 个按版本单宿主包与完整包收敛为三个跨版本桌面包：`gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip`。每个包包含本宿主 compiler/fallback 和 Godot 4.4～4.7 全部桌面 Release SDK；三个包均包含 Android 与 Web Release SDK，仅 mac 包包含 iOS。
- 通过显式运行时白名单暂存宿主 compiler/fallback，使 Windows 发布包排除 MSVC `.lib`/`.exp` 导入产物，同时保留最终归档的严格审计。
- 每个生成的客户翻译单元仅针对所选导出目标编译一次；Debug/Release 对象缓存彼此隔离，同时共享确定性的前端结果和生成源码状态。
- `Dictionary` 接收者跨越 `Variant` 边界时完整保留 GDScript 的 `Dictionary.key` 读写语义，覆盖 JSON/HTTP 响应字典与嵌套复合赋值；异步认证和网络回调不再因响应字段被静默读成空值而中断后续流程。
- 为全部十种 `PackedArray` 建立共享存储语义，局部别名、字段、参数、返回值、默认参数、`Callable`、lambda、Signal 与动态调用均与 GDScript 一致；显式构造复制和参数重绑定仍保持独立。
- 公共脚本方法统一通过 Variant ABI 绑定，避免 godot-cpp 指针调用在 GDExtension 边界触发写时复制；下标读写、迭代、方法调用及引擎 API 转换均通过同一共享存储契约。
- 将解析后的参数契约从语义分析贯通至 HIR 与项目接口，确保同脚本、继承、嵌套类和跨脚本调用均按被调用方真实原生 ABI 实体化参数，不再依赖调用方推断的表达式类型。
- 所有生成的原生到 Variant 边界统一进入与重载无关的运行时适配器，覆盖数组、字典、动态调用、信号、Callable、match 绑定、外部 provider、工具函数和引擎可变参数。
- 原生 `PackedArray` 转换改为显式执行，固定引擎参数独立适配，消除 MSVC 重载歧义且不重新引入值复制，也不削弱反射 Variant ABI。
- 发行 SDK 升级至 schema 11 / runtime ABI 10，强制执行单绑定导出契约，并在桌面、Android、iOS 与 Web 清单中完整校验附着式及引用语义运行时。
- 新增全部 `PackedArray` 类型的 GDScript/AOT 差分、公共方法与属性反射别名、Signal/Callable 动态边界以及逐字节二进制序列化回归，并在真实 Godot 原生运行中验证。
- 新增生成代码架构门禁及真实 Godot fixture，覆盖固定 PackedArray 参数、引擎及工具可变参数、动态容器、序列化和跨脚本缓存失效。
- 将生成项目 CMake 冒烟测试替换为官方兼容语料中每个生成单元的真实 C++17 语法编译，同时保留完整插件、独立 provider、导出、运行时和 PCK 门禁。
- 使用打包 SDK 在 MSVC 下验证全新 Windows x86_64 Debug/Release 导出，审计内嵌 PCK 仅含一个项目原生库且不泄漏源码、编译器或 SDK，并在编辑器外独立运行导出程序。
- Windows 插件发布构建对并行 MSVC SDK 编译启用受协调的 PDB 写入，避免大型 godot-cpp 目标因编译数据库争用而随机失败；客户导出仍保持逐文件串行。
- Visual Studio 多配置构建现显式选择 Release SDK、记录实际 editor 优化配置并把插件 DLL 直接写入可安装的 `binary` 目录；发布链接优化与 Windows 长路径测试同样按配置可靠执行。
- 将 `GDPP AOT Build` 覆盖界面精简为标题与当前任务两行，并在项目源码编译期间追加实时逐文件计数。
- 进度条几何和动态任务文字改为直接提交渲染服务器，并在每次强制呈现前完成同步；Windows 无需移动窗口即可同时刷新文字与进度。
- 新增真实后台构建、进度主线程派发、JSON 字典运行时、无界面进度模型、打包、交付、AddressSanitizer、ThreadSanitizer 与 UndefinedBehaviorSanitizer 门禁，覆盖编辑器响应性、分层进度分配、精确界面文案、原生内存/线程安全和单填充控件实现。
- 构建专用编译器描述符移入 Godot 不会递归扫描的 `.godot` 元数据目录，并在配置时清理旧的插件内描述符命名族，避免复用构建树时重复注册 ClassDB 类及破坏 Android 场景转换。

## 1.7.6

- 原生编译进度改用绑定当前焦点导出对话框的独占临时窗口，在所有桌面主机上均保持位于 Godot 模态导出界面之前。
- 移除列式伪连续填充与构建 profile 切换时的进度归零，整个 AOT 操作统一使用一条全程单调递增的连续进度条。
- development 与分发构建等分总进度；每个大阶段内再由扫描、解析、语义分析、脚本预编译、原生文件生成、项目编译和链接等分，支持文件回调的阶段继续按单文件细分。
- 独立显示实时文件计数，界面文案不再暴露后端转译术语，并统一使用简洁的 AOT、Debug 与 Release 构建提示。
- 新增无界面进度模型回归与交付契约，覆盖顶层窗口归属、分层进度分配、精确界面文案和单填充控件实现。

## 1.7.5

- 修正 Windows MSVC 环境初始化：`cmd.exe /c` 原始负载可正确进入 `vcvars64.bat` 与 `cl.exe`，不再被 C 运行时参数转义破坏嵌套引号。
- 生成翻译单元的编译与链接严格改为单命令串行执行，消除导出期子进程突发，并避免大型项目中的工具链内存争用。
- 每条串行原生命令运行期间持续刷新 Godot 编辑器，并在每个翻译单元完成后推进编译进度。
- Windows、macOS 与 Linux 的隐藏工具链进程现会捕获 stdout/stderr，保留失败文件、阶段与退出码，并将有上限的原始诊断显示在 Godot 导出结果中。
- 新增跨平台执行回归，验证命令串行顺序及 stderr 保留；交付门禁同时禁止原生导出调度重新退化为并行执行。

## 1.7.4

- 在全部商业插件压缩包中包含原生构建进度界面，确保全新安装可在导出前正常加载编辑器插件。
- 当暂存插件或最终确定性 ZIP 缺少进度界面时让发布组装失败。
- Debug 与 Release 游戏导出统一使用优化后的 `template_release` godot-cpp 绑定，同时保留 Godot 编辑器进程所必需的独立 `editor` 绑定。
- 从主机、Android、iOS 和 Web SDK 包中移除全部 `template_debug` 归档；每个平台及线程 ABI 仅携带一套分发绑定，不再重复发布数百 MB 的调试对象。
- 通过 GDPP 自有编译宏保留 GDScript Debug 导出的 `assert()` 求值语义，使其不再依赖 godot-cpp 绑定目标和原生优化配置。
- Debug 导出的项目动态库现与 Release 导出使用相同的商业级优化、死代码裁剪和符号隐藏流程。
- 发行 SDK 升级至 schema 9，显式声明分发绑定及优化配置，并在编译前拒绝陈旧、Debug 绑定或混合安装的 SDK。
- 新增 Godot 4.4～4.7 真实 Debug 导出运行门禁，验证仅安装 Release 绑定时脚本断言仍保持启用。
- 新增按 SDK 版本区分的完整插件包 `gdpp-4.4.zip`～`gdpp-4.7.zip`；每个包都组合 macOS arm64、Linux x86_64、Windows x86_64 三套编辑器二进制及桌面 SDK，并包含 Android arm64、iOS 真机/通用模拟器、Web threads/nothreads 导出 SDK。
- 插件可从完整包的平台隔离布局中自动选择当前桌面主机 SDK，同时继续兼容体积更小的单主机包。
- 完整包采用可复现组装，并在主机或目标缺失、插件静态文件冲突、运行时 ABI/哈希不一致、混入其他 SDK 版本、嵌套压缩包、生成产物或 Debug 绑定时失败关闭。
- 从已安装插件及全部发行压缩包中移除独立的第三方声明汇总文件。

## 1.7.3

- Windows 导出时的编译器、环境初始化和链接器子进程不再创建可见控制台窗口。
- 通过明确的直接构建 API 保留有界并行编译和分阶段顺序链接；Godot 导出路径不再携带可配置的 CMake 生成开关。
- 在 Godot 打包前显示独立的连续原生构建进度条，按真实文件进度覆盖项目扫描、GDScript 解析、语义分析、GDScript 到 C++ 转译、C++ 编译和原生链接。
- 开发版与分发版构建中的前端、编译和链接进度保持分阶段有序且单调递增，并在进入 Godot 打包进度前自动移除。
- 将后台进程行为纳入交付回归测试。

## 1.7.2

- 完成第三方 GDExtension 的无损反射，覆盖 Variant、强类型 Array/Dictionary、Signal、对象类、引擎类型化 Dictionary 契约和编码容器元数据，无需修改源码即可保留 provider API。
- 按实际生成的 C++ ABI 重构内部类 override：兼容覆盖进入原生虚槽，不兼容的 GDScript 契约使用私有实现符号，Variant 接收者继续保持动态分派。
- 将内部方法契约贯通 receiver、`super`、默认参数、可变参数与协程调用，继承行为不再退化为仅按名称或语义类型猜测。
- 统一嵌套枚举在生成声明、参数、返回值、容器和跨脚本引用中的规范身份，消除大型多脚本构建中的非法 C++ 类型名。
- 附着式脚本描述符替换现会完整比较属性、方法、信号、常量、默认值、元数据和 RPC 身份；Callable 复制/移动赋值也可安全处理自别名。
- 发行 SDK 升级至 schema 8；Windows 创建构建命令前必须通过 MSVC frontend、19.x 工具集声明、静态 CRT 和清单元数据一致性校验。
- 集中管理已交付目标矩阵，在生成项目前拒绝尚不可用的 Windows arm64、Linux arm64 与 Android x86_64 导出；生成描述符不再声明缺失的桌面 ARM64 库。
- 扩充独立 provider、双加载顺序、严格生成 C++ 与官方 Godot 4.6.x 回归门禁，并覆盖大型多脚本兼容语料。
- 保持零源码改动且仅在导出阶段接管：常规编辑和运行继续使用项目现有 `.gd`；扩展或全新 `.gdpp` 语法不属于本次发布范围。

## 1.7.1

- 完成强类型可变参数函数、构造器、方法、lambda、反射元数据、调用 thunk、缓存指纹和附着式脚本分派，并贯通 Godot 4.4～4.7；语法始终由 GDPP 自有前端解析，不依赖宿主 GDScript parser。
- 完成跨脚本 preload 命名空间，覆盖根类与嵌套类、枚举、常量、静态字段/函数、强类型资源引用、转换、类型测试、继承和规范化 Inspector 枚举身份。
- 加固跨版本原生对象 lowering，覆盖 RefCounted 值、裸 Object 指针、singleton 包装、属性 getter 真实返回类型、显式类型化 null 分支、动态调用和协程返回 ABI。
- 运行时与导出发现链现可解析脚本及场景 Autoload UID，事务式重写被剥离的 Autoload，拒绝 editor-only 运行资源图，并隔离 editor-only 生成注册。
- 统一以受保护赋值处理 Dictionary、强类型容器、String、StringName、NodePath、Array、PackedArray、Callable 和 Signal，消除原生资源滞留与自赋值损坏；附着式脚本描述符遵循同一规则。
- 集中传递嵌套 CMake 的 compiler、toolchain file、sysroot、target triple、Apple deployment、MSVC CRT、RC/MT、generator platform 和 toolset；独立构建的 provider 扩展可与打包 SDK 保持一致 CRT 并完成链接。
- 发行 SDK 升级至 schema 7 / runtime ABI 8，对 C++17、异常模型、MSVC 静态 CRT、Android `c++_shared`、源码完整性、平台、架构、profile 和运行时契约执行失败关闭校验。
- 修正 Godot 4.4 兼容门禁：仅允许由 GDPP 新语法触发且精确匹配的宿主内置 parser 提示，所有其他导入、导出、运行时与 PCK 诊断仍严格失败。
- 新增 Godot 4.4 真实原生压力测试，覆盖引用型值自赋值及动态、强类型、静态 Dictionary 的重复释放，并在 macOS、Linux、Windows 独立构建 provider SDK 消费者。
- 使用官方 Godot 4.6.1 复验二进制交付链，覆盖 AOT Autoload、独立第三方 GDExtension 启动、导出包运行，以及源码、compiler、SDK 零泄漏。

## 1.7.0

- 新增 GDScript 继承独立第三方 GDExtension 类型的零源码改动 AOT 支持，无需重新构建、重新链接或修改供应商插件。
- 引入基于 `ScriptLanguageExtension`、`ScriptExtension` 与 `ScriptInstance` 的附着式 AOT 后端：原生 Object 仍由供应商创建和持有，生成的 C++17 行为负责脚本字段、方法、属性、信号、通知和 RPC 元数据。
- 保留附着式原生根类之上的脚本继承，包括字段初始化、`_init`、方法/属性分派、虚回调、信号行为，以及 RPC 配置的继承与覆盖。
- 外部 `super` 调用使用供应商反射出的 MethodBind 精确兼容哈希和稳定 GDExtension ABI，不再采用不安全的跨动态库 C++ 继承或猜测签名。
- 扩展 ClassDB 采集与 `gdpp_bridge.json` 校验，纳入供应商身份、精确可调用元数据、与加载顺序无关的供应商解析、缓存失效，并在关键反射缺失时失败关闭。
- 二进制导出期间把场景、独立资源、内嵌资源和 Autoload 转换为附着式编译脚本，同时保留供应商原生类型与序列化状态。
- 导出游戏逐字节保留第三方描述符和目标平台动态库，provider-first 与 project-first 两种注册表顺序均可安全启动，不要求客户项目或供应商源码改动。
- 加固 macOS Universal 2 导出：事务式规范化供应商扫描描述符、逐 Mach-O 切片验证、崩溃恢复，并在导出后恢复所有源描述符。
- 新增完全独立的双 GDExtension fixture，并为附着式交付链建立 Godot 4.4～4.7 真实构建、加载、运行、导出、PCK 内容、二进制架构与零源码泄漏门禁。
- 发行 SDK 升级至 schema 6 / runtime ABI 7，对宿主、桌面、移动与 Web 目标中的全部附着式运行时头文件和源文件执行摘要校验。

## 1.6.0

- 完成 Godot Variant 完整类型域、可空性模型和零值真值语义，并贯通语义分析、类型化 HIR、生成的 C++17 与原生运行时。
- 集中定义数值、字符串、内建值、PackedArray、对象、Ref、RID 和容器的严格赋值、显式转换、分析器兼容、运行时可构造与原生存储规则。
- 支持 `Array[T]` 与 `Dictionary[K,V]` 参数化 `as` 目标、跨脚本限定枚举转换目标、常量严格转换检查和运行时受检转换；对原生存储无法保持的确定性 String/RID 路径在生成前失败关闭。
- 强类型容器存储改为精确校验运行时元素、键、值、对象类和脚本元数据，不再依赖 godot-cpp 的隐式转换构造器，同时保持 GDScript 分析器对未约束容器和 PackedArray 边界的接受行为。
- 为必然失败的常量转换、直接违反不变规则的强类型容器、确定性运行时转换失败及 Object 背书 RID 等原生存储不可表示路径增加稳定诊断。
- 扩展生成代码严格编译与 Godot 4.7.1 GDScript/AOT 真实差分，覆盖全部真值类别、内建转换、PackedArray、强类型容器元数据恢复、String 转换分歧和 RID 存储行为。
- 隔离嵌套警告控制作用域并按固定格式化器统一分析器源码，保持 Clang、GCC 与 MSVC 发布门禁下的警告即错误构建。
- 严格类型改造暴露限定枚举与运行时容器边界回归后，恢复固定节奏游戏和角色扮演游戏项目的完整编译。
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
- 扩充严格生成 C++ 编译和真实 Godot 4.7 GDScript/AOT 差分，覆盖类型收窄、Callable ABI、语言工具、注解、静态上下文和求值顺序。

## 1.4.0

- 将前端常量求值、HIR 优化、生成的强类型 C++ 与动态运行时整数运算统一到同一套可移植 64 位契约。
- 明确定义回绕溢出、归一化移位、有符号除余边界、复合赋值和防溢出的原生范围终止，不再触发 C++ 未定义行为。
- 将发行 SDK 升级到 schema 5 / runtime ABI 4，为所有宿主和导出目标打包并校验共享整数语义头的哈希。
- 新增真实 Godot GDScript/AOT 整数差分、严格生成 C++ 编译，以及阻断式 Linux UBSan 核心流水线。
- 加固 Chromium 交付验证，可靠处理异步 profile 落盘竞态，同时保留严格的清理失败报告。

## 1.3.0

- 完成浮点计数及 Vector2/Vector2i、Vector3/Vector3i 数学范围的原生 `for` 循环支持，方向、步长和边界行为与 Godot 保持一致。
- 新增基于 Godot `_iter_init`、`_iter_next`、`_iter_get` 协议的静态对象迭代支持，在编译期验证协议并保留迭代结果类型。
- 强化类型化循环安全性，对数组和字典字面量执行上下文类型化，精确诊断非法元素、键和循环变量声明。
- 扩充 Godot 4.7 官方兼容门禁及真实 Godot GDScript/AOT 差分测试，覆盖类型化、数学范围、容器和自定义对象迭代。

## 1.2.0

- 新增强类型数组与字典的完整原生支持，在语义分析、跨脚本接口和生成的 C++ 中保留元素、键与值类型契约。
- 完善强类型容器安全检查，覆盖赋值、参数、返回值、导出属性、嵌套容器及项目依赖边界。
- 优化生成代码，直接构造原生强类型容器字面量并消除冗余包装拷贝，降低分配与转换开销。
- 扩充编译器、工程与 Godot 差分测试，覆盖强类型容器行为、ABI 稳定性、依赖失效和运行性能。

## 1.1.0

- 大幅提升最新版 GDScript 兼容性，完善字符串、数字、Unicode 标识符、局部常量、尾随逗号和 lambda 等语法支持。
- 完整支持数组、字典、嵌套及剩余项 `match` 模式，并贯通类型检查、优化与原生代码生成。
- 完善 `await` 与协程编译，支持返回值、立即完成、并发调用及继承场景下的动态分派。
- 强化多脚本工程编译，改进类型与调用关系分析，实现更精确的依赖失效和增量重建。
- 提升编译器安全性与诊断质量，加入 Unicode 关键字防伪、非法输入处理及前端资源上限保护。
- 改善跨平台导出兼容性，继承 Godot 官方模板设置，并加入固定 Godot 4.7 官方语料回归测试。

## 1.0.0

- 首个版本，提供 GDScript AOT 编译器和 Godot 编辑器插件。
