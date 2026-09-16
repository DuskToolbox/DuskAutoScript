#!/usr/bin/env python3
"""Pre-commit hook: 拒绝非 UTF-8 (含 BOM) 的代码文件提交 + 新增行审查报告编号检查 + C++ 命名风格检查 + 路径文本编码检查"""

import os
import re
import subprocess
import sys
from pathlib import Path

# 项目根（.githooks 的上一级）
ROOT = Path(__file__).resolve().parent.parent

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

# 代码新增行中的过程产物编号引用：review finding 编号、决策 ID、phase 编号、plan 编号。
# 这些编号是过程产物，不应以注释形式残留在代码里（与 .githooks/commit-msg 对齐）。
# 决策 ID 的 D 前置 \b：D 是十六进制字符，GUID 字面量（68F1000D-0000-...）
# 天然含 D-数字-数字 形——决策 ID 的 D 前恒为词边界（空格/括号），GUID 内
# D 前恒为数字（非边界），以此区分两者。
REVIEW_FINDING_REF = re.compile(
    r"\b(CR|WR|IN)-[0-9]+\b"
    r"|\bD-[0-9]+(\.[0-9]+)?-[0-9]+"
    r"|(?:Phase|phase|Plan|plan)[ \t]+[0-9]+"
)


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
        print("pre-commit: 新增代码含过程产物编号引用（review finding、决策 ID、phase、plan 编号）", file=sys.stderr)
        print("这些编号是过程产物，不应以注释形式残留在代码里。", file=sys.stderr)
        print("", file=sys.stderr)
        for filepath, line in finding_violations:
            print(f"  {filepath}: {line}", file=sys.stderr)
        sys.exit(1)

    # C++ 命名风格检查（clang-tidy readability-identifier-naming，仅查暂存改动行）
    naming = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "check_cpp_naming.py"), "--staged"]
    )
    if naming.returncode != 0:
        sys.exit(naming.returncode)

    # 路径文本编码检查（libclang AST 级 path 窄串审计，仅查暂存改动行；
    # 非强制——环境由使用者保证，缺 libclang/编译库时脚本自身 warning 放行）。
    # DAS_PYTHON 指向 DAS 工具链 python（当前需带 clang binding），设了且
    # 存在即替换默认解释器——PATH python 无 clang binding 时靠它接通本道。
    das_python = os.environ.get("DAS_PYTHON")
    path_text_interpreter = (
        das_python if das_python and os.path.isfile(das_python) else sys.executable
    )
    path_text = subprocess.run(
        [path_text_interpreter, str(ROOT / "tools" / "check_path_text_encoding.py"), "--staged"]
    )
    if path_text.returncode != 0:
        sys.exit(path_text.returncode)

    sys.exit(0)


if __name__ == "__main__":
    main()
