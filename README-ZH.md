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

## 跨越边界

如果 GDScript 代码涉及频繁的引擎调用，可能不会带来显著的性能提升。我们也在优化 GDPP 的编译过程以减少跨越边界的行为，这是性能改进的方向之一。

## 许可证

1. 本仓库里的代码为 GDPP 社区版，遵守 [MIT](https://github.com/godothub/gdpp/blob/main/LICENSE) 协议，完全开源免费。
2. GDPP 社区版使用 `1.x` 版本号，区别于专业版的 `2.x` 版本。专业版对于个人用户（包括非企业形式的独游小团体）免费使用，企业、单位和机构等用户请[联系我们](mailto:contact@godothub.com)。
