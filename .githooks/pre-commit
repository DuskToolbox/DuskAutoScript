#!/usr/bin/env python3
"""Pre-commit hook: 拒绝非 UTF-8 (含 BOM) 的代码文件提交"""

import os
import subprocess
import sys

# 需要检查编码的文件扩展名
CHECK_EXTENSIONS = {
    # C/C++
    ".h", ".hpp", ".hxx",
    ".cpp", ".cxx", ".cc", ".ixx", ".cppm",
    ".c",
    # CMake / Python
    ".cmake", ".txt",
    ".py",
}

BOM = b"\xef\xbb\xbf"


def get_staged_files():
    """获取暂存区中的文件列表"""
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("错误：无法获取暂存区文件列表", file=sys.stderr)
        sys.exit(1)
    return [f.strip() for f in result.stdout.strip().split("\n") if f.strip()]


def check_file(filepath):
    """检查文件编码，返回错误信息或 None"""
    try:
        with open(filepath, "rb") as f:
            raw = f.read()
    except OSError as e:
        return f"无法读取: {e}"

    if raw.startswith(BOM):
        return "UTF-8 BOM"

    try:
        raw.decode("utf-8")
    except UnicodeDecodeError:
        return "非 UTF-8 编码"

    return None


def main():
    staged = get_staged_files()
    if not staged:
        sys.exit(0)

    violations = []
    for filepath in staged:
        ext = os.path.splitext(filepath)[1].lower()
        if ext not in CHECK_EXTENSIONS:
            continue

        error = check_file(filepath)
        if error:
            violations.append((filepath, error))

    if violations:
        print("pre-commit: 以下文件编码不符合要求（仅允许 UTF-8 无 BOM）", file=sys.stderr)
        print("", file=sys.stderr)
        for filepath, reason in violations:
            print(f"  [{reason}] {filepath}", file=sys.stderr)
        print("", file=sys.stderr)
        print("请使用 tools/remove_bom.py 去除 BOM，或将文件转为 UTF-8 编码。", file=sys.stderr)
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
