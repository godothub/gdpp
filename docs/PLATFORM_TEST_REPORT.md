# 平台验证报告

## 基线

| 项目 | 值 |
|---|---|
| GDPP | 1.7.10 |
| 提交 | `d77fb4629ce860bfb382f6198b5d09372cb7d2ae` |
| 正式发布运行 | `https://github.com/abandoft/gdpp/actions/runs/30170732292` |
| 结果 | 46 个作业成功，0 失败 |
| 发行资产 | `gdpp-mac.zip`、`gdpp-linux.zip`、`gdpp-win.zip`、`SHA256SUMS` |
| 编译器单元测试 | 488 / 488 |

本报告只描述可重复证据。内部商业语料和客户项目不按名称公开；它们只能补充发现问题，不能替代
产品级 fixture 与 CI。

## 正式发布门禁

### 编译器与宿主

| 门禁 | 环境 | 验证 |
|---|---|---|
| Compiler core | Ubuntu 22.04、macOS 15、Windows 2025 | C++17、严格 warning、488 项单元 |
| ASan | Ubuntu 22.04 | 地址错误和 leak 阻断 |
| UBSan | Ubuntu 22.04 | 未定义行为阻断 |
| TSan | Ubuntu 22.04 | 线程数据竞争阻断 |
| Native plugin | 三桌面 runner | compiler GDExtension、SDK、直接项目构建、进度模型 |
| Quality | Ubuntu 24.04 | 架构、格式、workflow、固定 Action SHA、Node.js 24 MSVC action |

开发 core CTest 当前 17 项；启用 plugin 的本地 CTest 当前 20 项。这里的 CTest 项目会各自运行
大量内部断言，不能把“20 项 CTest”误写成“只有 20 个测试”。

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

三端均从 host component 实际导出、运行普通 oracle和 4996 项协程循环 oracle。macOS/Windows
还把最终生成的 ZIP 安装到全新工程，再重复导入、导出、运行、库唯一性和 PCK 审计；这能发现
“构建目录可用但发行包缺文件”的问题。

Linux 最终 ZIP 目前有完整结构/内容测试和 host component 实跑，但没有独立的“解压最终 ZIP 到
干净工程”作业；这是发布流程仍可补齐的对称性缺口。

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

损坏消息 fixture 会发送多个独立坏包。AOT runtime 采用 fail-soft：每个坏包记录错误并返回安全
默认值，后续事件继续执行；原 GDScript 可能在首个属性错误后终止当前处理函数。该差异是已知
错误策略边界，不能把“错误条数不同”误判为重复消息分派。

## 尚未认证

- Windows arm64、Linux arm64、Android x86_64；
- Android 真机/AAB/商店，iOS 真机/TestFlight/App Store；
- Web 非 Chromium 浏览器、真实 CDN 和长时内存增长；
- 多进程同时构建、用户取消、断电/低磁盘/只读目录；
- 多小时网络、资源流送、重复场景加载和磁盘增长；
- 全语言逐 API/逐错误路径差分；
- 代码签名、公证、SBOM、provenance 和符号服务。

因此当前结论是“声明矩阵内的发布主路径通过并失败关闭”，不是“全部平台与全部 GDScript 已达
1.0 Stable”。
