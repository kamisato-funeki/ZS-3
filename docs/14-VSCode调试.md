# VS Code 程序调试

导航：[首页](../README.md) · [验证记录](12-开发与验证记录.md)。

## 1. 选择程序调试入口

安装 Microsoft C/C++ 扩展（`ms-vscode.cpptools`），以及系统 CMake、GDB 和 README 2.2 节的开发依赖。在 VS Code 打开包含 `CMakeLists.txt` 的 `code` 仓库目录。

在“运行和调试”面板的下拉框选择 `Placement: GUI (GDB)` 或 `Placement: CLI (GDB)`，然后按 F5。配置位于 [launch.json](../.vscode/launch.json)，使用 C++ 调试器 `cppdbg`。CLI 默认传入 `--help`，执行后正常退出；运行实际任务时修改该启动项的 `args` 数组。

如果提示“`cmakeDebugType` 为 `script` 时要求 `scriptPath`”，表示当前选中了 CMake 脚本调试器。切换到上面的 Placement 启动项。CMake 的脚本调试器用于执行独立 `.cmake` 脚本；`CMakeLists.txt` 是项目配置入口，不应作为 `cmake -P` 脚本补入 `scriptPath`。

## 2. 构建与运行环境

[tasks.json](../.vscode/tasks.json) 在启动前依次执行配置与构建，生成 `build/debug/placement_gui` 和 `build/debug/placement_cli`，包含 Debug 符号。此构建开启 GUI、关闭测试，不改变 `build/default` 的 Release 构建。自动测试仍按 README 使用 `build/default` 执行。

配置任务使用 `cmake --fresh`，要求 CMake 3.24 或以上，避免复用失效的临时依赖缓存；JSON 包路径显式选择本机 Ubuntu amd64 系统包。其他发行版需调整该路径及 `/usr/bin/gdb`。首次 F5 需要等待编译完成。GUI 设置 `QT_QPA_PLATFORMTHEME=generic` 与 `QT_STYLE_OVERRIDE=Fusion`，沿用本机已验证的主题启动方式。

本机同时在上级 `school-3/.vscode/` 放置了适用于打开 `school-3` 的启动与任务配置，源码路径为 `${workspaceFolder}/code`。上级目录不属于本仓库，该配置只在本机生效；克隆仓库后直接打开 `code` 即可使用已纳入版本管理的配置。
