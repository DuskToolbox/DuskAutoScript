#!/usr/bin/env python3
"""
DAS IDL 批量代码生成器

根据配置文件批量生成代码，支持多线程和多进程执行。

用法:
    python das_idl_batch_gen.py --config <json_config_file>

示例:
    python das_idl_batch_gen.py --config batch_config.json

JSON 配置格式:
    [
        {
            "-i": "path/to/interface1.idl",
            "--output-dir": "./generated/interface1",
            "--swig": true,
            "--cpp-wrapper": true
        },
        {
            "-i": "path/to/interface2.idl",
            "--output-dir": "./generated/interface2",
            "--all": true
        }
    ]
"""

import argparse
import json
import os
import sys
import subprocess
import time
import threading
from pathlib import Path
from typing import List, Dict, Any, Optional
from concurrent.futures import ThreadPoolExecutor, ProcessPoolExecutor, as_completed
from datetime import datetime

# 检测Python是否支持NO GIL (Python 3.13+ free-threaded build)


def _list_csharp_reduce_outputs(reduce_config: Dict[str, Any]) -> List[str]:
    """List reduce-owned C# package outputs for CMake dependency tracking."""
    csharp_output_dir = reduce_config.get("csharp_output_dir")
    csharp_namespace_root = reduce_config.get("csharp_namespace_root")
    csharp_idl_dir = reduce_config.get("csharp_idl_dir")
    csharp_idl_files = reduce_config.get("csharp_idl_files", [])

    if not (csharp_output_dir and csharp_namespace_root):
        return []

    outputs = [
        f"{csharp_output_dir}/{csharp_namespace_root}/AssemblyAttributes.cs",
        f"{csharp_output_dir}/{csharp_namespace_root}/DasResult.cs",
        f"{csharp_output_dir}/{csharp_namespace_root}/DasException.cs",
        f"{csharp_output_dir}/{csharp_namespace_root}/Abi/DasGuid.cs",
        f"{csharp_output_dir}/{csharp_namespace_root}/Abi/NativeMethods.cs",
        f"{csharp_output_dir}/{csharp_namespace_root}/Runtime/DasCSharpBootstrap.cs",
        f"{csharp_output_dir}/Native/DasCSharpDirectorSupport.h",
        f"{csharp_output_dir}/Native/DasCSharpDirectorSupport.cpp",
    ]

    if csharp_idl_dir and csharp_idl_files:
        from das_idl_parser import parse_idl_file

        idl_dir = Path(csharp_idl_dir)
        interface_names: set[str] = set()
        for idl_file in csharp_idl_files:
            document = parse_idl_file(str(idl_dir / idl_file))
            for interface in document.interfaces:
                interface_names.add(interface.name)

        for interface_name in sorted(interface_names):
            outputs.append(
                f"{csharp_output_dir}/{csharp_namespace_root}/Wrappers/{interface_name}.cs"
            )
            outputs.append(
                f"{csharp_output_dir}/{csharp_namespace_root}/Directors/{interface_name}Director.cs"
            )

    return outputs


def check_nogil_support() -> bool:
    """
    检测当前Python环境是否支持NO GIL

    NO GIL 需要 Python 3.13+ 的 free-threaded build 版本。
    普通的 Python 3.13+ 并不默认支持 NO GIL。

    Returns:
        bool: True表示当前GIL已禁用，False表示GIL仍然启用
    """
    # 方法1: 使用 sys._is_gil_enabled() (Python 3.13+ free-threaded build)
    # 这是最准确的检测方法
    if hasattr(sys, '_is_gil_enabled'):
        try:
            # 返回 False 表示 GIL 已禁用（即支持 NO GIL）
            return not sys._is_gil_enabled()
        except Exception:
            pass

    # 方法2: 检查 sysconfig 中的 Py_GIL_DISABLED 配置
    # 这可以检测是否为 free-threaded build
    try:
        import sysconfig
        config_vars = sysconfig.get_config_vars()
        if config_vars.get('Py_GIL_DISABLED'):
            return True
    except Exception:
        pass

    # 方法3: 检查 Python 实现名称是否包含 free-threaded 标识
    # 某些发行版可能在 sys.version 中标注
    if 'free-threading' in sys.version.lower() or 'nogil' in sys.version.lower():
        return True

    # 默认情况下，GIL 是启用的
    return False


class TaskTiming:
    """任务计时器"""

    def __init__(self, task_name: str, task_id: int):
        self.task_name = task_name
        self.task_id = task_id
        self.start_time: Optional[float] = None
        self.end_time: Optional[float] = None
        self.duration: Optional[float] = None
        self.status: str = "pending"
        self.error: Optional[str] = None
        self.worker_pid: int = 0

    def start(self):
        """开始计时"""
        self.start_time = time.time()
        self.status = "running"

    def finish(self, error: Optional[str] = None):
        """结束计时"""
        self.end_time = time.time()
        if self.start_time:
            self.duration = self.end_time - self.start_time
        self.status = "completed" if error is None else "failed"
        self.error = error

    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        return {
            "task_id": self.task_id,
            "task_name": self.task_name,
            "start_time": datetime.fromtimestamp(self.start_time).isoformat() if self.start_time else None,
            "end_time": datetime.fromtimestamp(self.end_time).isoformat() if self.end_time else None,
            "duration": f"{self.duration:.2f}s" if self.duration else None,
            "status": self.status,
            "error": self.error
        }


def execute_single_task(task_config: Dict[str, Any], task_id: int) -> TaskTiming:
    """
    执行单个代码生成任务

    Args:
        task_config: 任务配置字典
        task_id: 任务ID

    Returns:
        TaskTiming: 任务计时对象
    """
    input_file = task_config.get("-i", task_config.get("--input", ""))
    timing = TaskTiming(input_file, task_id)

    try:
        # 构建命令行参数
        cmd = [sys.executable, str(Path(__file__).parent / "das_idl_gen.py")]

        # 将配置转换为命令行参数
        for key, value in task_config.items():
            if value is True:
                cmd.append(key)
            elif value is False:
                continue
            else:
                cmd.append(key)
                cmd.append(str(value))

        timing.start()
        timing.worker_pid = os.getpid()

        # 执行命令
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=600  # 10分钟超时
        )

        if result.returncode != 0:
            error_msg = f"返回码: {result.returncode}"
            if result.stderr:
                error_msg += f"\n{result.stderr}"
            timing.finish(error_msg)
        else:
            timing.finish()

    except subprocess.TimeoutExpired:
        timing.finish("超时错误")
    except Exception as e:
        timing.finish(str(e))

    return timing


def generate_timeline(timings: List[TaskTiming], use_multiprocessing: bool) -> str:
    """生成 ASCII 任务执行时间线"""
    if not timings:
        return "无任务数据"

    valid = [t for t in timings if t.start_time is not None and t.end_time is not None]
    if not valid:
        return "无有效的任务时间数据"

    total_width = 110
    label_width = 40
    timeline_width = 54

    base_time = min(t.start_time for t in valid)
    wall_duration = max(t.end_time for t in valid) - base_time
    if wall_duration <= 0:
        wall_duration = 1.0

    # 将 worker PID 映射为顺序编号 W1, W2, ...
    unique_pids = sorted(set(t.worker_pid for t in valid if t.worker_pid))
    worker_map = {pid: i + 1 for i, pid in enumerate(unique_pids)}

    lines = []
    lines.append("=" * total_width)

    # --- 时间刻度尺 ---
    # 分 5 个等间隔
    num_intervals = 5
    tick_chars = ["|"] + ["-"] * (timeline_width // num_intervals)
    tick_unit = "".join(tick_chars)
    tick_line = tick_unit * num_intervals
    if len(tick_line) > timeline_width:
        tick_line = tick_line[:timeline_width]
    elif len(tick_line) < timeline_width:
        tick_line += "-" * (timeline_width - len(tick_line))

    # 时间标签行
    label_chars = [" "] * timeline_width
    for i in range(num_intervals + 1):
        pos = int(i / num_intervals * timeline_width)
        pos = min(pos, timeline_width - 1)
        t_val = i / num_intervals * wall_duration
        lbl = f"{t_val:.1f}s"
        start = max(0, pos - len(lbl) + 1)
        for j, ch in enumerate(lbl):
            idx = start + j
            if 0 <= idx < timeline_width:
                label_chars[idx] = ch

    lines.append(" " * label_width + "".join(label_chars))
    lines.append(" " * label_width + tick_line)

    # --- 任务行 ---
    for timing in timings:
        if timing.start_time is None or timing.end_time is None:
            continue

        # 左侧: T## filename
        filename = Path(timing.task_name).name if (
            "/" in timing.task_name or "\\" in timing.task_name
        ) else timing.task_name
        left = f"T{timing.task_id:02d} {filename}"
        if len(left) > label_width:
            left = left[:label_width - 2] + ".."
        left = f"{left:<{label_width}}"

        # 时间轴 # 条
        start_pos = int((timing.start_time - base_time) / wall_duration * timeline_width)
        bar_len = max(1, round((timing.duration or 0) / wall_duration * timeline_width))
        start_pos = max(0, min(start_pos, timeline_width - 1))
        bar_len = min(bar_len, timeline_width - start_pos)

        if timing.status == "completed":
            bar_char = "#"
        elif timing.status == "failed":
            bar_char = "X"
        else:
            bar_char = "!"

        bar = list(" " * timeline_width)
        for i in range(bar_len):
            idx = start_pos + i
            if 0 <= idx < timeline_width:
                bar[idx] = bar_char

        # 右侧: [+] 0.03s W2
        marker = "[+]" if timing.status == "completed" else "[-]" if timing.status == "failed" else "[T]"
        dur_str = f"{timing.duration:.2f}s" if timing.duration else "  N/A"
        w_num = worker_map.get(timing.worker_pid, 0)
        w_str = f"W{w_num}" if w_num > 0 else ""
        right = f"{marker} {dur_str:>6} {w_str}"

        lines.append(f"{left}{''.join(bar)}{right}")

    lines.append("-" * total_width)
    lines.append("Legend: [#] Success  [X] Failed  [!] Timeout")
    lines.append("        [+] OK  [-] Error  [T] Timeout")

    # 统计摘要
    completed = sum(1 for t in timings if t.status == "completed")
    failed = sum(1 for t in timings if t.status == "failed")
    cpu_time = sum(t.duration for t in timings if t.duration)
    speedup = cpu_time / wall_duration if wall_duration > 0 else 1.0
    lines.append(
        f"Total: {len(timings)} tasks, {completed} ok, {failed} failed, "
        f"CPU: {cpu_time:.2f}s, Wall: {wall_duration:.2f}s, Speedup: {speedup:.2f}x"
    )
    lines.append("")

    return "\n".join(lines)


def _format_task_line(timing: TaskTiming) -> str:
    """格式化单行任务状态，用于实时输出"""
    marker = "[+]" if timing.status == "completed" else "[-]" if timing.status == "failed" else "[T]"
    dur_str = f"{timing.duration:.2f}s" if timing.duration else "  N/A"
    filename = Path(timing.task_name).name if (
        "/" in timing.task_name or "\\" in timing.task_name
    ) else timing.task_name
    return f"{marker} {dur_str:>6}  T{timing.task_id:02d} {filename}"


def run_with_multiprocessing(tasks: List[Dict[str, Any]], max_workers: Optional[int] = None) -> List[TaskTiming]:
    """
    使用多进程执行任务

    Args:
        tasks: 任务配置列表
        max_workers: 最大并发数

    Returns:
        List[TaskTiming]: 任务计时列表
    """
    timings: List[TaskTiming] = []

    print(f"使用多进程模式 (max_workers={max_workers or os.cpu_count()})")

    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        # 提交所有任务
        future_to_task = {
            executor.submit(execute_single_task, task, task_id): task_id
            for task_id, task in enumerate(tasks)
        }

        # 等待任务完成
        for future in as_completed(future_to_task):
            task_id = future_to_task[future]
            try:
                timing = future.result()
                timings.append(timing)
                print(_format_task_line(timing))
            except Exception as e:
                print(f"[-]    N/A  T{task_id:02d} {tasks[task_id].get('-i', 'unknown')} - {e}")
                task_timing = TaskTiming(tasks[task_id].get("-i", "unknown"), task_id)
                task_timing.finish(str(e))
                timings.append(task_timing)

    # 按任务ID排序
    timings.sort(key=lambda t: t.task_id)
    return timings


def run_with_threading(tasks: List[Dict[str, Any]], max_workers: Optional[int] = None) -> List[TaskTiming]:
    """
    使用多线程执行任务

    Args:
        tasks: 任务配置列表
        max_workers: 最大并发数

    Returns:
        List[TaskTiming]: 任务计时列表
    """
    timings: List[TaskTiming] = []
    lock = threading.Lock()

    print(f"使用多线程模式 (max_workers={max_workers or os.cpu_count()})")

    def worker(task_config: Dict[str, Any], task_id: int) -> TaskTiming:
        timing = execute_single_task(task_config, task_id)
        with lock:
            timings.append(timing)
        return timing

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        # 提交所有任务
        future_to_task = {
            executor.submit(worker, task, task_id): task_id
            for task_id, task in enumerate(tasks)
        }

        # 等待任务完成
        for future in as_completed(future_to_task):
            task_id = future_to_task[future]
            try:
                timing = future.result()
                print(_format_task_line(timing))
            except Exception as e:
                print(f"[-]    N/A  T{task_id:02d} {tasks[task_id].get('-i', 'unknown')} - {e}")

    # 按任务ID排序
    timings.sort(key=lambda t: t.task_id)
    return timings


def main():
    parser = argparse.ArgumentParser(
        description='DAS IDL 批量代码生成器 - 根据配置文件批量生成代码',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s --config batch_config.json
  %(prog)s --config batch_config.json --workers 4

JSON 配置格式:
  [
      {
          "-i": "path/to/interface1.idl",
          "--output-dir": "./generated/interface1",
          "--swig": true,
          "--cpp-wrapper": true
      },
      {
          "-i": "path/to/interface2.idl",
          "--output-dir": "./generated/interface2",
          "--all": true
      }
  ]
"""
    )

    parser.add_argument(
        '-c', '--config',
        required=True,
        help='JSON配置文件路径'
    )

    parser.add_argument(
        '-w', '--workers',
        type=int,
        help='最大并发数（默认：CPU核心数）'
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='显示详细输出'
    )

    parser.add_argument(
        '--update-list',
        help='包含需要重新生成的IDL文件列表（每行一个），如果指定则只生成这些IDL'
    )

    parser.add_argument(
        '--ipc-cache-dir',
        help='IPC 中间缓存目录（用于批量生成时的 IPC 缓存共享）'
    )

    parser.add_argument(
        '--list-outputs',
        action='store_true',
        help='仅列出将生成的文件路径（每行一个），不实际生成文件。用于 CMake configure 阶段。'
    )

    args = parser.parse_args()

    # 读取配置文件
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"错误: 配置文件不存在: {config_path}", file=sys.stderr)
        return 1

    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            config = json.load(f)
    except json.JSONDecodeError as e:
        print(f"错误: 配置文件JSON格式错误: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"错误: 读取配置文件失败: {e}", file=sys.stderr)
        return 1

    # 从顶层结构提取 tasks 和 reduce 配置
    tasks = config.get("tasks", [])
    reduce_config = config.get("reduce", {})

    if not isinstance(tasks, list):
        print("错误: 配置文件必须包含任务列表", file=sys.stderr)
        return 1

    # ====== --list-outputs 模式：仅列出预期输出文件，不实际生成 ======
    if args.list_outputs:
        # 将 das_idl 目录加入 sys.path 以便 import
        sys.path.insert(0, str(Path(__file__).parent))
        from das_idl_gen import list_expected_outputs

        all_outputs = []
        for task in tasks:
            all_outputs.extend(list_expected_outputs(task))

        # 追加聚合文件路径（这些由 batch gen 的后处理步骤产出）
        swig_output_dir = None
        ipc_output_dir = None
        for task in tasks:
            if "--swig-output-dir" in task and swig_output_dir is None:
                swig_output_dir = task["--swig-output-dir"]
            if "--ipc-output-dir" in task and ipc_output_dir is None:
                ipc_output_dir = task["--ipc-output-dir"]

        if swig_output_dir:
            all_outputs.append(f"{swig_output_dir}/DasTypeMaps.i")
            all_outputs.append(f"{swig_output_dir}/DasTypeMapsExtend.i")
            all_outputs.append(f"{swig_output_dir}/DasTypeMapsIgnore.i")

        if ipc_output_dir:
            all_outputs.append(f"{ipc_output_dir}/IpcGenerated.cpp")
            all_outputs.append(f"{ipc_output_dir}/IpcAllProxies.h")
            all_outputs.append(f"{ipc_output_dir}/IpcAllStubs.h")
            if args.ipc_cache_dir:
                all_outputs.append(f"{ipc_output_dir}/IpcProxyFactory.h")
                all_outputs.append(f"{ipc_output_dir}/IpcStubFactory.h")
                all_outputs.append(f"{ipc_output_dir}/registry/ProxyRegistry.h")

        # 追加 Lua 聚合文件路径（由 reduce 阶段的 das_lua_export.py 产出）
        lua_output_dir = reduce_config.get("lua_output_dir")
        lua_name = reduce_config.get("lua_name")

        if lua_output_dir and lua_name:
            all_outputs.append(f"{lua_output_dir}/{lua_name}_lua_export.cpp")
            all_outputs.append(f"{lua_output_dir}/{lua_name}_lua_export.lua")

        # 追加 Node/NAPI 聚合文件路径（由 reduce 阶段的 das_napi_export.py 产出）
        node_output_dir = reduce_config.get("node_output_dir")
        node_addon_name = reduce_config.get("node_addon_name")

        if node_output_dir and node_addon_name:
            node_stem = f"{node_addon_name}_export"
            all_outputs.append(f"{node_output_dir}/package.json")
            all_outputs.append(f"{node_output_dir}/index.cjs")
            all_outputs.append(f"{node_output_dir}/index.d.ts")
            all_outputs.append(f"{node_output_dir}/bin/das-node-host.cjs")
            all_outputs.append(f"{node_output_dir}/{node_stem}.cpp")
            all_outputs.append(f"{node_output_dir}/{node_stem}.d.ts")
            all_outputs.append(f"{node_output_dir}/{node_stem}.js")

        all_outputs.extend(_list_csharp_reduce_outputs(reduce_config))

        # 输出到 stdout（每行一个路径），去重并排序，统一使用正斜杠
        for f in sorted(set(all_outputs)):
            print(f.replace("\\", "/"))
        return 0

    # 如果指定了更新列表，过滤任务
    if args.update_list:
        update_list_path = Path(args.update_list)
        if not update_list_path.exists():
            print(f"错误: 更新列表文件不存在: {update_list_path}", file=sys.stderr)
            return 1

        with open(update_list_path, 'r', encoding='utf-8') as f:
            # 规范化路径：将反斜杠转为正斜杠，去除末尾分隔符
            update_list = set(
                Path(line.strip()).as_posix()
                for line in f
                if line.strip()
            )

        # 过滤出需要更新的任务
        original_tasks = tasks
        tasks = []
        for task in original_tasks:
            # 规范化任务中的IDL路径
            task_idl = task.get("-i", task.get("--input", ""))
            if task_idl:
                normalized_task_idl = Path(task_idl).as_posix()
                if normalized_task_idl in update_list:
                    tasks.append(task)


        if args.verbose:
            print(f"根据更新列表过滤: {len(tasks)}/{len(original_tasks)} 个任务需要生成")
            if len(tasks) < len(original_tasks):
                print("  跳过的任务:")
                for task in original_tasks:
                    task_idl = task.get("-i", task.get("--input", ""))
                    if task_idl:
                        normalized_task_idl = Path(task_idl).as_posix()
                        if normalized_task_idl not in update_list:
                            print(f"    - {task_idl}")

    if len(tasks) == 0:
        print("没有任务需要生成")
        return 0

    # 如果指定了全局 IPC 缓存目录，添加到每个任务配置
    if args.ipc_cache_dir:
        for task in tasks:
            if "--ipc-cache-dir" not in task:
                task["--ipc-cache-dir"] = args.ipc_cache_dir

    print(f"从配置文件加载了 {len(tasks)} 个任务")
    print(f"配置文件: {config_path}")

    # 检测Python是否支持NO GIL
    has_nogil = check_nogil_support()
    print(f"\nPython版本: {sys.version}")
    print(f"NO GIL支持: {'是' if has_nogil else '否'}")

    # 根据NO GIL支持情况选择执行模式
    start_time = time.time()

    if has_nogil:
        print("\n使用多线程模式执行任务（支持NO GIL）")
        timings = run_with_threading(tasks, args.workers)
        use_multiprocessing = False
    else:
        print("\n使用多进程模式执行任务（不支持NO GIL）")
        timings = run_with_multiprocessing(tasks, args.workers)
        use_multiprocessing = True

    total_duration = time.time() - start_time

    # 打印任务执行时间线
    print(f"\nTask Execution Timeline")
    print(generate_timeline(timings, use_multiprocessing))

    # 返回状态
    failed_count = sum(1 for t in timings if t.status == "failed")
    batch_result = 1 if failed_count > 0 else 0

    # ====== 执行 Typemap 聚合 ======
    # 只有在没有失败任务时才执行聚合
    reduce_phases = []  # 收集 reduce 各阶段计时

    if batch_result == 0:
        # 从第一个任务获取SWIG输出目录（所有任务的输出目录应该一致）
        if tasks and len(tasks) > 0:
            # 获取SWIG输出目录
            swig_output_dir = None
            
            # 从任务配置中提取SWIG输出目录
            # 配置中使用 --swig-output-dir 指定SWIG文件输出目录
            for task in tasks:
                if "--swig-output-dir" in task:
                    swig_output_dir = Path(task["--swig-output-dir"])
                    break
            
            if swig_output_dir and swig_output_dir.exists():
                aggregate_script = Path(__file__).parent / "aggregate_typemaps.py"
                if aggregate_script.exists():
                    cmd = [
                        sys.executable,
                        str(aggregate_script),
                        "--swig-output-dir", str(swig_output_dir),
                        "--output-file", "DasTypeMaps.i"
                    ]
                    
                    try:
                        t0 = time.time()
                        result = subprocess.run(
                            cmd,
                            capture_output=True,
                            text=True,
                            timeout=30
                        )
                        dt = time.time() - t0
                        
                        if result.returncode == 0:
                            reduce_phases.append(("Typemap", dt, "ok", result.stdout.strip()))
                        else:
                            reduce_phases.append(("Typemap", dt, "failed", result.stderr.strip()))
                            print(f"[警告] Typemap聚合失败，但IDL生成成功", file=sys.stderr)
                    except Exception as e:
                        reduce_phases.append(("Typemap", 0.0, "error", str(e)))
                        print(f"[警告] Typemap聚合出错，但IDL生成成功", file=sys.stderr)
        
        # ====== 执行 IPC Registry 生成 ======
        ipc_output_dir = None
        for task in tasks:
            if "--ipc-output-dir" in task:
                ipc_output_dir = Path(task["--ipc-output-dir"])
                break
            elif "-o" in task:
                ipc_output_dir = Path(task["-o"])
                break
        
        if args.ipc_cache_dir and batch_result == 0 and ipc_output_dir:
            registry_script = Path(__file__).parent / "das_ipc_registry_generator.py"
            if registry_script.exists():
                cmd = [
                    sys.executable,
                    str(registry_script),
                    "--cache-dir", args.ipc_cache_dir,
                    "--output-dir", str(ipc_output_dir)
                ]
                
                try:
                    t0 = time.time()
                    result = subprocess.run(
                        cmd,
                        capture_output=True,
                        text=True,
                        timeout=30
                    )
                    dt = time.time() - t0
                    
                    if result.returncode == 0:
                        reduce_phases.append(("IPC Registry", dt, "ok", result.stdout.strip()))
                    else:
                        reduce_phases.append(("IPC Registry", dt, "failed", result.stderr.strip()))
                        batch_result = 1
                except Exception as e:
                    reduce_phases.append(("IPC Registry", 0.0, "error", str(e)))
                    batch_result = 1
        
        # ====== 执行 IPC Proxy/Stub 聚合 ======
        if ipc_output_dir and batch_result == 0:
            ipc_aggregate_script = Path(__file__).parent / "aggregate_ipc.py"
            if ipc_aggregate_script.exists():
                cmd = [
                    sys.executable,
                    str(ipc_aggregate_script),
                    "--ipc-output-dir", str(ipc_output_dir),
                ]
                if args.ipc_cache_dir:
                    cmd.extend(["--ipc-cache-dir", str(args.ipc_cache_dir)])

                try:
                    t0 = time.time()
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
                    dt = time.time() - t0

                    if result.returncode == 0:
                        reduce_phases.append(("IPC Aggregate", dt, "ok", result.stdout.strip()))
                    else:
                        reduce_phases.append(("IPC Aggregate", dt, "failed", result.stderr.strip()))
                        batch_result = 1
                except Exception as e:
                    reduce_phases.append(("IPC Aggregate", 0.0, "error", str(e)))
                    batch_result = 1

        # ====== 执行 Lua 聚合 ======
        lua_output_dir = reduce_config.get("lua_output_dir")
        lua_name = reduce_config.get("lua_name")
        lua_open_module_name = reduce_config.get("lua_open_module_name")
        lua_idl_dir = reduce_config.get("lua_idl_dir")
        lua_idl_files = reduce_config.get("lua_idl_files", [])
        # --export-c-macro 从 tasks[0] 获取（所有 task 共享同一值）
        lua_export_c_macro = tasks[0].get("--export-c-macro") if tasks else None

        if lua_output_dir and lua_name and batch_result == 0:
            if not lua_open_module_name:
                print(
                    "错误: Lua reduce config missing lua_open_module_name",
                    file=sys.stderr,
                )
                batch_result = 1
            else:
                lua_script = Path(__file__).parent / "das_lua_export.py"
                if lua_script.exists():
                    cmd = [
                        sys.executable,
                        str(lua_script),
                        "--idl-dir", lua_idl_dir,
                        "--output", lua_output_dir,
                        "--name", lua_name,
                        "--open-module-name", lua_open_module_name,
                        "--export-c-macro", lua_export_c_macro or "DAS_C_API",
                        "--idl-files", *lua_idl_files,
                    ]

                    try:
                        t0 = time.time()
                        result = subprocess.run(
                            cmd,
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        dt = time.time() - t0

                        if result.returncode == 0:
                            reduce_phases.append(
                                ("Lua Binding", dt, "ok", result.stdout.strip())
                            )
                        else:
                            reduce_phases.append(
                                ("Lua Binding", dt, "failed", result.stderr.strip())
                            )
                            batch_result = 1
                    except Exception as e:
                        reduce_phases.append(("Lua Binding", 0.0, "error", str(e)))
                        batch_result = 1

        # ====== 执行 Node/NAPI 聚合 ======
        node_fields = {
            "node_output_dir": reduce_config.get("node_output_dir"),
            "node_package_name": reduce_config.get("node_package_name"),
            "node_addon_name": reduce_config.get("node_addon_name"),
            "node_idl_dir": reduce_config.get("node_idl_dir"),
            "node_idl_files": reduce_config.get("node_idl_files", []),
        }
        node_reduce_requested = any(node_fields.values())

        if node_reduce_requested and batch_result == 0:
            missing_node_fields = [
                name for name, value in node_fields.items() if not value
            ]
            if missing_node_fields:
                print(
                    "错误: Node reduce config missing "
                    + ", ".join(missing_node_fields),
                    file=sys.stderr,
                )
                batch_result = 1
            else:
                node_script = Path(__file__).parent / "das_napi_export.py"
                if node_script.exists():
                    cmd = [
                        sys.executable,
                        str(node_script),
                        "--idl-dir",
                        node_fields["node_idl_dir"],
                        "--output",
                        node_fields["node_output_dir"],
                        "--package-name",
                        node_fields["node_package_name"],
                        "--addon-name",
                        node_fields["node_addon_name"],
                        "--idl-files",
                        *node_fields["node_idl_files"],
                    ]

                    try:
                        t0 = time.time()
                        result = subprocess.run(
                            cmd,
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        dt = time.time() - t0

                        if result.returncode == 0:
                            reduce_phases.append(
                                (
                                    "Node NAPI Binding",
                                    dt,
                                    "ok",
                                    result.stdout.strip(),
                                )
                            )
                        else:
                            reduce_phases.append(
                                (
                                    "Node NAPI Binding",
                                    dt,
                                    "failed",
                                    result.stderr.strip(),
                                )
                            )
                            batch_result = result.returncode or 1
                    except Exception as e:
                        reduce_phases.append(
                            ("Node NAPI Binding", 0.0, "error", str(e))
                        )
                        batch_result = 1
                else:
                    print(
                        f"错误: Node reduce script missing: {node_script}",
                        file=sys.stderr,
                    )
                    batch_result = 1

        # ====== 执行 C# 聚合 ======
        csharp_fields = {
            "csharp_output_dir": reduce_config.get("csharp_output_dir"),
            "csharp_namespace_root": reduce_config.get("csharp_namespace_root"),
            "csharp_das_native_module_name": reduce_config.get(
                "csharp_das_native_module_name"
            ),
            "csharp_native_support_module_name": reduce_config.get(
                "csharp_native_support_module_name"
            ),
            "csharp_idl_dir": reduce_config.get("csharp_idl_dir"),
            "csharp_idl_files": reduce_config.get("csharp_idl_files", []),
        }
        csharp_reduce_requested = any(csharp_fields.values())

        if csharp_reduce_requested and batch_result == 0:
            missing_csharp_fields = [
                name for name, value in csharp_fields.items() if not value
            ]
            if missing_csharp_fields:
                print(
                    "错误: C# reduce config missing "
                    + ", ".join(missing_csharp_fields),
                    file=sys.stderr,
                )
                batch_result = 1
            else:
                csharp_script = Path(__file__).parent / "das_csharp_export.py"
                if csharp_script.exists():
                    cmd = [
                        sys.executable,
                        str(csharp_script),
                        "--idl-dir",
                        csharp_fields["csharp_idl_dir"],
                        "--output",
                        csharp_fields["csharp_output_dir"],
                        "--namespace-root",
                        csharp_fields["csharp_namespace_root"],
                        "--das-native-module-name",
                        csharp_fields["csharp_das_native_module_name"],
                        "--native-support-module-name",
                        csharp_fields["csharp_native_support_module_name"],
                        "--idl-files",
                        *csharp_fields["csharp_idl_files"],
                    ]

                    try:
                        t0 = time.time()
                        result = subprocess.run(
                            cmd,
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )
                        dt = time.time() - t0

                        if result.returncode == 0:
                            reduce_phases.append(
                                ("CSharp Binding", dt, "ok", result.stdout.strip())
                            )
                        else:
                            reduce_phases.append(
                                (
                                    "CSharp Binding",
                                    dt,
                                    "failed",
                                    result.stderr.strip(),
                                )
                            )
                            batch_result = result.returncode or 1
                    except Exception as e:
                        reduce_phases.append(("CSharp Binding", 0.0, "error", str(e)))
                        batch_result = 1
                else:
                    print(
                        f"错误: C# reduce script missing: {csharp_script}",
                        file=sys.stderr,
                    )
                    batch_result = 1

    # ====== 打印 Reduce 阶段汇总 ======
    if reduce_phases:
        print(f"\nReduce Phase Timeline")
        print("=" * 72)
        print(f"  {'Phase':<20} {'Time':>8}   {'Status':<8}  Detail")
        print("-" * 72)
        reduce_total = 0.0
        for name, dt, status, detail in reduce_phases:
            reduce_total += dt
            marker = "[+]" if status == "ok" else "[-]"
            detail_short = detail.split("\n")[0][:30] if detail else ""
            print(f"  {marker} {name:<18} {dt:>7.3f}s   {'OK' if status == 'ok' else 'FAIL':<8}  {detail_short}")
        print("-" * 72)
        print(f"  {'Reduce Total':<20} {reduce_total:>7.3f}s")
        print("=" * 72)

    return batch_result


if __name__ == '__main__':
    sys.exit(main())
