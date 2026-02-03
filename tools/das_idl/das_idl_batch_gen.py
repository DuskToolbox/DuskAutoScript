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


def generate_gantt_chart(timings: List[TaskTiming], use_multiprocessing: bool) -> str:
    """
    生成ASCII格式的甘特图

    Args:
        timings: 任务计时列表
        use_multiprocessing: 是否使用多进程

    Returns:
        str: ASCII格式的甘特图
    """
    if not timings:
        return "无任务数据"

    execution_mode = "多进程" if use_multiprocessing else "多线程"

    # 计算总时间
    valid_timings = [t for t in timings if t.start_time is not None and t.end_time is not None]
    if not valid_timings:
        return "无有效的任务时间数据"

    total_duration = max(t.end_time for t in valid_timings) - min(t.start_time for t in valid_timings)
    base_time = min(t.start_time for t in valid_timings)

    # 计算甘特图宽度
    chart_width = 90  # ASCII图宽度
    time_scale = chart_width / total_duration if total_duration > 0 else 1

    # 构建图表
    chart_lines = []

    # 检测任务并行情况
    # 为每个时间点统计正在运行的任务数
    time_resolution = 20  # 时间采样点数
    parallel_matrix = []
    for i in range(time_resolution + 1):
        check_time = base_time + (total_duration * i / time_resolution)
        running_count = 0
        for t in valid_timings:
            if t.start_time <= check_time <= t.end_time:
                running_count += 1
        parallel_matrix.append(running_count)

    # 时间轴和刻度
    chart_lines.append("  ┌" + "─" * chart_width + "┐")

    # 分成10个区间，每个区间9个字符宽（90/10）
    interval_width = chart_width // 10

    # 重新设计时间轴
    # 竖线位置：0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90（共11个竖线）
    # chart_width=90，竖线间距=9

    # 生成百分比行（先创建一个chart_width+2长的列表）
    percent_list = list(" " * (chart_width + 2))
    percent_list[0] = "│"  # 左边框
    percent_list[-1] = "│"  # 右边框

    # 在竖线位置放置百分比（右对齐到竖线位置）
    for i in range(11):
        pos = i * interval_width  # 竖线在chart内的位置：0, 9, 18, ..., 90
        pct = int((pos / chart_width) * 100)  # 百分比：0, 10, 20, ..., 100
        pct_str = f"{pct}%"

        # 百分比字符串放在竖线位置（%在竖线上）
        # 如果是100%，放在倒数第二个位置
        if i == 10:  # 最后一个100%
            # 放在末尾竖线前
            start_pos = chart_width + 1 - len(pct_str)
            for j, ch in enumerate(pct_str):
                if start_pos + j < chart_width + 1:
                    percent_list[start_pos + j] = ch
        else:
            # 其他百分比放在竖线位置（%在竖线上）
            start_pos = pos + 1 - len(pct_str)
            if start_pos >= 1:  # 确保不覆盖左边框
                for j, ch in enumerate(pct_str):
                    if start_pos + j < chart_width + 1:
                        percent_list[start_pos + j] = ch

    percent_line = "".join(percent_list)
    chart_lines.append("  " + percent_line)

    # 生成标记线
    # marker_list: 索引0是左边框│，索引1-90是内容区域，索引91是右边框│
    marker_list = ["│"] + ["─"] * chart_width + ["│"]
    # 在位置 9, 18, 27, 36, 45, 54, 63, 72, 81 放置竖线（共9个内部竖线）
    for i in range(1, 10):  # 1到9，对应位置 9, 18, ..., 81
        pos = i * interval_width  # 9, 18, 27, ..., 81
        if pos < chart_width:  # 确保不超出范围
            marker_list[pos] = "│"

    marker_line = "".join(marker_list)
    chart_lines.append("  " + marker_line)
    chart_lines.append("  ├" + "─" * chart_width + "┤")

    # 任务条
    for idx, timing in enumerate(timings):
        if timing.start_time is None or timing.end_time is None:
            continue

        # 计算起始位置和长度
        start_offset = (timing.start_time - base_time) * time_scale
        duration = (timing.end_time - timing.start_time) * time_scale

        # 计算该任务执行期间的最大并行数
        start_sample_idx = int((timing.start_time - base_time) / total_duration * time_resolution)
        end_sample_idx = int((timing.end_time - base_time) / total_duration * time_resolution)
        start_sample_idx = max(0, min(start_sample_idx, time_resolution))
        end_sample_idx = max(0, min(end_sample_idx, time_resolution))

        max_parallel = max(parallel_matrix[start_sample_idx:end_sample_idx+1]) if end_sample_idx >= start_sample_idx else 1
        is_parallel = max_parallel > 1

        # 构建任务名称（只显示文件名部分）
        task_name = Path(timing.task_name).name if "/" in timing.task_name or "\\" in timing.task_name else timing.task_name
        task_name = task_name[:25]

        # 状态符号
        status_symbol = "✓" if timing.status == "completed" else "✗"
        task_id_str = f"#{timing.task_id + 1}"

        # 左侧标签
        left_label = f"  {task_id_str} {status_symbol} {task_name:<25}"
        left_label = left_label[:30]

        # 构建进度条
        bar_prefix = " " * int(start_offset)

        # 根据状态和并行情况选择不同的填充字符
        if timing.status == "completed":
            if is_parallel and duration > 5:
                # 并行任务使用特殊填充
                bar_char = "#"
            else:
                bar_char = "■"
        else:
            bar_char = "□"

        bar = bar_char * int(duration)
        bar_line = bar_prefix + bar

        # 计算剩余空间
        used_width = len(bar_prefix) + len(bar)
        remaining = chart_width - used_width
        if remaining > 0:
            bar_line += " " * remaining

        # 组合输出
        chart_lines.append(f"  │{bar_line}│")

        # 在条形图下方显示耗时
        if duration > 3:  # 只有足够长的任务才显示耗时
            time_label = "  │"
            time_label += " " * int(start_offset)
            time_label += f"{timing.duration:.2f}s"
            time_label += " " * (chart_width - len(time_label) + 3)
            time_label += "│"
            chart_lines.append(time_label)

    chart_lines.append("  └" + "─" * chart_width + "┘")

    # 图例
    chart_lines.append("  ───────────────────────────────────────────────────────────────")
    chart_lines.append("     ■ 任务执行中（已完成）   # 任务并行执行中   □ 任务执行失败")
    chart_lines.append("  ───────────────────────────────────────────────────────────────")
    chart_lines.append("")

    return "\n".join(chart_lines)


def print_task_timings(timings: List[TaskTiming]):
    """
    打印任务耗时列表

    Args:
        timings: 任务计时列表
    """
    print(f"\n{'='*100}")
    print(f"任务耗时详情")
    print(f"{'='*100}")
    print(f"{'ID':<4} {'状态':<10} {'耗时':<12} {'文件名'}")
    print(f"{'-'*100}")

    for timing in timings:
        status_symbol = "OK" if timing.status == "completed" else "FAIL"
        duration_str = f"{timing.duration:.2f}s" if timing.duration else "N/A"

        print(f"{timing.task_id:<4} {status_symbol} {timing.status:<9} {duration_str:<12} {timing.task_name}")

        if timing.error:
            print(f"     错误: {timing.error[:80]}")
            if len(timing.error) > 80:
                print(f"           ...")

    # 统计 - 纯文本格式
    completed = sum(1 for t in timings if t.status == "completed")
    failed = sum(1 for t in timings if t.status == "failed")
    total_duration = max(t.end_time for t in timings if t.end_time) - min(t.start_time for t in timings if t.start_time)
    total_cpu_time = sum(t.duration for t in timings if t.duration)

    print(f"\n{'='*100}")
    print(f"统计摘要")
    print(f"{'='*100}")
    print(f"总任务数: {len(timings)}, 成功: {completed}, 失败: {failed}")
    print(f"任务总耗时: {total_cpu_time:.2f}s, 总耗时: {total_duration:.2f}s, 加速比: {total_cpu_time / total_duration:.2f}x")
    print(f"{'='*100}")


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
                print(f"[{task_id + 1}/{len(tasks)}] {timing.task_name} - {timing.status} ({timing.duration:.2f}s)")
            except Exception as e:
                print(f"任务 {task_id} 执行异常: {e}")
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
                print(f"[{task_id + 1}/{len(tasks)}] {timing.task_name} - {timing.status} ({timing.duration:.2f}s)")
            except Exception as e:
                print(f"任务 {task_id} 执行异常: {e}")

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

    args = parser.parse_args()

    # 读取配置文件
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"错误: 配置文件不存在: {config_path}", file=sys.stderr)
        return 1

    try:
        with open(config_path, 'r', encoding='utf-8') as f:
            tasks = json.load(f)
    except json.JSONDecodeError as e:
        print(f"错误: 配置文件JSON格式错误: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"错误: 读取配置文件失败: {e}", file=sys.stderr)
        return 1

    if not isinstance(tasks, list):
        print("错误: 配置文件必须包含任务列表", file=sys.stderr)
        return 1

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

    # 打印任务耗时详情
    print_task_timings(timings)

    # 生成甘特图
    print(generate_gantt_chart(timings, use_multiprocessing))

    # 返回状态
    failed_count = sum(1 for t in timings if t.status == "failed")
    batch_result = 1 if failed_count > 0 else 0

    # ====== 执行 Typemap 聚合 ======
    # 只有在没有失败任务时才执行聚合
    if batch_result == 0:
        # 从第一个任务获取SWIG输出目录（所有任务的输出目录应该一致）
        if tasks and len(tasks) > 0:
            # 获取SWIG输出目录
            swig_output_dir = None
            first_task = tasks[0]
            
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
                        result = subprocess.run(
                            cmd,
                            capture_output=True,
                            text=True,
                            timeout=30
                        )
                        
                        if result.returncode == 0:
                            print(f"\n[Typemap聚合] {result.stdout.strip()}")
                        else:
                            print(f"\n[Typemap聚合失败] {result.stderr}", file=sys.stderr)
                            # Typemap聚合失败不视为致命错误，但记录警告
                            print(f"[警告] Typemap聚合失败，但IDL生成成功", file=sys.stderr)
                    except Exception as e:
                        print(f"\n[Typemap聚合错误] {e}", file=sys.stderr)
                        print(f"[警告] Typemap聚合出错，但IDL生成成功", file=sys.stderr)
    
    return batch_result


if __name__ == '__main__':
    sys.exit(main())
