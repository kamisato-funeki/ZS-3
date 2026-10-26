#!/usr/bin/env python3
"""按锁文件准备发行版开发包；仅写显式前缀和build，不改系统配置。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(arguments, cwd=None):
    """执行依赖工具，失败立即退出；arguments为argv，cwd为可选工作目录，无返回值。"""
    subprocess.run(arguments, cwd=cwd, check=True)


def main():
    """解析前缀并下载/校验/解包/定位依赖；无输入，返回None，失败不更改锁文件。"""
    parser = argparse.ArgumentParser(description=__doc__)  # 明确用户选择的本地依赖位置。
    parser.add_argument("--prefix", type=Path, default=Path("/tmp/school3-deps"))
    parser.add_argument("--offline", action="store_true", help="仅使用已缓存且SHA匹配的包")
    arguments = parser.parse_args()  # 本次显式请求。
    root = Path(__file__).resolve().parents[1]  # 源码根，不依赖启动目录。
    lock = json.loads((root / "third_party/dependencies.lock.json").read_text())  # 固定版本和摘要。
    cache = root / "build/packages"  # 未纳入版本库的发行版包缓存。
    prefix = arguments.prefix.resolve()  # 解包根，usr/在其下面。
    cache.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(parents=True, exist_ok=True)
    missing = []  # 缺失或损坏、需要重新下载的固定版本包。
    for package in lock["packages"]:  # 每个包的名称/版本/文件/摘要记录。
        path = cache / package["file"]  # 本包缓存路径。
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != package["sha256"]:
            missing.append(package["name"] + "=" + package["version"])
    if missing:
        if arguments.offline:
            raise RuntimeError("离线缓存缺失：" + ", ".join(missing))
        run(["apt-get", "download", *missing], cache)
    for package in lock["packages"]:  # 只解包经SHA验证的输入。
        path = cache / package["file"]  # 对应deb文件。
        if hashlib.sha256(path.read_bytes()).hexdigest() != package["sha256"]:
            raise RuntimeError("SHA-256不匹配：" + str(path))
        run(["dpkg-deb", "-x", str(path), str(prefix)])
    libraries = prefix / "usr/lib/x86_64-linux-gnu"  # 本地开发库目录。
    system = Path("/usr/lib/x86_64-linux-gnu")  # 已安装运行库，只读取不修改。
    for path in libraries.glob("*.so"):  # 开发包可能只含指向系统运行库的链接。
        if path.is_symlink() and not path.exists():
            target = system / path.readlink().name  # 系统中对应的真实运行库。
            if target.exists():
                path.unlink()
                path.symlink_to(target)
    for path in system.glob("libQt6*.so.*"):  # Qt CMake导入目标检查精确版本文件。
        target = libraries / path.name  # 本地前缀中的运行库引用。
        if not target.exists():
            if target.is_symlink():
                target.unlink()
            target.symlink_to(path)
    plugins = libraries / "qt6/plugins"  # Qt平台插件沿用系统同版本文件。
    if not plugins.exists():
        plugins.parent.mkdir(parents=True, exist_ok=True)
        plugins.symlink_to(system / "qt6/plugins", target_is_directory=True)
    source = root / "build/deps/Limbo"  # 固定上游源码，仅构建实际消费组件。
    if not source.exists():
        if arguments.offline:
            raise RuntimeError("离线缺少Limbo源码")
        source.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--no-checkout", lock["limbo"]["url"], str(source)])
    revision = subprocess.check_output(["git", "-c", "safe.directory=" + str(source), "-C", str(source), "rev-parse", "HEAD"], text=True).strip()  # 已检出身份。
    if revision != lock["limbo"]["commit"] or not (source / "limbo").exists():
        run(["git", "-c", "safe.directory=" + str(source), "-C", str(source), "checkout", lock["limbo"]["commit"]])
    print("Dependencies ready. Configure with -DPLACEMENT_DEPS_PREFIX=" + str(prefix / "usr"))
    print("KLayout: LD_LIBRARY_PATH=" + str(prefix / "usr/lib/klayout") + ":" + str(libraries) + " " + str(prefix / "usr/lib/klayout/klayout"))


if __name__ == "__main__":
    main()
