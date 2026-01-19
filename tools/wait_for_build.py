#!/usr/bin/env python3
"""
等待构建锁文件消失的脚本

如果指定文件不存在，立即返回
如果文件存在，则轮询直到文件消失，轮询间隔为1秒
"""

import argparse
import os
import sys
import time


def wait_for_file_disappear(file_path: str, poll_interval: float = 1.0) -> None:
    """
    等待指定文件消失

    Args:
        file_path: 要等待的文件路径
        poll_interval: 轮询间隔（秒），默认1秒

    Returns:
        None
    """
    abs_path = os.path.abspath(file_path)

    # 如果文件不存在，立即返回
    if not os.path.exists(abs_path):
        print(f"File does not exist, returning immediately: {abs_path}")
        return

    print(f"Waiting for file to disappear: {abs_path}")

    # 轮询直到文件消失
    while os.path.exists(abs_path):
        time.sleep(poll_interval)

    print(f"File has disappeared: {abs_path}")


def main():
    parser = argparse.ArgumentParser(
        description="等待构建锁文件消失",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "file_path",
        help="要等待的文件路径",
    )
    parser.add_argument(
        "--poll-interval",
        type=float,
        default=1.0,
        help="轮询间隔（秒）",
    )

    args = parser.parse_args()

    try:
        wait_for_file_disappear(args.file_path, args.poll_interval)
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
