# 第三方 GDExtension 兼容

第三方 GDExtension 已经是独立 DLL/SO/dylib/Wasm，由 Godot 按供应商 `.gdextension` 加载和
导出。GDPP 不重新编译它，也不把它合并进项目库。

本页说明普通客户代码如何在不修改脚本、场景、资源或供应商源码的前提下引用和继承第三方原生类。

## 支持范围

| 用法 | 当前状态 |
|---|---|
| 安装/加载/导出第三方 `.gdextension` | Godot 原样处理 |
| `var value: VendorType` | 支持，类型契约从 ClassDB 自动采集 |
| 属性、实例方法、Signal、常量 | 支持，编译期校验后通过 Object/Variant ABI |
| 第三方静态方法 | 支持，使用 `ClassDB.class_call_static()` |
| `VendorType.new()` | 支持，使用 `ClassDB.instantiate()` 并校验可实例化 |
| 第三方 Engine 单例 | 支持，从运行时单例表查询 |
| 命名 enum/bitfield | 支持，保留命名身份、值与 Inspector hint |
| `extends VendorNode` | 支持，使用 Attached Script，不建立跨库 C++ 继承 |
| 脚本继承、外部 `super`、provider 回调 | 支持公开反射契约 |
| Node/Resource/Autoload/场景序列化 | 支持主路径 |
| 无 Godot 进程的 CLI | 使用可审计 `gdpp_bridge.json` |
| 私有/未注册 C++ API | 不支持，失败关闭 |

## 为什么不能直接生成 C++ 子类

`extends VendorNode` 的 GDScript 语义是：

1. Godot/供应商工厂创建真实 `VendorNode`；
2. GDScript `ScriptInstance` 附着到该对象；
3. 字段、方法和脚本虚调用来自 ScriptInstance；
4. 原生生命周期和对象身份仍由 `VendorNode` 提供。

如果 GDPP 在另一个动态库中生成 `class ProjectType : public VendorNode`，就要求供应商 C++ 头文件
和链接库，并进入 Godot 尚不支持的跨 GDExtension 原生继承边界。即使 C++ 能链接，也无法安全
保证扩展实例层、虚调用、销毁和热重载。

GDPP 因此复现 GDScript 的附着模型，而不是伪造 C++ 继承。

## Attached 实现

```text
供应商动态库
  -> 注册 VendorNode / VendorResource
  -> Godot 创建真实对象

GDPP 项目动态库
  -> 注册 AttachedScriptLanguage
  -> 注册每个脚本的 behavior descriptor
  -> AttachedCompiledScript / AttachedScriptInstance
  -> 把生成行为附着到真实对象
```

运行规则：

- 生成字段、方法、属性、Signal、RPC 和脚本继承由 behavior 提供；
- provider 属性、Signal、通知和生命周期仍在同一个真实对象上；
- provider 从 C++ 触发脚本虚方法时进入 Attached dispatch；
- `super.method()` 使用 ClassDB 采集的精确 MethodBind compatibility hash；
- 项目脚本继续在 Attached behavior 链上多级继承；
- Node 与 Resource 的脚本状态按 Godot 存储属性规则序列化；
- 项目库注册行为时不要求 provider 已先加载，真正实例化时才解析目标原生类；
- 目标类缺失或契约冲突会报告确定错误，不创建半附着实例。

项目库标记为不可热重载，避免现有 ScriptInstance 跨动态库世代悬挂。

## ClassDB 自动采集

编辑器内编译时，compiler 在主线程从 ClassDB 采集扩展 API：

- `API_EXTENSION` 与 `API_EDITOR_EXTENSION` 类；
- 完整扩展继承链和最近的官方 Godot 父类；
- 可读/可写属性及 getter/setter 补充类型；
- 实例/静态方法、参数、返回、默认参数、vararg；
- Signal、整数常量、命名 enum 与 bitfield；
- Object 具体类型和 enum owner；
- MethodBind compatibility hash；
- editor-only 标记；
- 每个类的确定性契约哈希。

无法可靠表达的 Variant 值保守保持 Variant。GDPP compiler 自身和陈旧 `GDPPNative_*` 类不会
进入客户 bridge。

快照只在内存中交给 ProjectCompiler。后台编译不访问实时 ClassDB。实际引用的契约身份进入
`addons/gdpp/build/project/bridge.lock` 和项目缓存；未使用供应商类的变化不会使所有脚本失效。

## editor-only 与 runtime

编辑器扩展类可以参与工具脚本，但不能进入发行 runtime 图。编译结果标记 editor-only 脚本，
导出器会阻断：

- runtime Autoload 使用 editor-only 类；
- runtime 场景附着 editor-only 脚本；
- runtime Resource 图引用 editor-only 脚本；
- 需要 editor provider 才能实例化的项目行为。

这避免在编辑器中能编译、成品中 provider 不存在时才崩溃。

## 离线 Runtime 清单

CLI 或交叉构建无法加载当前宿主的供应商库时，可使用 schema 1 JSON：

```json
{
  "schema": 1,
  "provider": "Vendor.gdextension",
  "abi": "vendor-2.4.1+sha256-...",
  "godot_minimum": "4.4",
  "classes": [
    {
      "gdscript_name": "VendorData",
      "godot_base": "RefCounted",
      "mode": "runtime",
      "members_complete": true,
      "properties": [
        {"name": "sample_count", "type": "int", "read_only": true}
      ],
      "methods": [
        {
          "name": "sample",
          "return_type": "float",
          "parameters": [{"name": "index", "type": "int"}]
        }
      ],
      "signals": [{"name": "changed", "parameters": []}],
      "enums": [
        {
          "name": "ChannelFlags",
          "bitfield": true,
          "values": [
            {"name": "LEFT", "value": 1},
            {"name": "RIGHT", "value": 2}
          ]
        }
      ]
    }
  ]
}
```

`members_complete=true` 时，未知成员和只读写入在编译期拒绝；`false` 只校验已知成员，其余保守
走动态路径。enum 值按精确 int64 解析，不经过 double。重复类/成员/enum、非法类型、空 ABI、
错误最低 Godot 或越界值会使整个清单事务失败。

清单不包含供应商头文件或链接输入，也不改变供应商加载方式。

## enum 与 bitfield

```gdscript
var channel: VendorData.Channel = VendorData.Channel.LEFT
var same: VendorData.Channel = VendorData.LEFT
var flags: VendorData.ChannelFlags = (
    VendorData.ChannelFlags.LEFT | VendorData.ChannelFlags.RIGHT
)
```

生成代码使用稳定 `int64_t` 存储 GDScript enum，不依赖供应商 C++ enum 大小、命名空间或头文件。
参数/返回在 Variant ABI 边界转换。不同命名 enum 仍保持不同语义身份。

## 导出事务

供应商描述符和二进制保持供应商所有。GDPP 只在 Godot 某些 Universal 2 扫描场景需要时：

1. 读取并备份原始描述符字节；
2. 用 `lipo` 验证供应商 dylib 实际包含目标切片；
3. 临时写仅供扫描的 universal 条目；
4. PCK 写回供应商原始描述符；
5. 成功、失败或下次启动恢复源文件。

项目库不复制供应商二进制，不更改签名和升级边界。

## 当前验证

独立 fixture 使用两个完全分离的 GDExtension：

- provider 注册 Node 与 Resource；
- GDPP 项目库只包含生成 Attached behavior；
- 多级脚本继承、字段、`_init`、onready、通知、provider→script 回调；
- script→provider `super`、原生/脚本 Signal、RPC；
- typed container、Node/Resource 序列化；
- 预绑定和运行时 ShaderMaterial；
- 网络图片、消息分派和失效对象防护；
- 无源码导出、PCK 内容和 provider 描述符恢复。

正式 CI 在 Godot 4.4.1、4.5.2、4.6.3、4.7.1 上构建、导出并运行该 fixture；iOS 另执行无签名
Simulator Xcode 路径。更多真实供应商、移动真机和全部平台升级组合仍是认证缺口。

## 已知边界

1. 只兼容公开注册到 Godot ABI 的能力；私有 C++ 方法、未注册回调和自定义内存布局不可见。
2. MethodBind hash 能锁定调用契约，但 provider 目标动态库的实际文件 SHA-256 尚未统一进入导出
   锁。采集后替换供应商库的场景需要进一步失败关闭。
3. editor-only/runtime 域已经区分，仍缺更多供应商和平台矩阵。
4. provider 升级后若公开契约变化会使消费者重编；完整版本兼容/迁移报告尚未提供。
5. 供应商许可证是否允许编译其 `.gd` 和分发生成二进制，由客户合同决定。
