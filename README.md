# 集成电路布局与交互分析系统

## 1. 项目概况

本项目面向集成电路物理设计中的布局实验，完成从电路数据读取、初始位置准备、全局布局，到宏单元与标准单元合法化、独立分析和结果导出的完整流程。

项目采用 **Linux + C++17 + CMake**，提供命令行程序 `placement_cli` 和可选的 **Qt 6 Widgets 图形界面** `placement_gui`。两种入口共用应用服务、算法和领域模型，适合交互观察、批量实验与无桌面环境运行。

主要处理链路为：

```text
BookShelf / LEF+DEF
        ↓
输入审计与初始位置准备（initial）
        ↓
静电模型全局布局（global）
        ↓
宏单元模拟退火合法化（macro_legal）
        ↓
行区间切分与标准单元 Abacus 合法化（standard_legal）
        ↓
独立合法性检查、指标报告、BMP / GDSII / JSON 导出
```

BookShelf 是主输入链路；LEF/DEF 支持文档限定的子集。全局布局阶段允许重叠，合法化阶段只有通过独立检查才会发布成功快照。

## 2. 使用与启动方式

### 2.1 进入项目目录

以下命令均在**包含 `CMakeLists.txt` 的仓库根目录**执行。本开发环境的路径为：

```sh
cd /mnt/hgfs/programs/school-3/code
```

在其他机器上请替换为实际克隆目录。仓库自带微型测试夹具，大型参考数据保持外部存放，不需要复制到源码目录。

### 2.2 准备环境和依赖

基础要求为 Linux、支持 C++17 的编译器、CMake 3.21 或以上，以及 Git。JSON 依赖要求 nlohmann/json 3.11 或以上；GUI 需要 Qt 6.5 或以上。只构建 CLI 时可以完全不安装 Qt。

**方式 A：使用系统开发包。** 下面以使用 APT 的 Ubuntu/Debian 环境为例；不同发行版的包名和可用版本应以本机为准。

```sh
# 核心、CLI、解析器构建及自动测试所需开发包。
sudo apt-get update
sudo apt-get install build-essential cmake git bison flex m4 \
  libfftw3-dev libssl-dev nlohmann-json3-dev cimg-dev libgtest-dev

# 需要图形界面时安装。
sudo apt-get install qt6-base-dev qt6-base-dev-tools

# 可选：用于独立回读、检查生成的 GDSII 文件。
sudo apt-get install klayout
```

此外，必须准备**固定提交的 Limbo 源码**。若 `build/deps/Limbo` 尚不存在，执行：

```sh
mkdir -p build/deps
git clone https://github.com/limbo018/Limbo.git build/deps/Limbo
git -C build/deps/Limbo checkout 6c115bfb1d4dca3a92044d505ee00456aac2bd45
```

CMake 默认从该位置读取 Limbo，并检查提交是否匹配；已有正确源码时跳过克隆。若放在其他位置，配置时增加 `-DLIMBO_SOURCE_DIR=/实际路径/Limbo`。项目只构建所需 LEF/DEF 解析器和 GDS writer，不需要先安装整个 Limbo 工程。

**方式 B：复用本项目验证过的本地依赖前缀。** 对与锁文件匹配的 Ubuntu 26.04 amd64 环境，可使用：

```sh
python3 cmake/bootstrap_deps.py --prefix /tmp/school3-deps
```

该脚本下载、校验并解包固定版本的发行版包，同时准备 Limbo，开发包位于 `/tmp/school3-deps/usr`；初次执行需要网络、Python 3、APT、`dpkg-deb` 和 Git。已具备完整下载缓存及 Limbo 源码时，可改为：

```sh
python3 cmake/bootstrap_deps.py --prefix /tmp/school3-deps --offline
```

本地解包方式依赖兼容的系统运行库，不是适用于任意 Linux 的通用安装器。使用它时，需要在下方 CMake 配置命令中增加 `-DPLACEMENT_DEPS_PREFIX=/tmp/school3-deps/usr`。精确版本、摘要与许可证见[依赖清单](third_party/DEPENDENCIES.md)和[依赖锁文件](third_party/dependencies.lock.json)。

### 2.3 从源码构建

**构建 GUI 和 CLI：**

```sh
cmake -S . -B build/default -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GUI=ON -DBUILD_TESTING=ON
cmake --build build/default --parallel 2

# 在有桌面会话的终端中启动图形界面。
./build/default/placement_gui
```

**只构建 CLI：** 适用于服务器、SSH 和没有图形桌面的环境。`BUILD_GUI=OFF` 会完全跳过 Qt 查找。

```sh
cmake -S . -B build/headless -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build/headless --parallel 2
./build/headless/placement_cli --help
```

`BUILD_TESTING=ON` 会构建测试，需要 GoogleTest；GUI 测试还使用 Qt Test。只需要程序时可设为 `-DBUILD_TESTING=OFF`。

仓库也提供等价的常规构建预设，依赖准备完成后可以选择使用：

```sh
# GUI + CLI，输出目录为 build/default。
cmake --preset default
cmake --build --preset default --parallel 2

# 仅 CLI，输出目录为 build/headless。
cmake --preset headless
cmake --build --preset headless --parallel 2
```

预设同样允许附加 `-DPLACEMENT_DEPS_PREFIX=...` 等配置参数。完整定义见 [CMakePresets.json](CMakePresets.json)。

### 2.4 本开发环境的启动命令

本机已安装系统开发包，当前使用系统 FFTW、OpenSSL、nlohmann/json、GoogleTest 和 Qt 6。由于 hgfs 共享目录的符号链接和对象文件读回限制，**当前验证构建目录为 `/tmp/school3-system-check`，同时包含 GUI 和 CLI**。

`/tmp` 是 Linux 根目录下的临时目录，不在项目目录内。可以在文件管理器按 `Ctrl+L`，输入 `/tmp/school3-system-check` 查看；临时目录被清理后需要重新构建。

已有构建结果时，在本机 Ubuntu 桌面的终端执行：

```sh
cd /mnt/hgfs/programs/school-3/code

# 使用 Qt 通用主题和 Fusion 样式，避开本机 GNOME 设置兼容问题。
QT_QPA_PLATFORMTHEME=generic QT_STYLE_OVERRIDE=Fusion \
  /tmp/school3-system-check/placement_gui

# 查看 CLI 帮助，不需要桌面。
/tmp/school3-system-check/placement_cli --help
```

本机直接启动 GUI 曾出现 `Settings schema 'org.gnome.settings-daemon.plugins.xsettings' does not contain a key named 'antialiasing'` 并中止。上面的环境变量仅作用于该次启动，用于绕过桌面主题集成；已在本机桌面会话中验证启动后运行 5 秒未出现该错误，随后主动结束验证。此检查只确认启动，不替代完整人工交互验收。

如果程序不存在，或者需要重新构建，先确认已按2.2节准备系统开发包及 Limbo 源码，再执行：

```sh
cd /mnt/hgfs/programs/school-3/code

cmake -S . -B /tmp/school3-system-check \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GUI=ON -DBUILD_TESTING=ON \
  -DPLACEMENT_DEPS_PREFIX= \
  -DGTest_DIR=/usr/lib/x86_64-linux-gnu/cmake/GTest \
  -Dnlohmann_json_DIR=/usr/share/cmake/nlohmann_json
cmake --build /tmp/school3-system-check --parallel 2

# 确认程序已生成，并运行自动测试。
ls -l /tmp/school3-system-check/placement_gui /tmp/school3-system-check/placement_cli
ctest --test-dir /tmp/school3-system-check --output-on-failure
```

上述 Ninja 构建命令需要 `ninja-build`，未安装时可执行 `sudo apt-get install ninja-build`。`-B` 指定构建输出目录，CMake 自动创建目录。命令中的两个包路径适用于本机 Ubuntu amd64，显式选择系统 GoogleTest 和 JSON，避免激活的 Miniconda 环境优先提供其他版本。`PLACEMENT_DEPS_PREFIX` 留空，表示此次构建不使用原来的本地解包前缀。

截至2026-10-26，该系统依赖配置已成功编译 GUI、CLI 和测试程序，**85/85 项 CTest 通过**，包括 GUI 离屏测试和 KLayout 独立回读。无桌面环境使用 CLI；Qt 的 `offscreen` 方式用于自动测试，不会显示可交互的桌面窗口。

### 2.5 GUI 首次使用流程

1. **导入设计。** 启动 `placement_gui`，通过“文件 → 打开 BookShelf AUX…”选择 `.aux`，或通过“打开 LEF / DEF…”依次选择一个或多个 LEF 和配套 DEF。路径由文件对话框选择；AUX 中的关联文件按 AUX 所在目录解析。
2. **确认加载结果。** 左侧显示数据集统计、输入审计和阶段树。加载成功后出现 `initial` 阶段；加载失败时保留已有成功项目，并在日志中显示诊断。
3. **设置参数。** 打开工具栏“统一配置（高级）”，编辑共享配置 JSON，点击“校验并应用”。界面与 CLI 使用同一套字段和默认值。参数修改后需要重新运行，已有阶段仍保留原运行配置。
4. **执行布局。** 使用“运行 → 全流程”，或依次选择“全局布局”“宏合法化”“标准单元合法化”。宏阶段要求已完成的 `global`，标准阶段要求 `macro_legal`；缺少前置阶段时相应按钮不可用。
5. **观察与控制。** 查看曲线、日志和阶段状态；暂停请求在完整安全点生效，界面先显示等待状态，再显示已暂停。可以继续或取消，取消会唤醒暂停中的任务。运行期间参数锁定。
6. **比较和导出。** 选择阶段，使用“单 / 双视图对比”；通过“导出 → 导出完整布局 / JSON / 报告…”选择源画布、格式和路径。当前视口截图使用单独菜单项，与完整布局 BMP 区分。

画布支持滚轮缩放、中键或空格加左键拖动平移、左键选择、Ctrl 增减选择、Alt 循环选择重叠对象、拖框选择。`F` 适应芯片，`Shift+F` 适应 core；名称查找区分大小写，属性和违规列表可用于定位对象。

仓库内可用 `tests/fixtures/tiny/tiny.aux` 体验导入与合法化。若希望先验证整个界面链路，可把高级配置中的 `overflow_stop` 设为 `0.9`，与下方微型 CLI 演示一致；这是演示阈值，正式实验默认值为 `0.1`，应结合实际收敛结果选择参数。

### 2.6 CLI：从输入到完整结果

后文使用环境变量 `PLACEMENT_CLI` 指向实际程序，先在**同一终端**选择一种路径：

```sh
# 常规 headless 构建。
export PLACEMENT_CLI=./build/headless/placement_cli
```

本开发环境改用以下路径：

```sh
export PLACEMENT_CLI=/tmp/school3-system-check/placement_cli
```

如果只构建了常规 GUI 版本，也可设为 `./build/default/placement_cli`。

**第一步：检查输入。**

```sh
"$PLACEMENT_CLI" inspect --aux tests/fixtures/tiny/tiny.aux
```

输出 JSON 审计，包含数据集身份、文件指纹、节点/网络/引脚数量、单位及警告。BookShelf 的 `.nodes/.nets/.pl/.scl/.wts` 由 `.aux` 关联，不需要逐个作为命令参数传入。

**第二步：运行完整微型示例。**

```sh
"$PLACEMENT_CLI" run \
  --aux tests/fixtures/tiny/tiny.aux \
  --stage all --set overflow_stop=0.9 \
  --output runs/readme-bookshelf
```

该夹具用于演示四阶段串联，显式采用宽松阈值 `0.9`，不能据此评价全局优化质量。真实实验可以去掉该覆盖，使用默认阈值 `0.1`。运行目录必须尚不存在；重复实验请更换 `--output`，或省略该参数，由程序在 `runs/<runId>/` 创建独占目录。

**第三步：独立检查最终布局。**

```sh
"$PLACEMENT_CLI" check \
  --aux tests/fixtures/tiny/tiny.aux \
  --snapshot runs/readme-bookshelf/stages/standard_legal/placement.json \
  --scope all
```

`check` 会重新执行独立检查，退出码 `0` 表示无违规，`3` 表示存在违规。重载快照仍须提供原始输入，以核对 SHA-256 指纹并恢复完整网络拓扑。

**第四步：按需重新导出。** 全流程已自动生成各阶段 BMP/GDS，以下示例演示额外导出：

```sh
"$PLACEMENT_CLI" export \
  --aux tests/fixtures/tiny/tiny.aux \
  --snapshot runs/readme-bookshelf/stages/standard_legal/placement.json \
  --format bmp --output runs/readme-bookshelf/final.bmp

"$PLACEMENT_CLI" export \
  --aux tests/fixtures/tiny/tiny.aux \
  --snapshot runs/readme-bookshelf/stages/standard_legal/placement.json \
  --format gds --output runs/readme-bookshelf/final.gds
```

格式可选 `placement`、`bmp`、`gds`、`report`。默认拒绝覆盖已有文件，明确需要替换时增加 `--overwrite`；该选项不能让 `run` 复用已有结果目录。`inspect` 或 `export` 成功只说明读取/导出完成，布局是否合格以独立检查为准。

### 2.7 LEF/DEF 输入和分阶段运行

仓库内配套 LEF/DEF 微型例的成功流程为：

```sh
"$PLACEMENT_CLI" inspect \
  --lef tests/fixtures/lefdef/tiny.lef --def tests/fixtures/lefdef/tiny.def

"$PLACEMENT_CLI" run \
  --lef tests/fixtures/lefdef/tiny.lef --def tests/fixtures/lefdef/tiny.def \
  --stage all --set overflow_stop=0.11 --output runs/readme-lefdef
```

可重复指定 `--lef` 读取多个库，`--def` 指定单个设计文件。上例的 `0.11` 是该配套夹具的显式参数；其默认 `0.10` 实测会触发密度停滞，具体结果见[验证记录](docs/12-开发与验证记录.md)。原参考资料中的 `cn.lef` 与 `ispd19_test1.input.def` 不配套，用于缺失 master 的诊断回归，不能替代成功示例。

需要分开运行阶段时，使用以下链路；每次都保存到新目录，并复用第一步的有效配置：

```sh
"$PLACEMENT_CLI" run --aux tests/fixtures/tiny/tiny.aux \
  --stage global --set overflow_stop=0.9 --output runs/readme-global

"$PLACEMENT_CLI" run --aux tests/fixtures/tiny/tiny.aux \
  --config runs/readme-global/effective-config.json \
  --snapshot runs/readme-global/stages/global/placement.json \
  --stage macro --output runs/readme-macro

"$PLACEMENT_CLI" run --aux tests/fixtures/tiny/tiny.aux \
  --config runs/readme-global/effective-config.json \
  --snapshot runs/readme-macro/stages/macro_legal/placement.json \
  --stage standard --output runs/readme-standard
```

若全局阶段返回 `max_iterations`，可在全流程请求或后续宏请求中增加 `--continue-unconverged`，显式允许继续合法化。后续合法化成功仍保留全局未收敛结论，最终退出码仍为 `3`。数值失败与达到迭代上限分别报告，不能用该选项把数值失败改成已收敛。

### 2.8 配置和退出码

配置优先级为 **CLI 显式参数 → JSON 配置文件 → 内建默认值**。文件只需包含待修改字段，未知字段、非法类型和越界值会被拒绝。例如：

```sh
cat > /tmp/placement-config.json <<'JSON'
{
  "seed": 42,
  "initial_mode": "auto",
  "target_density": 0.8,
  "grid_nx": 32,
  "grid_ny": 32,
  "overflow_stop": 0.1,
  "max_iterations": 2000,
  "macro_sweeps": 100,
  "moves_per_macro": 20
}
JSON

"$PLACEMENT_CLI" run --aux tests/fixtures/tiny/tiny.aux \
  --config /tmp/placement-config.json \
  --seed 7 --set overflow_stop=0.9 --output runs/readme-config
```

上例实际使用 seed=7、overflow_stop=0.9，其余字段继承文件和默认值。常用参数可用 `--seed`、`--target-density`、`--max-iterations` 等长选项，高级参数用 `--set snake_case_key=value`；最终完整值保存到 `effective-config.json`。具体范围和数值语义见[接口契约](docs/04-总体技术与接口设计.md)。CLI 运行中可按 `Ctrl+C` 请求协作取消。

| 退出码 | 含义 |
|---|---|
| `0` | 请求完成且相应条件满足；独立检查时表示零违规 |
| `2` | 输入、配置、引用、支持范围或前置条件错误 |
| `3` | 全局未收敛、未找到合法解，或独立检查发现违规 |
| `4` | 数值、内部处理或 I/O 失败 |
| `130` | 用户取消 |

### 2.9 常见启动与运行问题

| 现象 | 处理方式 |
|---|---|
| CMake 提示 `Limbo missing` 或提交不匹配 | 按2.2节准备固定提交，或传入正确的 `LIMBO_SOURCE_DIR` |
| CMake 找不到 FFTW、JSON、CImg、OpenSSL 或 GoogleTest | 安装对应开发包；本地解包方式传入 `PLACEMENT_DEPS_PREFIX` |
| 找不到 Qt Widgets / Qt Test | 安装 Qt 6 开发包；只需 CLI 时设 `BUILD_GUI=OFF`，只需程序时可关闭测试 |
| GUI 提示无法连接显示服务或加载平台插件 | 在可用桌面会话中启动并检查 Qt 平台插件；无桌面任务使用 CLI |
| GUI 因 GNOME `xsettings` 缺少 `antialiasing` 键而中止 | 使用2.4节带 `QT_QPA_PLATFORMTHEME=generic QT_STYLE_OVERRIDE=Fusion` 的启动命令 |
| 构建后在项目内找不到 `tmp` 目录 | `/tmp` 位于 Linux 根目录；执行 `ls -l /tmp/school3-system-check`，若程序不存在则检查配置和编译输出 |
| CMake 选中了 Miniconda 的 GoogleTest / JSON，或提示运行库路径冲突 | 参照2.4节显式指定系统 `GTest_DIR` 和 `nlohmann_json_DIR` 后重新配置、构建 |
| hgfs 共享目录构建异常或不支持符号链接 | 参照2.4节，将构建目录和解包依赖放到 `/tmp` 等本地文件系统 |
| 输出目录或文件已存在 | 运行改用新目录；单文件导出按需增加 `--overwrite` |
| 宏/标准阶段提示缺少前置快照 | 分别提供 `global` / `macro_legal` 的 `placement.json` 和原始输入 |
| 重载提示数据集不匹配 | 使用生成快照时的原始文件；输入内容变化后重新加载和运行 |
| 全局未收敛或数值停滞 | 查看 termination、指标及诊断；按有效配置调整后新开运行，明确区分不同终止原因 |

## 3. 功能列表

| 功能 | 当前能力 |
|---|---|
| 输入与审计 | BookShelf六文件关联、LEF/DEF子集、单位和方向归一化、引用/数量检查、SHA-256与分类审计 |
| 初始位置 | auto / preserve / random；固定对象保持输入值，随机过程由种子控制 |
| 全局布局 | WA线长及梯度、守恒平滑密度、静态区域并集、临时filler、FFTW DCT/泊松、投影Nesterov |
| 宏合法化 | 模拟退火平移搜索、固定输入方向、合法最优解与失败候选诊断 |
| 标准单元合法化 | 障碍扣除、原始站点网格切分、整数站点Abacus、候选行成本比较与独立复检 |
| 指标与检查 | 真实/加权HPWL、分类位移、重叠/边界/固定性/站点检查、完整计数及有界详情 |
| 结果与导出 | 四阶段快照、JSON/CSV/事件日志、无窗口BMP、真实GDSII、量化检查和原子写出 |
| GUI交互 | 阶段树、单/双画布、缩放平移、稳定ID选择、名称查找、网络高亮、图层、曲线和违规定位 |
| 运行控制 | 后台执行、安全点暂停/继续/取消、有界预览、迟到消息隔离和独立导出任务 |

## 4. 项目详情

### 4.1 分层与目录

```text
CLI / Qt GUI → application 应用服务 → I/O、算法、分析、可视化 → core 领域模型
```

| 目录 | 职责 |
|---|---|
| `include/placement/` | 稳定公共接口，仅暴露标准C++领域类型 |
| `src/core/` | 拓扑、位置、几何、配置、快照、错误和控制令牌 |
| `src/io/` | 输入解析、指纹、配置与结果序列化 |
| `src/global/` | 线长、密度、频域求解和全局优化 |
| `src/legalization/` | 宏SA、行切分和标准单元Abacus |
| `src/analysis/`、`src/visualization/` | 独立指标/检查与BMP/GDS输出 |
| `src/application/`、`src/cli/`、`src/gui/` | 共享流水线及两种入口 |
| `tests/` | 单元、集成、Qt测试、微型夹具与性能工具 |
| `cmake/`、`third_party/` | 构建适配、依赖准备和版本/许可证登记 |
| `benchmarks/`、`docs/` | 外部数据目录、实测摘要与设计/验证分册 |
| `build/`、`runs/` | 本地构建和运行产物；内容不纳入Git |

### 4.2 运行结果结构

完整运行在独占目录中保存：

```text
runs/<runId 或指定目录>/
├── effective-config.json          # 最终有效配置
├── input-audit.json               # 输入路径、指纹、规模和警告
├── manifest.json                  # 源版本、依赖、seed、阶段状态和输出清单
├── metrics.csv                    # 初始/前置阶段及算法完整轮次指标
├── events.jsonl                   # 带身份、顺序和时间的事件流
├── stages/
│   ├── initial/
│   ├── global/
│   ├── macro_legal/
│   └── standard_legal/            # 各阶段含placement.json和violations.json
├── images/<stage>.bmp
├── gds/<stage>.gds
├── gds/<stage>.gds.metadata.json
└── diagnostics/                   # 有失败候选时保存，非成功阶段
```

失败或取消时只保留已提交阶段及可用诊断，未完成阶段不会伪装成成功结果。`placement.json` 保存真实实例，临时 filler 不进入实例快照；结果重载用于查看、检查和后续合法化，不恢复全局优化器的动量状态。

### 4.3 单位、几何和支持边界

- BookShelf 保留 DU；未给定 `microns_per_du` 时，GDS 使用1 DU=1 μm的显示映射，并明确记录物理比例未知。真实物理尺寸需要用户提供正确比例。
- LEF/DEF 归一化为1 DU=1 μm，处理非零ORIGIN、八方向与引脚世界坐标。
- 碰撞、硬阻挡和边界共用物理距离容差；站点对齐比较DU距离残差，行切分保持原行网格。
- 全局布局初始overflow已达阈值时可直接完成；其余情况要求连续五轮达标。合法化成功不改写上游未收敛结论。
- 当前标准合法化面向单高度、整数站点宽度单元。不包含多高度合法化、旋转搜索、手动布局编辑、布线、时序驱动优化和完整工业DEF写回。

## 5. 测试与验证状态

构建时开启 `BUILD_TESTING` 后运行：

```sh
ctest --test-dir build/headless --output-on-failure
ctest --test-dir build/default --output-on-failure

# 早期本地依赖前缀构建的验证命令（保留历史记录）。
env -u DISPLAY ctest --test-dir /tmp/school3-headless --output-on-failure
ctest --test-dir /tmp/school3-gui --output-on-failure

# 当前系统依赖构建（同时覆盖 CLI 和 GUI 测试）。
ctest --test-dir /tmp/school3-system-check --output-on-failure
```

截至2026-10-26，已记录无Qt配置 **84/84 CTest**、GUI配置 **85/85 CTest** 通过，Qt Test内部 **17项**（含init/cleanup）通过；两个成功流程的8份阶段GDS经KLayout独立回读通过，同输入/配置/seed的两次thin1四阶段数学结果一致。

原88项验收逐项追踪为 **51项通过、37项部分覆盖**。真实桌面人工操作、完整大数据合法化性能等仍有未验收条件。GUI离屏测试通过不能替代真实桌面验收，微型例成功也不意味着任意电路或默认参数必然收敛。实际命令、环境、性能、失败记录和限制见[开发与验证记录](docs/12-开发与验证记录.md)及[验收追踪](docs/13-验收追踪.md)。

## 6. 详细文档与开发约定

| 文档 | 内容 |
|---|---|
| [01 项目任务书](docs/01-项目任务书.md) | 目标、范围和交付阶段 |
| [02 需求规格说明书](docs/02-需求规格说明书.md) | 输入输出、支持范围和需求编号 |
| [03 项目目录与模块设计](docs/03-项目目录与模块设计.md) | 目录职责与模块映射 |
| [04 总体技术与接口设计](docs/04-总体技术与接口设计.md) | 领域契约、单位、配置、CLI和错误 |
| [05 全局布局详细设计](docs/05-任务5全局布局详细设计.md) | 密度、线长、优化和终止状态 |
| [06 分析与可视化详细设计](docs/06-任务6分析与可视化详细设计.md) | 指标、BMP、GDSII与回读 |
| [07 布局合法化详细设计](docs/07-任务7布局合法化详细设计.md) | 检查器、宏SA与Abacus |
| [08 交互GUI详细设计](docs/08-交互GUI详细设计.md) | 交互、状态、线程、渲染和导出 |
| [09 测试用例与验收方案](docs/09-测试用例与验收方案.md) | 原验收用例及判据 |
| [10 参考资料与设计决策](docs/10-参考资料与设计决策.md) | 来源、范围假设和取舍 |
| [11 开发协作与子代理使用](docs/11-开发协作与子代理使用.md) | 中文注释、协作和提交规范 |
| [12 开发与验证记录](docs/12-开发与验证记录.md) | 已执行的构建、运行、性能和限制 |
| [13 验收追踪](docs/13-验收追踪.md) | 全部88项验收的实际证据状态 |

开发遵守 [AGENTS.md](AGENTS.md) 及各级补充规范：保持模块边界、完整中文语义注释和Doxygen契约，每个经过验证的完整功能单元形成中文本地提交。外部参考资料保持只读，大型输入和运行产物不入库。第三方依赖的版本及各组件许可证分别记录，不假定上游仓库的总许可证覆盖全部子组件。
