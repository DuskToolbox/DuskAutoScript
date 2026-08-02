#!/usr/bin/env python3
"""Pre-commit hook: 拒绝非 UTF-8 (含 BOM) 的代码文件提交 + 新增行审查报告编号检查"""

import os
import re
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

# 代码新增行中的审查报告编号引用（如类别前缀 C/R/W/I 后接 "-" 与数字）。
# 报告编号是过程产物，不应以注释形式残留在代码里。
REVIEW_FINDING_REF = re.compile(r"\b(CR|WR|IN)-[0-9]+\b")


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

    # 检测暂存新增行中的 review finding 编号引用（仅新增行，不误伤既有代码）
    finding_violations = []
    for filepath in staged:
        ext = os.path.splitext(filepath)[1].lower()
        if ext not in CHECK_EXTENSIONS:
            continue

        result = subprocess.run(
            ["git", "diff", "--cached", "-U0", "--", filepath],
            capture_output=True, text=True,
            encoding="utf-8", errors="replace"
        )
        if result.returncode != 0:
            continue

        for lineno, line in enumerate(result.stdout.split("\n"), start=1):
            if not line.startswith("+"):
                continue
            stripped = line[1:]
            if stripped.startswith("+++"):
                continue
            if REVIEW_FINDING_REF.search(stripped):
                finding_violations.append((filepath, stripped.strip()))

    if finding_violations:
        print("pre-commit: 新增代码含审查报告编号引用（类别前缀后接短横线与数字）", file=sys.stderr)
        print("审查报告编号是过程产物，不应以注释形式残留在代码里。", file=sys.stderr)
        print("", file=sys.stderr)
        for filepath, line in finding_violations:
            print(f"  {filepath}: {line}", file=sys.stderr)
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
