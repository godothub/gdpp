# CI 控制面 / CI Control Plane

本仓库只有 `.github/` 目录作为 GDPP CI 控制面持续维护和使用。

`main` 分支中的其他目录和文件只是停止维护的 1.x 源码快照，仅供历史查阅，并不是当前维护的 GDPP 项目代码；维护 CI 时应忽略这些内容。

当前产品源码在闭源仓库中维护，由这里的工作流通过 `GDPP_TOKEN` 拉取。

Only the `.github/` directory is maintained and used as the GDPP CI control plane. Every other directory and file on `main` is an archived 1.x source snapshot for historical access, not the currently maintained GDPP product code, and must be ignored during CI maintenance. The current product source is maintained in the private repository and checked out by these workflows through `GDPP_TOKEN`.
