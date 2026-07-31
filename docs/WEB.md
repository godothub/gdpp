# Web 平台支持与交付

GDPP Web 后端在桌面 Godot 编辑器中完成预编译，再由 Emscripten 生成 wasm32 GDExtension side
module。浏览器只加载 Godot 主 Wasm、项目 PCK 和 `libgdpp.*.wasm`；compiler、SDK、
godot-cpp、生成 C++、对象文件和项目 `.gd` 都不得进入成品。

当前实现同时支持 `nothreads` 与 `threads`，并通过正式发布流水线的 Chromium 运行门禁。Safari、
Firefox、移动浏览器和真实 CDN 尚未形成认证矩阵，不能由 Chromium 结果外推。

## 支持矩阵

| 能力 | 状态 |
|---|---|
| Godot 4.4～4.7 Web SDK | 发布包内置、矩阵校验 |
| `nothreads` wasm32 side module | 支持 |
| `threads` wasm32 side module | 支持 |
| Debug/Release 项目语义 | 支持；两者均链接 release binding |
| 官方 dlink 导出模板 | 强制 |
| 无源码 PCK 与完整目录审计 | 发布阻断门禁 |
| Wasm 入口、`dylink.0` 和共享内存属性 | 发布阻断门禁 |
| Chromium 导出、HTTP 启动与行为 oracle | Godot 4.5.2 发布阻断门禁 |
| Safari、Firefox、移动浏览器 | 未认证 |
| SIMD、WebGPU、PWA 离线与自定义内存策略 | 未认证 |

## 安装布局

`gdpp.zip`、`gdpp-all.zip` 和 `gdpp-lite.zip` 都包含 Web release SDK；前两个包含三种桌面编辑器插件，lite 包只包含 macOS 和 Windows 编辑器插件。用户将所需版本范围的 ZIP 解压到项目根目录，形成 `addons/gdpp/`，不需要再下载 Web target pack。SDK 同时携带 threads 与 nothreads 的 Release 静态 binding，不携带预构建的客户项目 Wasm；一次导出只生成与当前预设线程模式匹配的一个项目 side module。

每个 Godot 版本的 Web 文件与三种桌面、Android、iOS 文件共用 SDK 根：

```text
addons/gdpp/sdk/<godot-version>/
├── godot-cpp/
├── include/
├── lib/
│   ├── libgodot-cpp.web.template_release.wasm32.a
│   └── libgodot-cpp.web.template_release.wasm32.nothreads.a
├── manifests/
│   ├── web.wasm32.threads.sdk.manifest
│   └── web.wasm32.nothreads.sdk.manifest
└── src/
```

平台子目录只在 SDK 生产阶段存在；打包汇总会验证内容并合并到共享 `lib/`、`manifests/`
中，避免复制公共头文件与 runtime 源码。

用户仍需安装与目标 Godot 匹配的官方 Web 导出模板和 Emscripten。项目设置
`gdpp/build/emscripten_cxx` 默认查找 `em++`，也可设置绝对路径。客户无需安装 CMake、
Ninja、Python 或 SCons；插件内置的 Ninja 调度 Emscripten 编译和链接，Windows 的
`.bat/.cmd` 启动器由隐藏的命令处理器包装。

## 导出前提

Godot Web 预设必须满足：

- 启用 `Variant > Extensions Support`，使用官方 dlink 模板；
- 线程模式与服务器部署一致；
- 商业无源码预设保持 `gdpp/strip_gdscript_sources=true`；
- 保持 `gdpp/allow_source_fallback=false`，失败时不得静默交付脚本；
- Emscripten 版本与目标 SDK manifest 契约兼容。

GDPP 根据 Godot 导出 feature 选择 `threads` 或 `nothreads`，并把线程模式写入构建身份、对象
缓存和 `.gdextension` 库条件，不允许交叉复用。

最终项目模块位于：

```text
addons/gdpp/binary/libgdpp.<debug|release>.web.wasm32.<threads|nothreads>.wasm
```

一次导出只预编译当前 profile 和线程模式。编辑器不会构建或加载 Web 模块作为宿主桥接。

## 线程版服务器契约

两种模式不是同一个 ABI：

| 模式 | 静态 binding | Wasm 内存 | 部署要求 |
|---|---|---|---|
| `nothreads` | `*.wasm32.nothreads.a` | 普通导入内存 | 标准 HTTPS/HTTP |
| `threads` | `*.wasm32.a` | `shared` 导入内存 | cross-origin isolation |

线程版必须返回 COOP/COEP；被嵌入 iframe 时，父页面也必须满足隔离要求。测试服务器
`tools/serve_web.py` 固定发送：

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
Cross-Origin-Resource-Policy: cross-origin
Cache-Control: no-store
```

这只是一套 CI 配置，不替代生产 CDN。线上还需验证 MIME、压缩、范围请求、缓存更新、Service
Worker、跨域资源、HTTPS 与回滚策略。无论是否启用线程，GDExtension Web 导出都不能通过直接
双击 HTML 运行。

## 构建、增量与安全

Ninja 工作目录按以下维度隔离：

```text
addons/gdpp/build/project/native-direct/
└── <godot>/web/wasm32/<threads|nothreads>/<debug|release>/
```

构建身份包含 Godot API、SDK schema、runtime ABI、Emscripten 可执行文件、工具链身份、线程
模式、profile、生成源摘要和 binding 摘要。任一变化都会使不兼容对象失效。

Web Release 启用优化、section GC、路径映射和符号裁剪。导出门禁检查：

- PCK 中不存在 `.gd/.gdc`；
- 成品中不存在 compiler、SDK、godot-cpp、静态库、生成 C++ 和对象；
- side module 存在 `dylink.0` 与 `gdpp_library_init`；
- `threads` 导入 shared memory，`nothreads` 不导入 shared memory；
- 二进制不存在客户工程或构建机绝对路径；
- 描述符只引用当前模式的一份 `libgdpp.*.wasm`。

## 维护者构建与门禁

维护者可生成单个目标矩阵：

```sh
cmake --preset dev \
  -DGDPP_WEB_SDK_VERSIONS=4.5 \
  -DGDPP_WEB_VARIANTS='nothreads;threads' \
  -DGDPP_WEB_EMCMAKE=/absolute/path/to/emcmake
cmake --build --preset dev --target gdpp_web_sdk --parallel
```

Godot 4.4 SDK 使用 Emscripten 3.1.62 系列，4.5 及以上使用 4.0.0+。SDK 构建脚本会拒绝明显
不匹配的版本，并把自身中间产物留在根目录 `build/`。

`ci/.github/workflows/web.yml` 当前执行：

1. 4 个 Godot API 版本 × 2 个线程模式的 SDK 构建与 manifest/命名/隔离检查；
2. Godot 4.5.2 两种模式的 compiler 构建、Release Web 导出和无源码审计；
3. Wasm 结构、入口、dylink、共享内存和路径泄漏检查；
4. 带 COOP/COEP 的本地 HTTP 服务与无头 Chromium 行为 oracle；
5. 浏览器日志中的 Link/Runtime/Compile/SCRIPT ERROR 拒绝。

这些作业是正式 release gate，不再是待补的首次留档。

## 当前未覆盖

- Safari、Firefox、iOS/Android 浏览器的真实设备矩阵；
- CDN、反向代理、iframe、身份认证和跨域资源的组合测试；
- 断网重连、弱网、长连接、缓存升级和 Service Worker 版本迁移；
- 超大 PCK、内存增长、标签页后台化和多小时 soak；
- WebGPU、SIMD、PWA 与平台特有 JavaScriptBridge 业务。

项目依赖这些能力时，需要在目标部署环境增加专用 fixture 与运行门禁；GDPP 不能仅以“Wasm
编译成功”替代浏览器行为认证。
