# 平台验证报告

## 基线

| 项目 | 值 |
|---|---|
| GDPP | 1.8.3 |
| 功能审计范围 | 1.8.3 当前发布候选 |
| 最近正式发布运行 | 1.8.2 / `https://github.com/abandoft/gdpp/actions/runs/30311849463` |
| 1.8.2 发布状态 | 已发布；48 个正式发布作业全部成功 |
| 1.8.2 已发布资产 | `gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip`、`SHA256SUMS` |
| 当前目标发行资产 | `gdpp.zip`、`gdpp-all.zip`、`SHA256SUMS` |
| 本地编译器单元测试 | 560 / 560 |

本报告只描述可重复证据。内部商业语料和客户项目不按名称公开；它们只能补充发现问题，不能替代
产品级 fixture 与 CI。

## 1.8.3 发布前本地验证

| 门禁 | 结果 |
|---|---|
| 开发 core CTest | 24 项发布前合同 |
| 开发 plugin CTest | 24 项发布前合同 |
| 编译器单元 | 560 / 560 |
| godot-cpp SDK | macOS 上完整重建 4.4、4.5、4.6、4.7 `template_release` |
| 官方 Godot 4.7.1 直接构建 | 当前 compiler 生成、顺序编译并链接真实客户项目库成功 |
| 官方 Godot 4.7.1 AOT runtime | 重新生成 Universal 2 成品后，arm64 与 Rosetta x86_64 的 FunctionState、异步虚函数、协程 lambda、await 默认参数、协程访问器、Callable/Signal、全 Variant fault 和项目脚本生命周期成功 |
| 官方 Godot 4.7.1 fault 差分 | 清缓存 source/AOT 的 65 个调用者恢复序列、39 项值矩阵哈希、await 默认参数及协程访问器结果完全一致；覆盖 Dictionary 缺键、`null`、强类型直接/复合写入、只读写入及已证明整数槽位除零 |
| 官方 Godot 4.7.1 性能 | 13/13 family、启动和固定帧全部通过 10% 门禁；最终 5 轮候选中 Dictionary AOT -38.57%、String AOT -6.87%、Variant AOT -28.74% |
| 官方 Godot 4.5.2 AOT runtime | Universal 2 Release 导出后连续独立运行 10 次；动态 Script、Attached provider 与真实多 peer RPC 全部干净退出 |
| custom/double add-on | 4.7 double 从精确 API 干净构建；compiler、SDK、descriptor、静态库和 manifest 审计成功 |
| Windows DLL 装载边界 | Windows 11 / MSVC 19.50 同机探针对未修复 compiler 复现 `LoadLibraryExW` 1114，对当前 compiler 验证装载、`gdpp_library_init` 导出和卸载均成功 |
| 官方 Godot 4.6.1 Windows 客户流程 | 清缓存安装、导入、Release AOT、原生编译、链接、导出及成品独立运行成功；嵌套终止分支不再误报 `GDS5118` |
| Windows 回环协议 | HTTP/WebSocket/protobuf、双礼物平台、远程头像、扩展事件、97 包突发、正常关闭与损坏消息均退出 0；对象池总数保持 5000 |
| Windows 成品 PCK | 373 个文件、42 个资源可加载、1 个项目库、0 个源码/SDK/compiler/中间产物违规 |
| 官方 Godot 4.6.2 Release | Universal 2 Attached provider 导出、独立运行成功 |
| 官方 Godot 4.6.2 Debug | Universal 2 Attached provider 导出、独立运行成功 |
| PCK 审计 | Debug/Release 均 19 个文件、2 个转换场景、1 个转换资源、0 违规 |
| 源工程不变性 | compiler/provider 描述符及 extension registry SHA-256 导出前后相同 |

4.6.2 两种 profile 的独立运行都输出 `GDPP_ATTACHED_EXPORT_RUNTIME_OK`；当前 4.7.1 AOT
故障/Callable/Signal oracle 输出 `GDPP_CALLABLE_SIGNAL_RUNTIME_OK`，FunctionState 差分输出
`GDPP_FUNCTION_STATE_RUNTIME_OK`。await 默认参数和协程访问器另输出
`GDPP_AWAIT_DEFAULT_AOT_RUNTIME_OK`、`GDPP_COROUTINE_ACCESSOR_AOT_RUNTIME_OK`。这组本地证据
用于在正式矩阵前验证 runtime ABI 23、breakpoint、静态与 lambda 协程、异步虚函数、严格存储、
故障隔离、项目脚本 `Object.free()` 和生命周期语义；
它不替代下述跨 runner 发布门禁。

两个 AOT oracle 均在独立导出进程中通过 `OS.get_cmdline_user_args()` 选择。标准 Godot
release 模板默认关闭 path overrides 并忽略 `--script`，因此发布门禁不把官方模板明确禁用的
CLI 入口误当成 GDPP 语义；每个 oracle 仍独占进程、退出码、无诊断日志与精确成功标记。

本轮发布提交还在同一官方 4.7.1 引擎下分别运行源码 `runtime_fault_oracle.gd` 和导出的 Universal 2
成品。两端的 `GDPP_FAULT_MATRIX` 与 `GDPP_VALUE_MATRIX` 逐字一致；AOT 另在独立进程输出
`GDPP_AWAIT_DEFAULT_AOT_RUNTIME_OK` 和 `GDPP_COROUTINE_ACCESSOR_AOT_RUNTIME_OK`。性能矩阵
使用同一引擎/模板、5 轮 AB/BA、每轮 5 个样本、每个 family 10,000 次迭代，行为 oracle 先于
性能判定通过。协程 fault 检查器生命周期修复后又从当前源码重建项目库与成品，普通运行在
arm64、Rosetta x86_64 均输出 `GDPP_DYNAMIC_SCRIPT_RUNTIME_OK`、`GDPP_RPC_RUNTIME_OK` 和
`GDPP_ATTACHED_EXPORT_RUNTIME_OK`，随后三个独立 AOT oracle 再次通过。

最终 fault polling、Dictionary named/keyed ABI、已证明槽位借用、精确整数槽位原地运算、
Variant 边界借用和单次 RHS 快照移动版本又清理生成物、对象和项目库，使用官方 4.7.1 Universal
2 Release 模板重建 AOT 与 GDScript 对照成品，并执行 5 轮 AB/BA、每轮 5 个样本、每个 family
10,000 次迭代。行为 oracle 先通过，随后 13/13 family、启动与固定帧门禁全部通过；Dictionary、
String、Variant 的 AOT mean 相对 GDScript 分别为 -38.57%、-6.87%、-28.74%。独立 provider
成品的 65 组 source/AOT fault 序列逐字一致，覆盖缺失点号键、合法 `null`、只读直接/复合写入、
非法强类型直接值、已证明整数槽位除零，以及官方会保持原值并继续的非法强类型复合值。

## 正式发布门禁

### 编译器与宿主

| 门禁 | 环境 | 验证 |
|---|---|---|
| Compiler core | Ubuntu 22.04、macOS 15、Windows 2025 | C++17、严格 warning、560 项单元 |
| ASan | Ubuntu 22.04 | 地址错误和 leak 阻断 |
| UBSan | Ubuntu 22.04 | 未定义行为阻断 |
| TSan | Ubuntu 22.04 | 线程数据竞争阻断 |
| Native plugin | 三桌面 runner | compiler GDExtension、SDK、直接项目构建、进度模型；Windows 另验证 ABI 前 DLL 装载/卸载 |
| Quality | Ubuntu 24.04 | 架构、格式、workflow、固定 Action SHA、Node.js 24 MSVC action |

开发 core CTest 当前 24 项；启用 plugin 的本地 CTest 当前 24 项。部分兼容语料只在 core
preset 注册，Godot editor 服务只在 plugin preset 注册；这里的 CTest 项目会各自运行大量内部
断言，不能把“24 项 CTest”误写成“只有 24 个测试”。

### Godot 版本

| Godot | 运行门禁 |
|---|---|
| 4.4.1 | Linux compiler/plugin、Release/Debug AOT、GDScript 对照、Attached provider、运行与 PCK |
| 4.5.2 | 同上 |
| 4.6.3 | 同上 |
| 4.7.1 | 同上 |

每个版本：

- 使用对应 minor 的 SDK 和 API 能力表；
- 执行正式 Release AOT 导出、独立运行和无源码审计；
- 以四个独立成品进程验证主场景、fault/value 差分、await 默认参数及协程访问器；预期故障矩阵
  锁定差分结果，其余三个进程逐一审计全量诊断日志；
- 执行优化 Debug 导出并保留脚本调试语义；
- 导出 GDScript fallback 并运行固定行为/性能 oracle；
- 执行独立 provider + Attached 项目；
- 缺失 SDK 时验证严格二进制导出会失败关闭；
- 导出后验证源工程描述符和注册表未被修改。

4.4 自身 parser 对少数较新 `.gd` 写法会输出已知诊断。门禁只允许精确文本和精确源码行，并仍
要求 GDPP AOT 编译、导出、运行和其余日志全部成功。普通客户 `.gd` 的正式路径仍应被目标编辑器
接受。

### 桌面包

| 宿主 | 构建 | 真实导出/运行 | 二进制约束 |
|---|---|---|---|
| macOS | 4.4～4.7 Universal SDK + compiler | 官方 Godot 4.7.1 Universal 2 | arm64+x86_64、macOS 11.0 |
| Linux | 4.4～4.7 x86_64 SDK + compiler | 官方 Godot 4.7.1 | glibc ≤ 2.35 |
| Windows | 4.4～4.7 x86_64 SDK + compiler | 官方 Godot 4.7.1 | MSVC、Windows 10、静态 CRT |

三端均从 host component 实际导出、运行普通 oracle和 4996 项协程循环 oracle。默认
`gdpp.zip` 还会分别在 macOS、Linux、Windows 安装到全新工程，再重复导入、导出、运行、库唯一性和 PCK 审计；这能发现
“构建目录可用但发行包缺文件”的问题。

Windows compiler 还必须在未调用 `gdpp_library_init` 时通过 `LoadLibraryExW`，暴露正确入口后
可由 `FreeLibrary` 卸载。该门禁专门阻止包含 Godot 对象的静态/TLS 构造在 godot-cpp 取得引擎
接口前运行；仅依赖 Godot 编辑器后续报错不足以定位这一类 DLL attach 失败。

### Android

| 门禁 | 状态 |
|---|---|
| Godot 4.4～4.7 arm64 SDK | 成功 |
| `template_release` 唯一性、API 28、`c++_shared`、路径映射 | 成功 |
| 官方 Godot 4.5.2 Release APK | 成功 |
| 项目库 ELF、唯一入口、包内容和 `.gd` 剥离 | 成功 |
| 真机安装/输入/音频/前后台/低内存 | 未认证 |
| AAB、ABI split、商店签名、x86_64 | 未交付 |

“APK 构建成功”不能替代设备运行认证。

### iOS

| 门禁 | 状态 |
|---|---|
| Godot 4.4～4.7 device arm64 + Universal Simulator SDK | 成功 |
| iOS 16.0、路径映射、唯一 `template_release` | 成功 |
| Godot 4.6.2 无源码 Xcode 导出 | 成功 |
| 官方模板可用架构的无签名 Simulator build | 成功 |
| 独立 Universal 2 provider macOS 导出/运行 | 成功 |
| 签名真机、前后台、Archive、TestFlight/App Store | 未认证 |

### Web

| 门禁 | 状态 |
|---|---|
| Godot 4.4～4.7 `threads`/`nothreads` SDK | 成功 |
| Godot 4.5.2 两模式 Release 导出 | 成功 |
| Wasm validate、`dylink.0`、唯一入口、shared memory 差分 | 成功 |
| PCK/目录无源码和无 SDK | 成功 |
| Chromium + COOP/COEP + DOM 行为 oracle | 两模式成功 |
| Safari、Firefox、移动浏览器、真实 CDN | 未认证 |

旧文档把 Chromium CI 写成尚未执行已经过时；当前它是正式发布阻断门禁。

## Attached GDExtension fixture

独立 provider fixture 覆盖：

- provider-owned Node 与 Resource 身份；
- 多级项目脚本继承和外部 `super`；
- `_init`、onready、通知和 provider→script 虚调用；
- provider/script Signal、RPC 和 typed container；
- Node/Resource 序列化；
- 预绑定 ShaderMaterial 与运行时 Shader 参数；
- 网络图片、消息分派和失效对象防护；
- provider 描述符事务恢复和无源码 PCK。

Godot 4.4.1～4.7.1 均执行 Linux 导出/运行；Godot 4.6.2 另执行 macOS Universal 2 provider 路径。
这证明公开 ClassDB/Attached 主路径，不证明所有供应商私有 ABI。

1.8.0 已把包含 `breakpoint` 的普通、静态、lambda、词法遮蔽和 await 脚本与真实 godot-cpp
一同编译。本地使用官方 Godot 4.6.2 完成 Attached provider 的 macOS Universal 2
Debug/Release 无源码导出和独立运行；Release-only provider 由 Debug 导出复用时，源描述符保持
不变。官方 Godot 4.7.1 还实际运行了真正挂起的静态函数、类型化/并发 lambda 协程、创建时捕获
快照、Signal 发射期连接变更、one-shot/deferred/reference-counted 连接、宿主销毁和多线程
Callable 调用；挂起的异步虚函数还覆盖 FunctionState 对象、`completed`、手动恢复、旧连接清理、
有效性和即时 native 返回转换。await 默认参数、实例/静态/内部类属性访问器、动态二进制 Script
加载、全 Variant family fault 及真正的多 peer RPC 已加入产品 fixture 和 CI。编辑器 gutter
断点和完整单步调试尚未列为已认证能力。

## Windows 补充端到端审计

发布前另在 Windows 11 x86_64、官方 Godot 4.6.x、MSVC 环境运行通用联机事件 fixture。该证据
不是公开客户源码，也不是正式 CI 的替代。

### 本地协议服务器

模拟服务覆盖：

- 4 次 JSON HTTP GET；
- 登录 HTTP POST；
- WebSocket 101 握手；
- 头像/网络图片请求；
- 主/子序列二进制客户端包；
- 两种礼物平台消息；
- 点赞、英雄、分数、分组同步；
- 主动关闭、短帧和损坏 protobuf。

### 场景结果

| 场景 | 结果 |
|---|---|
| 正常登录与礼物 | 退出 0，UI/对象变化正确 |
| 重复消息 | 13 个礼物包，累计提示与对象池一致 |
| 扩展事件 | 点赞、英雄、分数、同步、头像均完成 |
| burst | 97 个礼物包，总计 100 次提示，进程稳定 |
| 服务端关闭 | 客户端干净结束 |
| 短帧/损坏 protobuf | 记录受控错误并继续运行 |

对象池批处理使用 4996 个待处理项、每帧 200 个，完成后活动/空闲数符合 oracle。该测试暴露并
修复了异步循环状态提升顺序问题；同一 oracle 已进入单元、Godot 四版本、桌面 host 和最终 ZIP
门禁。

### Shader 与网络图片

补充审计验证：

- 从场景/Resource 预绑定的 ShaderMaterial 保持 Shader 和 uniform；
- 运行时给 Sprite/Control 赋 ShaderMaterial 不丢失 shader；
- 动态 uniform 的 Color/float/texture 经过正确 Variant ABI；
- HTTP 图片解码、ImageTexture 创建和 UI 赋值；
- 网络对象销毁后回调不会访问失效 Object。

这些路径现在有产品 fixture，不再只依赖某个游戏画面人工观察。

### 无源码审计

一次完整导出审计结果：

```text
PCK_AUDIT_FILES=397
PCK_AUDIT_RESOURCES_LOADED=42
PCK_AUDIT_PROJECT_LIBRARIES=1
PCK_AUDIT_VIOLATIONS=0
PCK_AUDIT_UNEXPECTED_ERRORS=0
```

确认没有 `.gd/.gdc`、生成 C++、头文件、静态库、对象或 compiler/SDK 泄漏。

## 性能补充

同一 Windows 机器、相同联机事件和三轮 warm 测量：

```text
AOT median: 5832 ms
GDS median: 6989 ms
AOT / GDS: 0.8345
预算: <= 1.10
```

该结果证明当前 fixture 没有超过“最多慢 10%”的回归预算，不代表所有项目都快 16.55%。正式
性能方法见 [PERFORMANCE.md](PERFORMANCE.md)。

## 已知共享行为

当测试驱动在音频仍播放时立即强制退出，AOT 与原 GDScript 都会留下同样的
`AudioStreamPlaybackWAV`/音频 Resource 退出警告。这是测试结束时序和 Godot 音频生命周期的共享
行为，不是 GDPP 额外引用；正常生命周期不应以强制退出日志代替。

损坏消息 fixture 会发送多个独立坏包。统一 fault frame 与 GDScript 一样中止发生故障的当前
处理函数，外层网络循环仍可接收后续独立事件；源码/AOT oracle 比较 marker、源码位置和调用者
继续执行，不以“进程没崩溃”代替语义验证。

## 尚未认证

- Windows arm64、Linux arm64、Android x86_64；
- Android 真机/AAB/商店，iOS 真机/TestFlight/App Store；
- Web 非 Chromium 浏览器、真实 CDN 和长时内存增长；
- 多进程同时构建、用户取消、断电/低磁盘/只读目录；
- 多小时网络、资源流送、重复场景加载和磁盘增长；
- 新 Godot patch 的逐 API/逐错误组合持续扩展；
- 代码签名、公证、SBOM、provenance 和符号服务。

因此当前结论是“声明矩阵内的发布主路径通过并失败关闭”，不是“全部平台与全部 GDScript 已达
1.0 Stable”。
