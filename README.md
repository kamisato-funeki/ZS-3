# 综合设计 III：布局与交互分析系统

版本：1.1；日期：2026-10-26。原 1.0 版是详细设计基线；本 README 同时作为当前实现状态入口。项目使用 C++17、CMake，提供无 Qt 核心/CLI 构建与可选 Qt 6 GUI。

## 文档导航

| 顺序 | 文档 | 内容 |
|---|---|---|
| 1 | [项目任务书](docs/01-项目任务书.md) | 目标、范围和交付阶段 |
| 2 | [需求规格说明书](docs/02-需求规格说明书.md) | 输入输出、支持范围和需求编号 |
| 3 | [项目目录与模块设计](docs/03-项目目录与模块设计.md) | 目录职责、构建目标与当前实现映射 |
| 4 | [总体技术与接口设计](docs/04-总体技术与接口设计.md) | 领域契约、单位、配置和 CLI |
| 5 | [任务5全局布局详细设计](docs/05-任务5全局布局详细设计.md) | 密度、线长、优化和终止状态 |
| 6 | [任务6分析与可视化详细设计](docs/06-任务6分析与可视化详细设计.md) | 指标、BMP、GDSII 与回读边界 |
| 7 | [任务7布局合法化详细设计](docs/07-任务7布局合法化详细设计.md) | 检查器、宏搜索与 Abacus |
| 8 | [交互GUI详细设计](docs/08-交互GUI详细设计.md) | GUI 状态、控制、渲染和导出 |
| 9 | [测试用例与验收方案](docs/09-测试用例与验收方案.md) | 原设计用例与验收判据 |
| 10 | [参考资料与设计决策](docs/10-参考资料与设计决策.md) | 来源、范围假设和取舍 |
| 11 | [开发协作与子代理使用](docs/11-开发协作与子代理使用.md) | 中文注释、协作和提交规范 |
| 12 | [开发与验证记录](docs/12-开发与验证记录.md) | 本轮实际构建和验证命令及结果 |
| 13 | [验收追踪](docs/13-验收追踪.md) | 按原测试编号记录当前证据状态 |

第 12、13 份由主代理维护，记录逐例证据；本 README 和设计分册不替代它们。

## 当前实现状态

已实现 core、BookShelf/LEF/DEF I/O、全局布局、分析、合法化、结果序列化、BMP/GDS 导出、CLI 与 Qt GUI；headless 和 GUI CMake 构建均已成功。分模块测试及端到端验收进度不同，不能据此推定原设计的 88 个用例全部通过。精确结果和未核项目见[开发与验证记录](docs/12-开发与验证记录.md)及[验收追踪](docs/13-验收追踪.md)。

原设计交付时“尚未实现”的表述是历史状态，不能用来描述当前源码。需求和设计分册仍定义支持边界与判据；当前实现或验证尚未覆盖之处继续标为待核查，不视为范围自动扩大。

## 构建

标准 Linux 开发环境：

```sh
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build -j
```

需要 GUI 时设 `-DBUILD_GUI=ON` 并提供 Qt 6 Widgets；设为 OFF 时不查找 Qt。仓库提供 [`CMakePresets.json`](CMakePresets.json)：默认与 headless 预设的二进制分别位于 `build/default/placement_cli`、`build/headless/placement_cli`；本文下列 `build/placement_cli` 示例对应显式 `-B build` 的配置命令。构建所需第三方依赖及版本见 [`third_party/DEPENDENCIES.md`](third_party/DEPENDENCIES.md)。

本次实现验证环境将依赖解压在 `/tmp/school3-deps/usr`，因共享目录的符号链接/读回限制，实测构建目录使用 `/tmp`：

```sh
cmake -S . -B /tmp/school3-headless -DBUILD_GUI=OFF -DBUILD_TESTING=ON -DPLACEMENT_DEPS_PREFIX=/tmp/school3-deps/usr
cmake --build /tmp/school3-headless -j
cmake -S . -B /tmp/school3-gui -DBUILD_GUI=ON -DBUILD_TESTING=ON -DPLACEMENT_DEPS_PREFIX=/tmp/school3-deps/usr
```

GUI CMake 构建已成功；Qt Test 与整体验收状态以验证记录为准。本次发行版锁定依赖可用 `python3 cmake/bootstrap_deps.py` 准备，已有缓存可加 `--offline`；其他发行版优先安装对应系统开发包。

## CLI 用法

以下命令展示当前 CLI 的调用形式。`--aux`、重复的 `--lef`、`--def` 指定输入；`--config` 读取 JSON 配置，`--set key=value` 覆盖高级配置。run 可用 `--snapshot`、`--stage`、`--output`，以 `--continue-unconverged` 显式允许未收敛的全局结果继续；`export` 以 `--overwrite` 显式允许覆盖目标文件。

```sh
build/placement_cli inspect --aux /data/thin1/thin1.aux
build/placement_cli inspect --lef /data/design/cells.lef --def /data/design/top.def
build/placement_cli run --aux /data/thin1/thin1.aux --stage all --output runs/thin1 --set target_density=0.8
build/placement_cli check --aux /data/thin1/thin1.aux --snapshot runs/thin1/stages/standard_legal/placement.json
build/placement_cli export --aux /data/thin1/thin1.aux --snapshot runs/thin1/stages/global/placement.json --format bmp --output runs/thin1/global.bmp --overwrite
```

结果重载仍需原始 AUX 或 LEF/DEF，以核对指纹和完整网络拓扑。实际参数和错误状态以 CLI 帮助及[接口契约](docs/04-总体技术与接口设计.md)为准。`inspect`/`export` 成功不代表布局合法；`check` 的独立结果才用于报告合法性。不要把示例路径当作仓库内已提供的数据。

## 支持范围与协作

BookShelf 为主链路，LEF/DEF 是有明确子集的适配链路；两者映射到统一领域模型。原 `cn.lef` 与 `ispd19_test1.input.def` 的 master 不匹配，已配套的微型夹具位于 [`tests/fixtures/lefdef`](tests/fixtures/lefdef)。不支持的格式约束必须明确报错。单元手动编辑、布线、时序驱动优化、完整工业级 DEF 写回和多高度标准单元合法化仍不在本版范围。

每次实现功能需保留完整中文注释约定，详见 [AGENTS.md](AGENTS.md) 与[协作分册](docs/11-开发协作与子代理使用.md)。外部参考数据保持只读，不复制进仓库。
