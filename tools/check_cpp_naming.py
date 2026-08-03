#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_cpp_naming.py - 基于 clang-tidy readability-identifier-naming 的 C++ 命名风格检查。

仅检查暂存区改动文件的"改动行"内引入的命名违规（--line-filter），
存量违规不阻塞提交，可后续批量清理。

用法:
    python tools/check_cpp_naming.py --staged         # 检查暂存区改动行的命名
    python tools/check_cpp_naming.py --install-hook   # 安装 pre-commit hook(组合 IPC 头校验)
    python tools/check_cpp_naming.py --help           # 帮助

配置: 命名规则定义在项目根 .clang-tidy 的 CheckOptions(Check: readability-identifier-naming)。
编译数据库: 自动探测 build/*/compile_commands.json (优先 mingw-debug / ember)。
"""
import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP_EXTS = {'.h', '.hpp', '.cpp', '.cxx', '.cc', '.hh', '.cuh'}
PREFERRED_BUILDS = ('mingw-debug', 'mingw', 'ember', 'debug')


def find_compile_db():
    """在 build/ 下探测 compile_commands.json，优先 mingw-debug。"""
    build_dir = ROOT / 'build'
    if not build_dir.is_dir():
        return None
    candidates = []
    for name in PREFERRED_BUILDS:
        p = build_dir / name / 'compile_commands.json'
        if p.is_file():
            candidates.append(p)
    if not candidates:
        candidates = sorted(build_dir.glob('*/compile_commands.json'))
    return str(candidates[0]) if candidates else None


def find_clang_tidy():
    return shutil_which('clang-tidy')


def shutil_which(name):
    try:
        import shutil
        return shutil.which(name)
    except Exception:
        return None


def staged_cpp_files():
    """返回暂存区改动(新增/复制/修改)的 C++ 文件，相对仓库根路径。"""
    out = subprocess.run(
        ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
        cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', check=True,
    ).stdout
    files = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if Path(line).suffix.lower() in CPP_EXTS:
            files.append(line)
    return files


def changed_line_ranges(rel_path):
    """解析 git diff --cached -U0 的 hunk，返回暂存版本(new 侧)的改动行范围 [(start, end)]。"""
    out = subprocess.run(
        ['git', 'diff', '--cached', '-U0', '--', rel_path],
        cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace', check=True,
    ).stdout
    ranges = []
    for line in out.splitlines():
        if not line.startswith('@@'):
            continue
        # @@ -old,oldc +new,newc @@  (context=0 时 newc 为 0 表示纯删除)
        try:
            new_part = line.split('+')[1].split('@@')[0].strip().split(',')
        except (IndexError, ValueError):
            continue
        start = int(new_part[0])
        count = int(new_part[1]) if len(new_part) > 1 else 1
        if count <= 0:
            continue
        ranges.append((start, start + count - 1))
    return ranges


def build_line_filter(rel_path, ranges):
    # clang-tidy 的 line-filter name 匹配的是文件 basename（实测完整路径会导致全量抑制）
    name = Path(rel_path).name
    return json.dumps([{'name': name, 'lines': [[s, e] for s, e in ranges]}])


def run_clang_tidy(rel_path, ranges, compile_db):
    abs_path = str((ROOT / rel_path).resolve())
    cmd = [
        find_clang_tidy(),
        '-p', compile_db,
        '--checks=-*,readability-identifier-naming',
        '--line-filter=' + build_line_filter(rel_path, ranges),
        abs_path,
    ]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')
    violations = []
    for line in (proc.stdout + proc.stderr).splitlines():
        if 'warning: invalid case' in line and 'readability-identifier-naming' in line:
            violations.append(line.strip())
    return violations


def main():
    parser = argparse.ArgumentParser(description='C++ 命名风格检查 (clang-tidy readability-identifier-naming)')
    parser.add_argument('--staged', action='store_true', help='检查暂存区改动文件的改动行')
    parser.add_argument('--install-hook', action='store_true', help='安装 pre-commit hook')
    args = parser.parse_args()

    if args.install_hook:
        hook_path = ROOT / '.git' / 'hooks' / 'pre-commit'
        hook_path.parent.mkdir(parents=True, exist_ok=True)
        hook_content = '''#!/bin/sh
# Pre-commit hook: IPC header validation + C++ naming check
# Installed by check_cpp_naming.py (组合 check_ipc_header_usage.py)

python tools/check_ipc_header_usage.py --staged || exit $?
python tools/check_cpp_naming.py --staged || exit $?
'''
        hook_path.write_text(hook_content, encoding='utf-8')
        print(f"Pre-commit hook installed at: {hook_path}")
        print("Run 'chmod +x .git/hooks/pre-commit' to make it executable on Unix")
        return 0

    if not args.staged:
        parser.error('需要 --staged 或 --install-hook')

    clang_tidy = find_clang_tidy()
    if not clang_tidy:
        print('[check_cpp_naming] 跳过: 未找到 clang-tidy (需要 MSYS2/MinGW clang64 工具链)')
        return 0
    compile_db = find_compile_db()
    if not compile_db:
        print('[check_cpp_naming] 跳过: 未找到 build/*/compile_commands.json，请先配置构建')
        return 0

    files = staged_cpp_files()
    if not files:
        print('[check_cpp_naming] 无暂存 C++ 改动')
        return 0

    def check_one(rel, db):
        ranges = changed_line_ranges(rel)
        if not ranges:
            return rel, []
        return rel, run_clang_tidy(rel, ranges, db)

    all_violations = 0
    # 并行跑 clang-tidy（大提交时显著缩短 hook 耗时，worker 数为 CPU 核心数的一半）
    workers = max(1, (os.cpu_count() or 4) // 2)
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = {ex.submit(check_one, rel, compile_db): rel for rel in files}
        for fut in as_completed(futures):
            rel, violations = fut.result()
            for v in violations:
                print(v)
            all_violations += len(violations)

    if all_violations:
        print(f'[check_cpp_naming] 发现 {all_violations} 处命名违规（仅改动行）: '
              f'请按 .clang-tidy 规则修改后重新 add')
        return 1
    print('[check_cpp_naming] 命名检查通过')
    return 0


if __name__ == '__main__':
    sys.exit(main())
