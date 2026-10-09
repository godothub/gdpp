# Godot GDScript AOT & Extension

**GDPP** 可以将 GDScript 编译为二进制：

- 高性能：编译为原生代码和二进制
- 跨平台：支持 桌面/移动/Web 平台
- 简单易用：启用插件后一键导出
- 语法拓展：更强大的 GDScript 语言

## 性能数据

| 场景 | GDScript | GDPP AOT | 相对性能 |
| --- | ---: | ---: | ---: |
| 矩阵乘法 | 3.064 s | 30.397 ms | 10,080.53% |
| 2D 测试案例 | 17.776 μs/帧 | 0.140 μs/帧 | 12,697.14% |

> 注：以上测试数据基于 Mac mini M4（10 核）/ 16 GB 内存，系统为 macOS 26.6.2，采用官方 Godot 4.7.2 和导出模板。

## 快速开始

1. 将插件解压到`addons/gdpp`，然后启用插件
2. 确保本地有 C++ 编译器，例如 MSVC(Windows)
3. 导出项目，等待编译的进度条完成

## 语法拓展

GDPP 以官方 GDScript 为兼容目标，同时计划提供更多语法支持。如果您发现兼容性问题、或者想对更完善的 GDScript 语法规划提出建议，欢迎通过 [issue](https://github.com/godothub/gdpp/issues) 向我们反馈。

## 插件支持

可以使用 **GDPP Plugins** 面板或 YAML 配置，将 GDScript 插件导出为编译好的 GDPP 插件，安装到其他 Godot 项目中使用。使用导出的插件无需安装 GDPP 或 C++ 编译器。导出包保留资源引用和公开接口所需的 `.gd` 文件，插件逻辑编译进原生库。

导出整个项目时，GDPP 只编译 `addons/` 之外的项目脚本；如需编译某个插件，请单独导出该插件。

### 在编辑器中导出

启用 GDPP 后，打开底部的 **GDPP Plugins** 面板，选择项目中的插件，或填写外部插件目录。面板会生成 YAML 配置；可以修改目标平台和编译器、导入或保存配置，然后点击 **Inspect** 查看配置与脚本清单，点击 **Export** 编译并打包。目标名称输入框可覆盖 YAML 的 `build.targets`，多个名称用逗号分隔。

macOS、Windows 和 Linux 编辑器均可使用该面板，无需单独安装 `gdpp` 命令行程序。导出前需安装目标平台所需的编译器或工具链。

### 使用命令行导出

从 Release 下载并解压 `gdpp-compiler.zip`，保持完整的目录结构，使用对应系统的程序；也可以将程序所在目录加入 `PATH`。

```text
linux/x86_64/gdpp          # glibc 2.35 起
windows/x86_64/gdpp.exe    # Windows 10 起
```

将下方示例保存为 `my_plugin.yaml`，修改插件目录、目标平台和编译器配置，然后执行：

```sh
gdpp inspect my_plugin.yaml
gdpp build my_plugin.yaml --targets windows-msvc
```

```yaml
schema_version: 1
plugin:
  source: ./addons/my_plugin
godot:
  executable: godot
  # path: /opt/godot
  target_version: "4.7"
build:
  targets: [windows-msvc]
output:
  directory: ./dist
targets:
  windows-msvc:
    platform: windows
    architecture: x86_64
    compiler: cl.exe
  macos:
    platform: macos
    architecture: universal
    compiler: clang++
  android:
    platform: android
    architecture: arm64
    ndk: /opt/android-ndk
```

- `plugin.source` 填原始插件目录，`output.directory` 填导出目录；相对路径以 YAML 所在目录为基准。
- `godot.executable` 填 Godot 程序名；已加入 `PATH` 时无需填写 `path`，否则填写程序所在目录。`target_version` 填目标 Godot 版本，如 `"4.7"`。
- 在 `targets` 下配置所需平台的编译器。`compiler` 可填完整路径，也可填 `PATH` 中的程序名；Windows 可使用 `cl.exe` 或 MinGW64 的 `g++.exe`。Android 填写 `ndk` 目录。
- `build.targets` 选择要导出的目标名称，也可用命令中的 `--targets` 指定；多个名称用逗号分隔，如 `--targets macos,android`。

导出目标需要相应工具链：MSVC 使用 Windows，macOS/iOS 使用 macOS 和 Xcode，Android 使用 NDK，Web 使用 Emscripten。导出不会修改原始插件。

## 跨越边界

如果 GDScript 代码涉及频繁的引擎调用，可能不会带来显著的性能提升。我们也在优化 GDPP 的编译过程以减少跨越边界的行为，这是性能改进的方向之一。

## 许可证

1. 本仓库里的代码为 GDPP 社区版，遵守 [MIT](https://github.com/godothub/gdpp/blob/main/LICENSE) 协议，完全开源免费。
2. GDPP 社区版使用 `1.x` 版本号，区别于专业版的 `2.x` 版本。专业版对于个人用户（包括非企业形式的独游小团体）免费使用，企业、单位和机构等用户请[联系我们](mailto:contact@godothub.com)。
