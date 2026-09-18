#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""check_acp_error_message.py - boost::system::system_error 消息 ACP 转码纪律检查。

Windows 上 boost::system::system_error 的 what() / code().message() 中文段
来自 FormatMessageA（CP_ACP 字节），直接进入 UTF-8 文本通道（日志 fmt/
DAS_LOG、失败消息、诊断字符串）即乱码。消费点必须经统一转码通道：
  - Das::Utils::AcpTextToUtf8(...)   das/Utils/PathText.h，POSIX 透传

（历史上的 ToString(string_view)/U8String 等价转码通道已退役，全部
消费点迁移至 AcpTextToUtf8 后白名单收紧为单通道。）

检测面（纯文本词法级，无需 libclang/编译库）：
  catch (… boost::system::system_error …) 块内——
  A. 接收者.what() 成员调用（\w+\.what()）
  B. 接收者.code().message() 链式调用（错误消息与 what() 同源同编码）
裸调用（不在白名单转码函数的实参括号区间内）→ 违规。块内嵌套的其它
catch 子块剔除出检测范围（内层 std::exception 等的 what() 不归外层
boost 块管辖）。

两种模式：

  全仓模式（审计/整改清单）：
    python tools/check_acp_error_message.py [文件路径子串 ...]

    无参数扫全仓（das/include/Testing/tools 下的 git 跟踪 C++ 源，排除
    build/.planning 等非源码区；untracked scratch 不在整改面——一次性
    复现器不卡归零门，与 path 审计的正典口径一致）；路径子串只查命中
    文件（复核用）。

  暂存增量模式（pre-commit 第五道检查）：
    python tools/check_acp_error_message.py --staged

    只检查暂存区 C++ 文件"改动行"内的违规：git diff --cached -U0 的
    new 侧 hunk 行号与违规行号求交集（与 check_cpp_naming /
    check_path_text_encoding 同语义——解析对象为工作区文件内容，
    stage 后又改动的文件按工作区内容判定）。存量违规（未改动行）
    与未 staged 文件不阻塞。

    违规 → exit 1（逐条 文件:行号 + kind + 源码行）；检查器自身异常
    fail-open 放行 exit 0（违规判定本身是纯文本匹配，失败只可能是
    解析 bug，不应卡死提交）。

已知局限（v1 抓直接消费点）：
  - what()/code().message() 先存入局部变量再转码的两步形态不追踪
    （auto msg = e.what(); … AcpTextToUtf8(msg) 不会命中也不误报）；
  - 宏模板内的 catch 定义体按普通文本扫描（模板处违规会报，宏展开
    处不可见）。
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SCOPE_TOP = {"das", "tools", "include", "Testing"}
EXCLUDED_PARTS = {"build", ".planning", ".git", ".gitnexus", ".claude", ".gsd", "_deps", "vendor"}
CPP_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".hh", ".cuh"}

TOOL_NAME = "check_acp_error_message"

CATCH_TYPE_MARKER = "boost::system::system_error"
CATCH_RE = re.compile(r"\bcatch\b\s*\(")
WHAT_CALL_RE = re.compile(r"\b[A-Za-z_]\w*\s*\.\s*what\s*\(\s*\)")
CODE_MESSAGE_RE = re.compile(
    r"\b[A-Za-z_]\w*\s*\.\s*code\s*\(\s*\)\s*\.\s*message\s*\(\s*\)"
)
SANITIZER_RE = re.compile(r"\bAcpTextToUtf8\s*\(")

BRACKET_PAIRS = {"(": ")", "{": "}", "[": "]"}
RAW_STRING_OPEN_RE = re.compile(r'R"([^()\\ \t\n]*)\(')


# ---------------------------------------------------------------------------
# 词法剥除（等长替换：注释/字符串/字符字面量/raw string 内容 → 空格）
# ---------------------------------------------------------------------------

def strip_code(text):
    """把注释与字面量的内容替换为等长空格，换行保留。

    所有偏移与原文本一一对应（行列不变），后续 regex 命中可直接映射回
    原行号；剥除后字符串/注释里的括号花括号不再干扰配对。
    """
    out = list(text)
    n = len(text)

    def blank(begin, end):
        for k in range(begin, min(end, n)):
            if out[k] != "\n":
                out[k] = " "

    i = 0
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            j = i
            while j < n and text[j] != "\n":
                j += 1
            blank(i, j)
            i = j
        elif ch == "/" and nxt == "*":
            j = i + 2
            while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                j += 1
            end = min(j + 2, n)
            blank(i, end)
            i = end
        elif ch == '"':
            raw = RAW_STRING_OPEN_RE.match(text, i)
            if raw is not None:
                terminator = ")" + raw.group(1) + '"'
                end = text.find(terminator, raw.end())
                end = n if end == -1 else end + len(terminator)
                blank(i, end)
                i = end
            else:
                j = i + 1
                while j < n:
                    if text[j] == "\\":
                        j += 2
                        continue
                    if text[j] == '"':
                        break
                    j += 1
                end = min(j + 1, n)
                blank(i, end)
                i = end
        elif ch == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            end = min(j + 1, n)
            blank(i, end)
            i = end
        else:
            i += 1
    return "".join(out)


def match_bracket(stripped, open_pos):
    """从 open 位置做括号配对，返回闭合位置；不平衡返回 -1。"""
    open_ch = stripped[open_pos]
    close_ch = BRACKET_PAIRS[open_ch]
    depth = 0
    for i in range(open_pos, len(stripped)):
        ch = stripped[i]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_catch_ranges(stripped):
    """返回全部 catch 子句的 (声明头文本, 处理体起, 处理体终) 绝对区间。

    处理体 = 花括号块内部（不含花括号）；无花括号的单语句 catch 退化
    为 ) 之后到首个分号（含）。嵌套 catch（块内 try/catch）同样被
    finditer 命中，由调用方按区间包含关系剔除。
    """
    result = []
    for match in CATCH_RE.finditer(stripped):
        decl_open = match.end() - 1
        decl_close = match_bracket(stripped, decl_open)
        if decl_close == -1:
            continue
        head = stripped[decl_open + 1 : decl_close]
        k = decl_close + 1
        while k < len(stripped) and stripped[k] in " \t\r\n":
            k += 1
        if k < len(stripped) and stripped[k] == "{":
            body_close = match_bracket(stripped, k)
            if body_close == -1:
                continue
            result.append((head, k + 1, body_close))
        else:
            semi = stripped.find(";", decl_close)
            if semi == -1:
                continue
            result.append((head, decl_close + 1, semi))
    return result


def check_source(text):
    """对单个 C++ 源文本跑检查，返回违规列表（行号 1 基）。

    [{'line': int, 'kind': 'what'|'code-message', 'snippet': str}]
    """
    stripped = strip_code(text)
    all_catches = find_catch_ranges(stripped)
    lines = text.splitlines()
    findings = []
    for head, body_start, body_end in all_catches:
        normalized_head = re.sub(r"\s+", " ", head)
        if CATCH_TYPE_MARKER not in normalized_head:
            continue
        # 白名单转码函数的实参括号区间（可跨行长匹配）
        exempt = []
        for sanitizer in SANITIZER_RE.finditer(stripped, body_start, body_end):
            arg_open = sanitizer.end() - 1
            arg_close = match_bracket(stripped, arg_open)
            if arg_close != -1:
                exempt.append((sanitizer.start(), arg_close))
        # 嵌套 catch 子块剔除：内层 catch 的 what() 不归外层 boost 块管
        nested = [
            (inner_start, inner_end)
            for _inner_head, inner_start, inner_end in all_catches
            if body_start < inner_start and inner_end <= body_end
        ]
        for pattern, kind in (
            (WHAT_CALL_RE, "what"),
            (CODE_MESSAGE_RE, "code-message"),
        ):
            for hit in pattern.finditer(stripped, body_start, body_end):
                if any(a <= hit.start() <= b for a, b in nested):
                    continue
                if any(a <= hit.start() <= b for a, b in exempt):
                    continue
                line = stripped.count("\n", 0, hit.start()) + 1
                snippet = lines[line - 1].strip() if line <= len(lines) else ""
                findings.append(
                    {"line": line, "kind": kind, "snippet": snippet}
                )
    return findings


# ---------------------------------------------------------------------------
# 文件收集（全仓模式）
# ---------------------------------------------------------------------------

def tracked_files():
    """git 跟踪文件集合（正斜杠相对路径）；git 不可用时返回 None（放行全部）。"""
    try:
        out = subprocess.run(
            ["git", "ls-files"],
            cwd=str(ROOT), capture_output=True, text=True,
            encoding="utf-8", errors="replace", check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return {line.strip().replace("\\", "/") for line in out.splitlines() if line.strip()}


def repo_files(file_filters):
    tracked = tracked_files()
    files = []
    for top in sorted(SCOPE_TOP):
        top_dir = os.path.join(str(ROOT), top)
        if not os.path.isdir(top_dir):
            continue
        for current, dirs, names in os.walk(top_dir):
            dirs[:] = [d for d in dirs if d not in EXCLUDED_PARTS]
            for name in names:
                if os.path.splitext(name)[1].lower() not in CPP_EXTS:
                    continue
                relative = os.path.relpath(
                    os.path.join(current, name), str(ROOT)
                ).replace("\\", "/")
                if tracked is not None and relative not in tracked:
                    continue
                if file_filters and not any(flt in relative for flt in file_filters):
                    continue
                files.append(relative)
    return files


# ---------------------------------------------------------------------------
# 暂存增量模式（与 check_path_text_encoding 的行级过滤同语义）
# ---------------------------------------------------------------------------

def staged_cpp_files():
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=True,
    ).stdout
    files = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        if os.path.splitext(line)[1].lower() in CPP_EXTS:
            files.append(line.replace("\\", "/"))
    return files


def changed_line_ranges(rel_path):
    out = subprocess.run(
        ["git", "diff", "--cached", "-U0", "--", rel_path],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=True,
    ).stdout
    ranges = []
    for line in out.splitlines():
        if not line.startswith("@@"):
            continue
        try:
            new_part = line.split("+")[1].split("@@")[0].strip().split(",")
        except (IndexError, ValueError):
            continue
        start = int(new_part[0])
        count = int(new_part[1]) if len(new_part) > 1 else 1
        if count <= 0:
            continue
        ranges.append((start, start + count - 1))
    return ranges


def run_staged_mode():
    files = staged_cpp_files()
    if not files:
        print(f"[{TOOL_NAME}] no staged C++ changes")
        return 0
    violations = []
    for relative_path in files:
        absolute = ROOT / relative_path
        if not absolute.is_file():
            continue
        ranges = changed_line_ranges(relative_path)
        if not ranges:
            continue
        changed_lines = set()
        for start, end in ranges:
            changed_lines.update(range(start, end + 1))
        for finding in check_source(absolute.read_text(encoding="utf-8", errors="replace")):
            if finding["line"] in changed_lines:
                violations.append((relative_path, finding))
    if violations:
        print(
            f"[{TOOL_NAME}] boost::system_error message ACP transcoding "
            "violations (changed lines only):"
        )
        for relative_path, finding in sorted(
            violations, key=lambda item: (item[0], item[1]["line"])
        ):
            print(
                f"  {relative_path}:{finding['line']} [{finding['kind']}] "
                f"{finding['snippet']}"
            )
        print(
            f"[{TOOL_NAME}] catch (boost::system::system_error) 内的 "
            "what()/code().message() 必须经 AcpTextToUtf8 转码后再消费"
        )
        return 1
    print(f"[{TOOL_NAME}] ACP error message check passed")
    return 0


def run_full_repo_mode(file_filters):
    violations = []
    for relative_path in repo_files(file_filters):
        absolute = ROOT / relative_path
        try:
            text = absolute.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"[{TOOL_NAME}] warning: cannot read {relative_path}: {exc}")
            continue
        for finding in check_source(text):
            violations.append((relative_path, finding))
    if violations:
        print(
            f"[{TOOL_NAME}] boost::system_error message ACP transcoding violations:"
        )
        for relative_path, finding in sorted(
            violations, key=lambda item: (item[0], item[1]["line"])
        ):
            print(
                f"  {relative_path}:{finding['line']} [{finding['kind']}] "
                f"{finding['snippet']}"
            )
        by_kind = {}
        for _path, finding in violations:
            by_kind[finding["kind"]] = by_kind.get(finding["kind"], 0) + 1
        summary = ", ".join(f"{kind}={count}" for kind, count in sorted(by_kind.items()))
        print(f"[{TOOL_NAME}] total {len(violations)} ({summary})")
        return 1
    print(f"[{TOOL_NAME}] ACP error message check passed")
    return 0


def build_argument_parser():
    parser = argparse.ArgumentParser(
        description=(
            "catch (boost::system::system_error) 块内 what()/code().message() "
            "的 ACP 转码纪律检查（全仓 + 暂存增量双模式）"
        )
    )
    parser.add_argument(
        "--staged",
        action="store_true",
        help="检查暂存区改动行（pre-commit 增量模式）",
    )
    parser.add_argument(
        "file_filters",
        nargs="*",
        help="文件路径子串过滤（全仓模式复核用）",
    )
    return parser


def console_main(argv=None):
    args = build_argument_parser().parse_args(argv)
    if args.staged:
        # 非强制检查 fail-open：检查器自身 bug 不得卡死提交
        try:
            return run_staged_mode()
        except Exception as exc:  # noqa: BLE001
            first_line = (
                str(exc).splitlines()[0] if str(exc) else exc.__class__.__name__
            )
            print(
                f"[{TOOL_NAME}] warning: internal error, fail-open ({first_line})",
                file=sys.stderr,
            )
            return 0
    return run_full_repo_mode(args.file_filters)


if __name__ == "__main__":
    sys.exit(console_main())
