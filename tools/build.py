#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

# 导入VS环境配置工具
sys.path.insert(0, str(Path(__file__).parent))
import vs_env


class BuildError(Exception):
    """构建错误异常"""
    pass


class BuildConfig:
    """构建配置"""
    def __init__(self):
        self.source_dir = Path(__file__).parent
        self.build_dir = Path("C:/vmbuild")
        self.verbose = True
        self.log_dir = self.source_dir / "build_logs"


def run_cmake_build_with_pty(config, target=None, clean=False):
    """使用伪控制台运行 CMake 构建

    Args:
        config: 构建配置
        target: 构建目标（None表示构建全部）
        clean: 是否先清理

    Returns:
        构建退出码

    Raises:
        BuildError: 构建失败
    """
    build_dir = config.build_dir

    if not build_dir.exists():
        raise BuildError(f"构建目录不存在: {build_dir}")

    # 确保日志目录存在
    config.log_dir.mkdir(parents=True, exist_ok=True)

    # 创建日志文件
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = config.log_dir / f"build_{timestamp}.log"

    # 检查是否需要配置VS环境
    vs_script, arch = vs_env.get_vs_env_from_build_dir(build_dir)

    if vs_script:
        print(f"检测到MSVC编译器，将使用VS环境脚本: {vs_script}")
        print(f"目标架构: {arch}\n")

        # 对于MSVC，创建临时批处理文件来设置VS环境并执行构建
        batch_file = config.log_dir / f"build_{timestamp}.bat"

        cmake_args = ['cmake', '--build', str(build_dir), '--config', 'Debug']
        if target:
            cmake_args.extend(['--target', target])
        if config.verbose:
            cmake_args.append('--verbose')

        build_cmd = ' '.join(cmake_args)

         # 写入批处理文件
        with open(batch_file, 'w', encoding='utf-8') as f:
            f.write('@echo off\n')
            # Use triple quotes to handle paths with spaces
            f.write(f'call ""{vs_script}"" -arch={arch}\n')
            f.write(f'{build_cmd}\n')

        # 执行批处理文件
        cmd = ['cmd.exe', '/c', str(batch_file)]

        print(f"\n{'='*60}")
        print(f"开始构建（使用VS环境）")
        print(f"{'='*60}\n")
        print(f"构建日志: {log_file}")
        print(f"批处理文件: {batch_file}\n")

        try:
            return _run_without_pty(cmd, log_file, build_dir)
        finally:
            # 清理临时批处理文件
            if batch_file.exists():
                batch_file.unlink()

    # 如果需要清理
    if clean:
        print("清理构建目录...")
        subprocess.run(
            ["cmake", "--build", str(build_dir), "--config", "Debug", "--target", "clean"],
            check=True
        )

    # 构建命令（使用cmake --build而不是直接调用ninja）
    cmd = ["cmake", "--build", str(build_dir), "--config", "Debug"]

    if target:
        cmd.extend(["--target", target])

    if config.verbose:
        cmd.append("--verbose")

    print(f"\n{'='*60}")
    print(f"开始构建: {' '.join(cmd)}")
    print(f"{'='*60}\n")
    print(f"构建日志: {log_file}")

    # 使用 winpty 捕获控制台输出
    return _run_without_pty(cmd, log_file, build_dir)

def _run_without_pty(cmd, log_file, build_dir):
    """不使用伪控制台运行命令（回退方案）"""
    # 创建环境变量，禁用彩色输出和输出缓冲
    env = os.environ.copy()
    env['TERM'] = 'dumb'
    env['NO_COLOR'] = '1'
    env['CLICOLOR'] = '0'
    env['CLICOLOR_FORCE'] = '0'
    env['PYTHONUNBUFFERED'] = '1'
    env['GCC_COLORS'] = ''

    import queue
    import threading
    import sys

    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        cwd=build_dir,
        env=env
    )

    output_queue = queue.Queue()
    stop_event = threading.Event()

    def read_output(pipe, log_f, is_stderr=False):
        """读取输出流"""
        prefix = "[STDERR] " if is_stderr else "[STDOUT] "
        while not stop_event.is_set():
            try:
                line = pipe.readline()
                if not line:
                    break
                try:
                    decoded_line = line.decode('utf-8')
                except:
                    decoded_line = line.decode('gbk', errors='replace')

                log_f.write(f"{prefix}{decoded_line}")
                log_f.flush()
                try:
                    print(f"{prefix}{decoded_line}", end='', flush=True)
                except UnicodeEncodeError:
                    safe_line = decoded_line.encode('utf-8', errors='replace').decode('utf-8')
                    print(f"{prefix}{safe_line}", end='', flush=True)
                output_queue.put((is_stderr, decoded_line))
            except Exception as e:
                print(f"\n读取输出时出错: {e}")
                break

    with open(log_file, "w", encoding="utf-8", buffering=1) as log_f:
        log_f.write(f"构建开始时间: {datetime.now()}\n")
        log_f.write(f"命令: {' '.join(cmd)}\n")
        log_f.write(f"工作目录: {os.getcwd()}\n")
        log_f.write(f"使用伪控制台: 否\n")
        log_f.write(f"{'='*60}\n\n")

        # 启动两个线程分别读取 stdout 和 stderr
        stdout_thread = threading.Thread(target=read_output, args=(process.stdout, log_f, False), daemon=True)
        stderr_thread = threading.Thread(target=read_output, args=(process.stderr, log_f, True), daemon=True)
        stdout_thread.start()
        stderr_thread.start()

        # 等待进程结束
        return_code = process.wait()
        stop_event.set()

        # 等待所有输出被读取
        stdout_thread.join(timeout=2)
        stderr_thread.join(timeout=2)

        log_f.write(f"\n{'='*60}\n")
        log_f.write(f"构建结束时间: {datetime.now()}\n")
        log_f.write(f"退出码: {return_code}\n")

    return return_code


def main():
    parser = argparse.ArgumentParser(
        description="DuskAutoScript 构建脚本（使用伪控制台）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default="C:/vmbuild",
        help="构建目录路径"
    )
    parser.add_argument(
        "--target",
        type=str,
        default=None,
        help="构建目标（None表示构建全部）"
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="构建前先清理"
    )
    parser.add_argument(
        "--no-verbose",
        action="store_true",
        help="禁用详细输出（不推荐）"
    )

    args = parser.parse_args()

    # 创建配置
    config = BuildConfig()
    config.build_dir = Path(args.build_dir)
    config.verbose = not args.no_verbose

    try:
        # 运行构建
        return_code = run_cmake_build_with_pty(config, target=args.target, clean=args.clean)
        if return_code != 0:
            print(f"❌ 构建失败，退出码: {return_code}", return_code)

    except BuildError as e:
        print(f"❌ 构建错误: {e}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\n⚠️  用户中断")
    except Exception as e:
        print(f"❌ 未预期的错误: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    main()
