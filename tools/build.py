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
    """Build error exception"""
    pass


class BuildConfig:
    """Build configuration"""
    def __init__(self):
        self.source_dir = Path(__file__).parent
        self.build_dir = Path("C:/vmbuild")
        self.verbose = True
        self.log_dir = self.source_dir / "build_logs"


def run_cmake_build_with_pty(config, target=None, clean=False):
    """Run CMake build with pseudo-console

    Args:
        config: Build configuration
        target: Build target (None for all)
        clean: Clean before build

    Returns:
        Build exit code

    Raises:
        BuildError: Build failed
    """
    build_dir = config.build_dir

    if not build_dir.exists():
        raise BuildError(f"Build directory does not exist: {build_dir}")

    # Ensure log directory exists
    config.log_dir.mkdir(parents=True, exist_ok=True)

    # Create log file
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = config.log_dir / f"build_{timestamp}.log"

    # Check if VS environment needs configuration
    vs_script, arch = vs_env.get_vs_env_from_build_dir(build_dir)

    if vs_script:
        print(f"Detected MSVC compiler, will use VS environment script: {vs_script}")
        print(f"Target architecture: {arch}\n")

        # For MSVC, create temporary batch file to set VS environment and run build
        batch_file = config.log_dir / f"build_{timestamp}.bat"

        cmake_args = ['cmake', '--build', str(build_dir), '--config', 'Debug']
        if target:
            cmake_args.extend(['--target', target])
        if config.verbose:
            cmake_args.append('--verbose')

        build_cmd = ' '.join(cmake_args)

         # Write to batch file
        with open(batch_file, 'w', encoding='utf-8') as f:
            f.write('@echo off\n')
            f.write(f'call "{vs_script}" -arch={arch}\n')
            f.write(f'cd /d "{build_dir}"\n')
            f.write(f'{build_cmd}\n')
            f.write(f'exit /b %errorlevel%\n')

        # Execute batch file
        cmd = ['cmd.exe', '/c', str(batch_file)]

        print(f"\n{'='*60}")
        print(f"Starting build (with VS environment)")
        print(f"{'='*60}\n")
        print(f"Build log: {log_file}")
        print(f"Batch file: {batch_file}\n")

        try:
            return _run_without_pty(cmd, log_file, build_dir)
        finally:
            # 清理临时批处理文件
            if batch_file.exists():
                batch_file.unlink()

    # 如果需要清理
    if clean:
        print("Cleaning build directory...")
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
    print(f"Starting build: {' '.join(cmd)}")
    print(f"{'='*60}\n")
    print(f"Build log: {log_file}")

    # Use winpty to capture console output
    return _run_without_pty(cmd, log_file, build_dir)

def _run_without_pty(cmd, log_file, build_dir):
    """Run command without pseudo-console (fallback)"""
    # Create environment variables, disable color output and output buffering
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

                # Filter out "Note: including file:" messages (both Chinese and English)
                if decoded_line.strip().startswith("注意: 包含文件:"):
                    continue
                if decoded_line.strip().startswith("Note: including file:"):
                    continue

                log_f.write(f"{prefix}{decoded_line}")
                log_f.flush()
                try:
                    print(f"{prefix}{decoded_line}", end='', flush=True)
                except UnicodeEncodeError:
                    safe_line = decoded_line.encode('utf-8', errors='replace').decode('utf-8')
                    print(f"{prefix}{safe_line}", end='', flush=True)
                output_queue.put((is_stderr, decoded_line))
            except Exception as e:
                print(f"\nError reading output: {e}")
                break

    with open(log_file, "w", encoding="utf-8", buffering=1) as log_f:
        log_f.write(f"Build start time: {datetime.now()}\n")
        log_f.write(f"Command: {' '.join(cmd)}\n")
        log_f.write(f"Working directory: {os.getcwd()}\n")
        log_f.write(f"Using pseudo-console: no\n")
        log_f.write(f"{'='*60}\n\n")

        # Start two threads to read stdout and stderr
        stdout_thread = threading.Thread(target=read_output, args=(process.stdout, log_f, False), daemon=True)
        stderr_thread = threading.Thread(target=read_output, args=(process.stderr, log_f, True), daemon=True)
        stdout_thread.start()
        stderr_thread.start()

        # Wait for process to finish
        return_code = process.wait()
        stop_event.set()

        # Wait for all output to be read
        stdout_thread.join(timeout=2)
        stderr_thread.join(timeout=2)

        log_f.write(f"\n{'='*60}\n")
        log_f.write(f"Build end time: {datetime.now()}\n")
        log_f.write(f"Exit code: {return_code}\n")

    return return_code


def main():
    parser = argparse.ArgumentParser(
        description="DuskAutoScript build script (with pseudo-console)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--build-dir",
        type=str,
        default="C:/vmbuild",
        help="Build directory path"
    )
    parser.add_argument(
        "--target",
        type=str,
        default=None,
        help="Build target (None for all)"
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Clean before build"
    )
    parser.add_argument(
        "--no-verbose",
        action="store_true",
        help="Disable verbose output (not recommended)"
    )

    args = parser.parse_args()

    # Create configuration
    config = BuildConfig()
    config.build_dir = Path(args.build_dir)
    config.verbose = not args.no_verbose

    try:
        # Run build
        return_code = run_cmake_build_with_pty(config, target=args.target, clean=args.clean)
        if return_code != 0:
            print(f"[X] 构建失败，Exit code: {return_code}", return_code)

    except BuildError as e:
        print(f"[X] Build error: {e}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\n[!]  User interrupted")
    except Exception as e:
        print(f"[X] Unexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    main()
