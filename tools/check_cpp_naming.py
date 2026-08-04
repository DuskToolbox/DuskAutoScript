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


# pre-commit 钩子启用的 clang-tidy checks：命名规范 + 单行语句强制花括号 + 性能 + Modern C++ + 静态分析
CHECKS = ('-*,readability-identifier-naming,readability-braces-around-statements,'
          'performance-*,modernize-*,-modernize-use-trailing-return-type,'
          '-modernize-avoid-c-arrays,-modernize-use-transparent-functors,clang-analyzer-*')
# 解析时按 check 名前缀分类违规：命名检查(naming)与其他 clang-tidy 检查(other)分开收集，
# 避免其他检查的违规混进 naming 日志。run_clang_tidy 返回 (naming, other) 两组。
NAMING_MARKER = 'readability-identifier-naming'
OTHER_MARKERS = ('readability-braces-around-statements',
                 'performance-', 'modernize-', 'clang-analyzer-')


def run_clang_tidy(rel_path, ranges, compile_db):
    abs_path = str((ROOT / rel_path).resolve())
    cmd = [
        find_clang_tidy(),
        '-p', compile_db,
        '--checks=' + CHECKS,
        '--line-filter=' + build_line_filter(rel_path, ranges),
        abs_path,
    ]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')
    naming = []
    other = []
    for line in (proc.stdout + proc.stderr).splitlines():
        s = line.strip()
        if NAMING_MARKER in s:
            naming.append(s)
        elif any(m in s for m in OTHER_MARKERS):
            other.append(s)
    return naming, other


def main():
    parser = argparse.ArgumentParser(description='C++ naming style check (clang-tidy readability-identifier-naming)')
    parser.add_argument('--staged', action='store_true', help='check changed lines in staged files')
    parser.add_argument('--install-hook', action='store_true', help='install the pre-commit hook')
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
        parser.error('--staged or --install-hook is required')

    clang_tidy = find_clang_tidy()
    if not clang_tidy:
        print('[check_cpp_naming] skipped: clang-tidy not found (need MSYS2/MinGW clang64 toolchain)')
        return 0
    compile_db = find_compile_db()
    if not compile_db:
        print('[check_cpp_naming] skipped: build/*/compile_commands.json not found, configure a build first')
        return 0

    files = staged_cpp_files()
    if not files:
        print('[check_cpp_naming] no staged C++ changes')
        return 0

    def check_one(rel, db):
        ranges = changed_line_ranges(rel)
        if not ranges:
            return rel, [], []
        naming, other = run_clang_tidy(rel, ranges, db)
        return rel, naming, other

    naming_lines = []
    other_lines = []
    # 并行跑 clang-tidy（大提交时显著缩短 hook 耗时，worker 数为 CPU 核心数的一半）
    workers = max(1, (os.cpu_count() or 4) // 2)
    total = len(files)
    done = 0
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = {ex.submit(check_one, rel, compile_db): rel for rel in files}
        for fut in as_completed(futures):
            rel, naming, other = fut.result()
            done += 1
            if total > 1:
                print(f'[check_cpp_naming] progress {done}/{total} done ({rel})', file=sys.stderr)
            naming_lines.extend(naming)
            other_lines.extend(other)

    # 分段打印：naming 与其他 clang-tidy 检查不混在一起
    if naming_lines:
        print(f'[check_cpp_naming] naming violations ({NAMING_MARKER}, changed lines only):')
        for v in naming_lines:
            print(v)
    if other_lines:
        print(f'[check_cpp_naming] other clang-tidy violations '
              f'(braces/performance/modernize/analyzer, changed lines only):')
        for v in other_lines:
            print(v)

    total_naming = len(naming_lines)
    total_other = len(other_lines)
    if total_naming or total_other:
        print(f'[check_cpp_naming] {total_naming} naming + {total_other} other clang-tidy '
              f'violation(s): fix per .clang-tidy rules, then re-add')
        return 1
    print('[check_cpp_naming] naming check passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
