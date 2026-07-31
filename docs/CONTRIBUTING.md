# 贡献与验证

GDPP 的提交必须保持“实现、证据、公开状态”一致。不能只让某个示例通过，也不能在缺失语义时
生成猜测代码；无法等价实现的路径应在前端或导出预检中失败关闭，并进入
[状态审计](STATUS.md)与[路线图](ROADMAP.md)。

## 开发环境

初始化固定版本的 godot-cpp 子模块，所有 CMake 产物只允许写入根目录 `build/`：

```sh
git submodule update --init --recursive
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev --output-on-failure
```

`dev` 构建编译器核心和 CLI，不构建 Godot 编辑器插件。需要 GDExtension、项目导出与附加运行时
测试时使用：

```sh
cmake --preset plugin
cmake --build --preset plugin --parallel
ctest --preset plugin --output-on-failure
```

不要把 compiler、SDK 或平台生成物手工复制回源码树。示例插件中的生成目录只由明确的 SDK/
发行目标维护，测试 fixture 应位于 `test/` 或 `build/`。

## 架构约束

公开模块遵循：

```text
core → frontend → semantic → ir → codegen → compiler → project
                                      runtime ← generated project
```

- compiler core 不依赖 Godot 类型；
- Godot 编辑器/导出集成只进入 `src/integration/godot/`；
- 生成项目 runtime 与编译器实现隔离，只共享版本化公共契约；
- `include/gdpp/` 下的头文件必须属于明确模块，不能恢复平铺接口；
- API、SDK schema、runtime ABI、平台和 profile 必须进入缓存/产物身份。

`tools/check_architecture.py` 会验证模块目录、include 方向和关键 runtime 边界。

## 功能提交的最低证据

语言或运行语义变更至少需要：

1. 合法正例、非法拒绝例和边界输入；
2. lexer/parser/semantic/HIR/MIR 中受影响层的结构断言；
3. 生成 C++ 可编译测试；
4. 与对应版本 Godot GDScript 的行为 oracle；
5. 涉及导出时的项目库、PCK 无源码和 descriptor 检查；
6. 涉及第三方 GDExtension 时的 ClassDB 契约、加载顺序和生命周期 fixture；
7. 性能敏感路径的 AOT/GDScript 同机 AB/BA 对照。
8. 涉及调试语义时的 Debug/Release 代码差分、真实 godot-cpp 编译和无调试器运行路径；若声明
   编辑器调试体验，还必须有对应 Godot 调试会话证据。

修复应覆盖同类结构，而不是匹配单个文件名、场景名或客户资源。测试和文档不得包含客户项目名称、
账号、地址、凭据或可识别业务数据。

## 文档契约

`docs/` 只描述当前源码和已经存在的证据。历史发布内容只放
`CHANGELOG.md` / `CHANGELOG-ZH.md`，路线图不能把计划写成已交付。

```sh
python3 tools/check_docs.py
python3 tools/check_architecture.py
ruby ci/tools/check_workflow_topology.rb
```

文档检查会验证：

- 产品版本、Godot 版本矩阵、SDK schema 与 runtime ABI；
- 三个多宿主发行包和当前项目库命名；
- 相对链接、必需文档集合和过时状态语句；
- 客户标识没有进入公开工程文档；
- release 工作流默认版本与产品版本一致。

能力、限制或认证范围变化时，至少同步 `STATUS.md`、`COMPATIBILITY.md`、`ROADMAP.md` 与对应专项
文档。

## 提交前验证

如果修改了 C++：

```sh
find include src test -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

随后执行：

```sh
python3 tools/check_docs.py
python3 tools/check_architecture.py
ruby ci/tools/check_workflow_topology.rb
cmake --build --preset dev --parallel
ctest --preset dev --output-on-failure
cmake --build --preset plugin --parallel
ctest --preset plugin --output-on-failure
```

平台代码还必须走对应的可复用工作流；正式发行只有在 quality、core、native、compatibility、
Android、iOS、Web、三平台组件、汇总打包和安装后导出/运行全部通过时才能发布。

## 持续集成边界

可执行的 GitHub Actions 控制面位于根级 `ci/` 子模块（`godothub/gdpp`），闭源仓库不保留
workflow。公开控制面只允许维护者从默认分支手动启动，通过只读 `GDPP_TOKEN` 检出一个固定
的闭源源码提交；所有并行门禁、打包与发布作业复用同一源码提交，并验证闭源仓库锁定的
`ci/` 提交就是当前流水线提交。外部 PR 不触发任何带密钥的任务，公开诊断工件在上传前必须
通过源码泄露审计。

## 提交与版本

- 每个提交只包含一个可描述、可回退的工程意图；
- 大型功能拆分为前端、IR、runtime、平台集成、测试和文档等有序提交；
- 不把数十个文件压成少数不可审阅提交；
- 修复升级 patch，重大兼容能力升级 minor；
- 版本变更同时更新 CMake、`plugin.cfg`、双语 changelog、状态/兼容文档和 release 输入默认值；
- 发行 tag 和 GitHub Release 不覆盖、不复用。

提交后直接推送约定分支；除非仓库所有者明确要求，不把正常交付流程改成 PR。
