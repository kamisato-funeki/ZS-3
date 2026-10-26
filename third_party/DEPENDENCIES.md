# 依赖版本、来源与使用边界

本次环境为 Ubuntu 26.04 amd64。发行版包的精确版本、文件名及 SHA-256 见 [dependencies.lock.json](dependencies.lock.json)；仅源码和锁文件入库，下载包、解包和构建产物不入库。系统开发包优先；本机 sudo 要求交互认证，因此实际使用本地解包前缀 `/tmp/school3-deps/usr`。共享目录不支持符号链接，实际验证构建放 `/tmp/school3-headless` 和 `/tmp/school3-gui`。

| 依赖 | 实测版本/提交 | 用途与链接方式 | 实际消费组件许可证来源 |
|---|---|---|---|
| GCC / libstdc++ | 15.2.0 | C++17 编译及系统动态运行库 | 系统发行版版权文件；包含运行库例外 |
| CMake | 4.2.3，项目最低3.21 | 构建/CTest，不进入产品运行库 | CMake发行版BSD类许可证 |
| FFTW | 3.3.10 | double DCT，动态链接；基线1线程 | `libfftw3-dev/copyright`，GPL-2+ |
| Qt Core/Gui/Widgets/Test | 6.10.2，项目最低6.5 | GUI/测试，动态链接；核心关闭GUI不依赖Qt | `qt6-base-dev/copyright`：消费模块LGPL-3/GPL-2；构建工具GPL-3附Qt例外 |
| CImg | 3.5.2 | 仅头文件BMP绘制，`cimg_display=0`，不链接X11/OpenCV | `cimg-dev/copyright`及CImg.h，CeCILL-C/CeCILL双许可；本项目按CeCILL-C使用 |
| Limbo | `6c115bfb1d4dca3a92044d505ee00456aac2bd45` | 仅LEF/DEF与GDS组件，私有静态链接 | 上游根LICENSE为MIT，不能覆盖下列子组件版权 |
| Limbo所附Cadence LEF/DEF | 5.8 | 官方parser源经Bison生成，静态链接 | 对应源头部Apache-2.0；不构建上游其余演示程序 |
| Limbo GDS stream writer | 上述提交 | 真实GDSII写出，静态链接 | `GdsWriter.cpp`说明原GDSLIB为GNU GPL；上游MIT不替代此项 |
| OpenSSL libcrypto | 3.5.5 | 输入SHA-256，动态链接 | 系统OpenSSL Apache-2.0版权文件 |
| nlohmann/json | 3.11.3 | 实现层JSON，不暴露公共类型 | `nlohmann-json3-dev/copyright`，MIT |
| GoogleTest | 1.17.0 | 仅测试静态库 | `libgtest-dev/copyright`，BSD-3-Clause |
| Bison / m4 | 3.8.2 / 1.4.21 | 仅构建LEF/DEF解析器 | 各发行版copyright；Bison骨架适用生成代码例外 |
| KLayout | 0.30.0 | 独立批处理GDS回读，不链接到产品 | 发行版KLayout GPL-2+及所附组件版权 |
| Boost头文件 | 1.90.0 | 本地包审计保留；当前消费的stream writer不需要Boost链接 | Boost Software License 1.0；未因此引入额外运行库 |

版权原文保留在下载的发行版包及其解包 `usr/share/doc/<包名>/copyright`，Limbo源码保留原LICENSE和源头部声明。该项目当前提供源码和构建说明，未制作闭源或商业二进制分发包；二进制分发应同时携带所消费组件的版权、许可和对应源码/重新链接材料，尤其FFTW与GDS writer的GPL条件。

准备本次锁定环境可运行 `python3 cmake/bootstrap_deps.py`，离线复验加 `--offline`。其他发行版优先用本机开发包并记录实际版本；锁文件只描述上述实测发行版，不假称可直接安装于任意Linux。
