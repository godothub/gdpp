# Godot API 元数据

GDPP 以 Godot 4.4 为最低运行版本，并随编译器内置 4.4、4.5、4.6、4.7 四套稳定版
`extension_api.json` 快照。最新稳定 GDScript 语法由 GDPP 自己解析；目标 Godot 版本只决定可用
引擎 API 与 ABI，不能因为前端接受高版本语法就调用低版本引擎不存在的方法。

当前发布门禁使用以下官方补丁版本：

| API 系列 | 运行门禁 |
|---|---|
| 4.4 | Godot 4.4.1 |
| 4.5 | Godot 4.5.2 |
| 4.6 | Godot 4.6.3 |
| 4.7 | Godot 4.7.1 |

这些补丁版本是当前可复现基线，不表示未来补丁版本自动获得认证。补丁升级必须重新执行编译、导出、
无源码审计和运行 oracle。

## 生成与查询

`tools/generate_godot_api.py` 在 `build/<preset>/generated/` 生成确定性 C++ 表，索引：

- 全局作用域、单例和继承关系；
- 引擎类、内建类、构造器、方法和操作符；
- 方法参数、默认值、返回值、`const`、`static` 和 `vararg`；
- 属性及其 getter/setter；
- 虚函数、枚举、位域、常量和 utility function；
- 版本特定的 native structure 与类型元数据。

四套表一起编译进 compiler 插件。发行用户不需要 Python，也不会在导出时重新生成数据库。
语义分析只查询本次导出的目标版本，并把已解析的调用写入类型化 IR：

```cpp
set_position(get_position() + delta);
godot::Input::get_singleton()->is_action_pressed("ui_accept");
godot::Color::html("ff8800");
```

静态可证明的调用直接生成 godot-cpp API；动态接收者、动态方法名或需要保留 GDScript 错误语义的
调用进入集中式 Variant/runtime 路径。不能解析的 API 在生成 C++ 前给出确定性诊断，不允许等到
客户机器的 C++ 编译器报模板错误。

## 版本隔离

目标 API 版本进入：

- 项目内容哈希和增量 manifest；
- 原生对象缓存目录与构建身份；
- SDK manifest 和 runtime ABI 校验；
- 生成项目 `.gdextension` 的 `compatibility_minimum`；
- 导出摘要与诊断。

因此不同 Godot 系列不能错误复用对象，也不能让 4.7 API 静默泄漏到 4.4 目标。SDK schema、
runtime ABI、API 版本、平台、架构、profile 或 binding 摘要任一不匹配，构建必须在编译前失败。

## 官方 API 之外的边界

两个多宿主正式包认证官方标准精度 Godot 与包内匹配的 godot-cpp。下列情况不能直接复用标准 SDK：

- `precision=double` 的自定义引擎；
- 修改过 `extension_api.json` 或 GDExtension ABI 的引擎；
- 自定义模块新增但未注册到 ClassDB 的接口；
- 依赖引擎私有头、私有符号或内存布局的扩展；
- 只在第三方二进制内部存在、没有 ClassDB/Variant 契约的 API。

第三方 GDExtension 只要把类、方法、属性、Signal 和枚举正确注册到 ClassDB，GDPP 就能通过附加
运行时桥接保持原项目调用方式；它不需要修改自身源码。未注册的私有 C++ API 不属于 GDExtension
公共契约，任何 ScriptLanguageExtension 也无法可靠发现。

仓库提供独立 custom SDK 生产流程。配置时传入
`GDPP_CUSTOM_GDEXTENSION_API_FILE=<目标引擎导出的 extension_api.json>`，并让
`GDPP_GODOT_PRECISION` 与目标的 `single`/`double` 一致；构建只允许打包该 API 所属的一个
Godot minor。编译器、editor 插件、fallback、godot-cpp `template_release`、生成 API 表和
runtime 使用同一输入，不允许把标准包的任一静态库混入。

custom 包与 SDK manifest 固定：

- 完整 API 文件 SHA-256、Godot major/minor 和 precision；
- SDK schema、runtime ABI、godot-cpp binding profile 和优化配置；
- compiler/fallback 与 descriptor 的 exact precision feature；
- 唯一匹配 precision 的 editor 和 `template_release` 二进制，不接受另一精度的文件。

CI 会从一个 4.7 double API 干净构建完整 custom add-on，并用独立审计器检查上述字段、文件名、
descriptor、插件版本和运行时哈希。客户定制引擎仍需要针对其实际平台重新执行同等级门禁；
“流程已产品化”不表示标准发行 ZIP 能直接加载任意定制引擎，也不允许手工替换 JSON 或静态库。
