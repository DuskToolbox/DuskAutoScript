#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_path_text_encoding.py - std/boost filesystem::path 窄字符串拼接审计（双模式）。

Windows 下 path::value_type = wchar_t，窄串（const char* 字面量、窄
std::string、运行期 char*）经 ACP 隐式转换进入 path：ASCII 偶然安全；
非 ASCII 字面量/动态内容不可逆乱码。

检测面（AST 级，clang.cindex）：
  A. path 构造调用（显式/拷贝初始化/函数实参隐式转换）收窄串实参
  B. path 成员 operator/ operator/= operator+= operator= append concat
     assign 收窄串实参
  C. std/boost::filesystem 命名空间下自由 operator/ 的窄串实参
     （A/B/C 三层互补：隐式转换若在 AST 中不可见则由父层兜底）
连带：path::string() 调用点计数（Windows ACP 路径→文本通道）。
合规面：宽字面量（L""/TEXT 宏展开）、u8""（显式 UTF-8，无 ACP）、
  to_string 结果（纯数字）、wstring()/u8string() 桥、ToStdPath 引用。

两种模式：

  全仓模式（审计 runner / 手动全量或过滤审计）：
    python tools/check_path_text_encoding.py <compile_commands.json> <findings.json> [file-filter ...]
    python tools/check_path_text_encoding.py <cc.json> <out.json> \
        --filter-prefix das/Plugins/DasMaaPi \
        --merge-baseline <全量基线.json> [--jobs N]

    file-filter 为路径子串，仅解析命中子串的 TU（冒烟/复核用）。
    --filter-prefix 为逗号分隔的仓内相对路径前缀，只解析命中前缀的 TU；
    与 --merge-baseline 组合时按文件整替换合并：被过滤跑实际走到的文件
    （TU 自身 + 其包含链上的仓内头）用新结果整体覆盖，其余文件沿用基线
    —— 输出 JSON 顶层带 merged_from / filtered_tus 元数据。
    --jobs 为并行进程数（ProcessPoolExecutor，每 worker 独立 clang Index，
    进程隔离无 libclang 线程安全问题）；默认取 CPU 逻辑核数，--jobs 1
    退化为串行路径。

  暂存增量模式（pre-commit 第三道检查，非强制）：
    python tools/check_path_text_encoding.py --staged

    只检查暂存区 C++ 文件"改动行"内引入的违规：行级过滤独立实现（git
    diff --cached -U0 的 new 侧 hunk 行号与 finding 行号求交集；不走
    clang-tidy --line-filter——其 name 匹配的是 basename，完整路径会导致
    全量抑制，本仓已踩过）。存量违规（未改动行）与未 staged 文件不阻塞。
    libclang 解析的是工作区文件：stage 后又改动文件，判定按工作区内容
    （与 check_cpp_naming 同语义）。

    compile db 两层查找（严格两层，无 build/*/ glob 兜底）：
      1. env DAS_COMPILE_COMMANDS_JSON 显式 pin（最高优先；指向不存在
         路径时打印 warning 后继续下层）；
      2. 解析 CMakePresets.json + CMakeUserPresets.json 的全部
         configurePresets，沿 inherits 链解析生效 binaryDir（自身无则取
         父链，如 ember 继承 mingw-debug），${sourceDir} 展开为仓库根，
         探测各 <binaryDir>/compile_commands.json；多树命中取 mtime 最新，
         所选数据库 json 解析失败 fallback 下一个。
    两层全落空 → warning 放行 exit 0。无论哪层命中都打印实际使用的
    数据库路径（多树场景可观测查的是哪棵树）。

    违规处理：libclang 可用且改动行有违规 → exit 1（逐条 文件:行号 +
    R1/R2/R3 规则档）；环境缺失（libclang 不可得 / db 找不到 / 单文件
    TU 解析不动）→ 仅 warning 放行 exit 0，每类缺失都有明确输出，不静默。

环境要求（环境由使用者自保证，工具零兜底、零硬编码路径）：
  - 当前解释器需可 import clang；hook 场景配置 env DAS_PYTHON 指向
    DAS 工具链 python（当前需带 clang binding，如 MSYS2 clang64 的
    python.exe），pre-commit 第三道以它替换默认解释器；
  - PATH 上的 clang 用于查询 resource dir（clang -print-resource-dir），
    亦可用 env CLANG_RESOURCE_DIR 显式指定；-isystem 系统头搜索组由
    clang driver 自报（clang++ -E -v 的尖括号搜索列表，含 resource
    dir 自带 include，照单全收、保持 driver 顺序）。
  任一缺失则本检查降级 warning 放行（exit 0），每类缺失都有明确输出；
  依赖这些路径的解析按 degraded 语义跳过并提示。

已知坑（沿用 .planning/audit/dasptr/dasptr_audit.py 的经验）：
  - 解析 flags 必须从 compile_commands 取；libclang 不跑 driver 的
    mingw sysroot 探测，需补 -resource-dir 与 -isystem。
  - 实参外层常包一层或多层 UNEXPOSED_EXPR，须解包后再判型。
  - 同一 header 被多个 TU 展开会产生重复游标，须按 (file,line,col)
    全局去重。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

try:
    from clang import cindex
except ImportError:
    # 无 clang 模块的默认解释器也要能安全 import 本模块：CLI 入口在此态
    # 做解释器回退；.planning 薄壳与 ProcessPoolExecutor worker 依赖此性质。
    cindex = None

K = cindex.CursorKind if cindex is not None else None

ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = str(ROOT).replace("\\", "/") + "/"

SCOPE_TOP = {"das", "tools", "include", "Testing"}
EXCLUDED_PARTS = {"build", ".planning", ".git", ".gitnexus", ".claude", ".gsd", "_deps", "vendor"}

PATH_OP_METHODS = {
    "operator/",
    "operator/=",
    "operator+=",
    "operator=",
    "append",
    "concat",
    "assign",
}
TEXT_METHODS = {"string", "generic_string"}
BRIDGE_METHODS = {"wstring", "generic_wstring", "u8string", "generic_u8string"}

# 追加补扫（不在 compile_commands 中的仓内文件，best-effort 解析）。
# Testing/boost-env-repro.cpp（untracked 一次性复现器，毒环境变量 boost
# utf8 崩溃取证用，结论已归档）已移出：审计正典口径 = 构建系统 compile
# db；untracked scratch 不在整改面内，且该文件按纪律不可编辑，留在扫描
# 集会让归零门被永久无关条目卡死。需要恢复扫描时加回即可。
EXTRA_FILES = [
    str(ROOT / "tools" / "das_idl" / "node_host" / "das_napi_loop_detach.h").replace(
        "\\", "/"
    ),
]

# helper 实现自身的窄边界白名单（findings 侧；compliance 台账照记）：
# 这些点位的窄构造是转换引擎本体而非待整改的消费点——
#   PathText.h:88   PathFromEnvironment posix 分支 getenv 本码直构
#                  （「消灭转换而非修转换」，posix 原生窄即本码）；
#   PathText.h:101 PathFromCommandLineArg Windows 分支窄构造（CRT argv
#                  是 ACP 源，窄构造与源配对正确而非待整改的隐式介入）；
#   StringUtils.h:213 ToString(string_view) 是 native(narrow/ACP)→UTF-8
#                  转码器，path 窄构造承担 ACP 解码半步，语义即目的。
# 行锚定：同文件未来新增窄点位不享受豁免，仍按常规 findings 报出。
HELPER_FINDING_WHITELIST = {
    ("das/Utils/include/das/Utils/PathText.h", 88),
    ("das/Utils/include/das/Utils/PathText.h", 101),
    ("das/Utils/include/das/Utils/StringUtils.h", 213),
}


def repo_rel(file_path):
    """返回仓库内相对路径（正斜杠）；仓外/排除区返回 None。"""
    if not file_path:
        return None
    normalized = file_path.replace("\\", "/")
    if not normalized.startswith(REPO_ROOT):
        return None
    rel = normalized[len(REPO_ROOT):]
    parts = rel.split("/")
    if not parts or parts[0] not in SCOPE_TOP:
        return None
    if any(part in EXCLUDED_PARTS for part in parts):
        return None
    # 规范化 `./`、`../` 段：不同 TU 对同一 header 可能以别名路径出现，
    # 不归一会绕过 (file,line,col) 去重产生双计数。normpath 的分隔符输出
    # 随解释器构建形态漂移（MSVC 构建会回退为反斜杠），显式归一到正斜杠，
    # 保证白名单/基线合并/git 输出路径（恒正斜杠）三方可比对。
    return os.path.normpath(rel).replace("\\", "/")


def module_of(rel):
    parts = rel.split("/")
    if parts[0] == "das" and len(parts) >= 3:
        if parts[1] in ("Core", "Plugins"):
            return f"das/{parts[1]}/{parts[2]}"
        return f"das/{parts[1]}"
    return parts[0]


# ---------------------------------------------------------------------------
# path 类判定
# ---------------------------------------------------------------------------

def flavor_from_record(record):
    """class/struct 游标是 std/boost filesystem 的 path 则返回 flavor。"""
    if record is None:
        return None
    if record.kind not in (K.CLASS_DECL, K.STRUCT_DECL):
        return None
    if record.spelling != "path":
        return None
    names = []
    node = record.semantic_parent
    while node is not None and node.kind == K.NAMESPACE:
        names.append(node.spelling)
        node = node.semantic_parent
    chain = "::".join(reversed(names))
    # libc++ 内联命名空间 std::__fs::filesystem；libstdc++ boost 均为
    # <std|boost>::filesystem。
    if chain.startswith("std") and chain.endswith("filesystem"):
        return "std"
    if chain.startswith("boost") and chain.endswith("filesystem"):
        return "boost"
    return None


def flavor_from_namespace(namespace):
    if namespace is None or namespace.kind != K.NAMESPACE:
        return None
    names = []
    node = namespace
    while node is not None and node.kind == K.NAMESPACE:
        names.append(node.spelling)
        node = node.semantic_parent
    chain = "::".join(reversed(names))
    if chain.startswith("std") and chain.endswith("filesystem"):
        return "std"
    if chain.startswith("boost") and chain.endswith("filesystem"):
        return "boost"
    return None


def path_call_info(call_cursor):
    """CALL_EXPR 是 path 相关调用则返回 (flavor, op_name)；否则 None。

    op_name: "ctor" / 成员运算符或方法名 / "operator/"（自由函数）。
    """
    ref = call_cursor.referenced
    if ref is None:
        return None
    if ref.kind == K.CONSTRUCTOR:
        flavor = flavor_from_record(ref.semantic_parent)
        return (flavor, "ctor") if flavor else None
    if ref.kind == K.CXX_METHOD:
        flavor = flavor_from_record(ref.semantic_parent)
        if not flavor:
            return None
        name = ref.spelling
        if name in PATH_OP_METHODS or name in TEXT_METHODS or name in BRIDGE_METHODS:
            return (flavor, name)
        return None
    if ref.kind == K.FUNCTION_DECL and ref.spelling in ("operator/", "operator/="):
        flavor = flavor_from_namespace(ref.semantic_parent)
        return (flavor, ref.spelling) if flavor else None
    return None


# ---------------------------------------------------------------------------
# 实参宽度分类
# ---------------------------------------------------------------------------

def norm_type(type_obj):
    try:
        spelling = type_obj.spelling or ""
    except Exception:  # noqa: BLE001
        return ""
    return spelling.replace(" ", "")


def _strip_qualifiers(sp):
    core = sp
    while core.startswith("const"):
        core = core[5:]
    return core.rstrip("&")


def narrow_string_type(sp):
    core = _strip_qualifiers(sp)
    if re.match(r"^char(\*|\[)", core):
        return True
    if core in ("std::string", "string"):
        return True
    if re.search(r"basic_string<char[>,]", core):
        return True
    if core in ("std::string_view", "string_view"):
        return True
    if re.search(r"basic_string_view<char[>,]", core):
        return True
    return False


def wide_string_type(sp):
    core = _strip_qualifiers(sp)
    if re.match(r"^wchar_t(\*|\[)", core):
        return True
    if core in ("std::wstring", "wstring"):
        return True
    return "basic_string<wchar_t" in core


def u8_string_type(sp):
    return "char8_t" in _strip_qualifiers(sp)


def pathish_type(sp):
    """规范化类型拼写是否为 path 本体/引用（operator/ 操作数等）。"""
    core = sp
    while core.startswith("const"):
        core = core[5:]
    core = core.rstrip("&*")
    return core == "path" or core.endswith("::path")


def unwrap_expr(cursor):
    while cursor.kind == K.UNEXPOSED_EXPR:
        child = None
        for child in cursor.get_children():
            break
        if child is None:
            break
        cursor = child
    return cursor


def classify_arg(arg_cursor):
    """实参宽度/形态分类。

    返回 (arg_class, detail)：
      arg_class ∈ literal_narrow / literal_wide / literal_u8 /
                  literal_u16u32 / dyn_narrow / wide_type / u8_type /
                  safe_to_string / unknown_call
    """
    node = unwrap_expr(arg_cursor)
    kind = node.kind

    if kind == K.STRING_LITERAL:
        sp = norm_type(node.type)
        if sp.startswith("constchar[") or sp.startswith("char["):
            return ("literal_narrow", "")
        if "wchar_t[" in sp:
            return ("literal_wide", "")
        if "char8_t[" in sp:
            return ("literal_u8", "")
        if "char16_t[" in sp or "char32_t[" in sp:
            return ("literal_u16u32", "")
        return (None, None)

    if kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
        ref = node.referenced
        if ref is not None and ref.kind in (
            K.VAR_DECL,
            K.PARM_DECL,
            K.FIELD_DECL,
        ):
            sp = norm_type(ref.type)
            if narrow_string_type(sp):
                return ("dyn_narrow", f"var:{ref.spelling}({sp})")
            if wide_string_type(sp):
                return ("wide_type", "")
            if u8_string_type(sp):
                return ("u8_type", "")
        sp = norm_type(node.type)
        if narrow_string_type(sp):
            return ("dyn_narrow", "expr")
        return (None, None)

    if kind == K.CALL_EXPR:
        ref = node.referenced
        name = ref.spelling if ref is not None else ""
        if name == "to_string":
            return ("safe_to_string", "")
        if name in BRIDGE_METHODS:
            return (
                "wide_type" if name in ("wstring", "generic_wstring") else "u8_type",
                f"call:{name}()",
            )
        sp = norm_type(node.type)
        if name in ("c_str", "data") and narrow_string_type(sp):
            return ("dyn_narrow", f"call:{name}()")
        if name in ("c_str", "data") and u8_string_type(sp):
            return ("u8_type", f"call:{name}()")
        if narrow_string_type(sp):
            return ("dyn_narrow", f"call-ret:{name or '?'}()->{sp}")
        if wide_string_type(sp):
            return ("wide_type", "")
        if u8_string_type(sp):
            return ("u8_type", "")
        if name in TEXT_METHODS:
            return ("dyn_narrow", f"call:{name}()->std::string")
        # path 型操作数（operator/ 左右参、隐式构造等）：非发现。
        if pathish_type(sp):
            return ("path_expr", "")
        # 返回类型未解析（模板退化等）才降级 R3；已解析非串型跳过。
        if not sp:
            return ("unknown_call", f"call:{name or '?'}()-><unresolved>")
        return (None, None)

    sp = norm_type(node.type)
    if narrow_string_type(sp):
        return ("dyn_narrow", f"expr:{sp}")
    if wide_string_type(sp):
        return ("wide_type", "")
    if u8_string_type(sp):
        return ("u8_type", "")
    return (None, None)


# ---------------------------------------------------------------------------
# 字面量字节还原（非 ASCII 判定用）
# ---------------------------------------------------------------------------

SIMPLE_ESCAPES = {
    "n": b"\n",
    "t": b"\t",
    "r": b"\r",
    "0": b"\x00",
    "a": b"\a",
    "b": b"\b",
    "f": b"\f",
    "v": b"\v",
    "\\": b"\\",
    "'": b"'",
    '"': b'"',
    "?": b"?",
}


def literal_token_text(tu, literal_cursor):
    try:
        tokens = list(literal_cursor.get_tokens())
        return tokens[0].spelling if tokens else ""
    except Exception:  # noqa: BLE001
        return ""


def all_ascii_components(cursor, tu, depth=0):
    """拼接表达式的全部窄串分量是否结构上 ASCII（字面量/to_string）。

    用于把 `"name_" + std::to_string(n)` 这类结构上不可能含非 ASCII 的
    拼接从动态窄内容（R1）改判为合规备注；出现任何变量/未知调用分量
    即保守返回 False。
    """
    if depth > 8:
        return False
    node = unwrap_expr(cursor)
    kind = node.kind
    if kind == K.STRING_LITERAL:
        sp = norm_type(node.type)
        if sp.startswith("constchar[") or sp.startswith("char["):
            text = literal_token_text(tu, node)
            if not text:
                return False
            return not any(byte > 0x7F for byte in literal_bytes(text))
        return True  # 宽/u8 字面量：非窄分量
    if kind == K.INTEGER_LITERAL or kind == K.FLOATING_LITERAL:
        return True
    if kind in (K.PAREN_EXPR, K.UNARY_OPERATOR):
        children = list(node.get_children())
        return all_ascii_components(children[0], tu, depth + 1) if children else False
    if kind == K.BINARY_OPERATOR:
        return all(
            all_ascii_components(child, tu, depth + 1)
            for child in node.get_children()
        )
    if kind == K.CALL_EXPR:
        ref = node.referenced
        name = ref.spelling if ref is not None else ""
        if name == "to_string":
            return True
        if name == "operator+":
            return all(
                all_ascii_components(child, tu, depth + 1)
                for child in node.get_children()
                if not (child.kind == K.UNEXPOSED_EXPR and "*)(" in norm_type(child.type))
            )
        return False
    return False


def literal_bytes(token_text):
    """按 C/C++ 转义规则把字面量 token 文本还原成字节序列（尽力而为）。"""
    text = token_text.strip()
    while text and text[0] in "LuU":
        text = text[1:]
    if text.startswith("u8"):
        text = text[2:]
    if len(text) >= 2 and text[0] == '"' and text[-1] == '"':
        text = text[1:-1]
    raw = text.encode("utf-8", errors="surrogatepass")
    out = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        b = raw[i]
        if b != 0x5C:  # backslash
            out.append(b)
            i += 1
            continue
        if i + 1 >= n:
            out.append(b)
            break
        nxt = raw[i + 1 : i + 2]
        if nxt == b"x":
            j = i + 2
            hexs = bytearray()
            while j < n and len(hexs) < 2 and chr(raw[j]) in "0123456789abcdefABCDEF":
                hexs.append(raw[j])
                j += 1
            if hexs:
                out.append(int(hexs.decode("ascii"), 16))
                i = j
            else:
                out.append(b)
                i += 1
        elif nxt == b"u" or nxt == b"U":
            width = 4 if nxt == b"u" else 8
            j = i + 2
            hexs = bytearray()
            while j < n and len(hexs) < width and chr(raw[j]) in "0123456789abcdefABCDEF":
                hexs.append(raw[j])
                j += 1
            if hexs:
                try:
                    out.extend(chr(int(hexs.decode("ascii"), 16)).encode("utf-8"))
                    i = j
                except (ValueError, UnicodeEncodeError):
                    out.append(b)
                    i += 1
            else:
                out.append(b)
                i += 1
        elif nxt.isdigit():  # 八进制
            j = i + 1
            octs = bytearray()
            while j < n and len(octs) < 3 and chr(raw[j]) in "01234567":
                octs.append(raw[j])
                j += 1
            if octs:
                out.append(int(octs.decode("ascii"), 8) & 0xFF)
                i = j
            else:
                out.append(b)
                i += 1
        else:
            out.extend(SIMPLE_ESCAPES.get(nxt.decode("latin-1"), nxt))
            i += 2
    return bytes(out)


# ---------------------------------------------------------------------------
# 审计器
# ---------------------------------------------------------------------------

class Auditor:
    def __init__(self):
        self.findings = {}
        self.compliance = defaultdict(list)  # kind -> [{file,line}]
        self.parse_errors = []
        self.parsed_ok = 0
        self.tu_degraded = False

        self._source_lines = {}

    def source_line(self, file_path, line):
        key = file_path
        if key not in self._source_lines:
            try:
                with open(file_path, encoding="utf-8", errors="replace") as handle:
                    self._source_lines[key] = handle.readlines()
            except OSError:
                self._source_lines[key] = []
        lines = self._source_lines[key]
        if 1 <= line <= len(lines):
            return lines[line - 1].rstrip("\n").strip()
        return ""

    def record_compliance(self, kind, file_path, line):
        rel = repo_rel(file_path)
        if rel is None:
            return
        entry = (rel, line)
        existing = self.compliance[kind]
        if entry not in existing:
            existing.append(entry)

    def emit(self, file_path, line, col, module, flavor, op, context, arg_class, detail, literal_text):
        rel = repo_rel(file_path)
        if rel is None:
            return
        if (rel, line) in HELPER_FINDING_WHITELIST:
            return
        key = (rel, line, col)
        if key in self.findings:
            return
        non_ascii = False
        dynamic = arg_class == "dyn_narrow"
        if arg_class == "literal_narrow" and literal_text:
            non_ascii = any(byte > 0x7F for byte in literal_bytes(literal_text))
        snippet = self.source_line(file_path, line)

        if self.tu_degraded:
            severity, reason = "R3", "TU 带致命诊断（best-effort 补扫），需人工复核"
        elif arg_class == "literal_narrow":
            if non_ascii:
                severity, reason = "R1", "窄字面量含非 ASCII 字节，经 ACP 转换不可逆乱码"
            else:
                severity, reason = "R2", "纯 ASCII 窄字面量（当前无损，模式仍错）"
        elif dynamic:
            severity, reason = "R1", "运行期窄内容边界（是否乱码取决于上游是否非 ASCII）"
        else:  # unknown_call
            severity, reason = "R3", "实参为未解析调用，需人工复核"

        self.findings[key] = {
            "file": rel,
            "line": line,
            "col": col,
            "module": module_of(rel),
            "flavor": flavor,
            "op": op,
            "context": context,
            "arg_class": arg_class,
            "detail": detail,
            "literal": literal_text,
            "non_ascii": non_ascii,
            "severity": severity,
            "reason": reason,
            "snippet": snippet,
        }

    def walk(self, cursor, context, tu):
        if cursor.kind == K.CALL_EXPR:
            info = path_call_info(cursor)
            if info is not None:
                flavor, op = info
                if op in TEXT_METHODS:
                    location = cursor.extent.start
                    self.record_compliance("path_dot_" + op, location.file.name, location.line)
                elif op in BRIDGE_METHODS:
                    location = cursor.extent.start
                    self.record_compliance(
                        "bridge_" + op, location.file.name, location.line
                    )
                else:
                    child_context = f"{flavor}::{op}"
                    for child in cursor.get_children():
                        self.inspect_arg(child, flavor, op, child_context, tu)
            else:
                ref = cursor.referenced
                if ref is not None and ref.spelling == "ToStdPath":
                    location = cursor.extent.start
                    self.record_compliance("tostdpath_ref", location.file.name, location.line)
        for child in cursor.get_children():
            self.walk(child, context, tu)

    def inspect_arg(self, arg_cursor, flavor, op, context, tu):
        node = unwrap_expr(arg_cursor)
        # 透过隐式 path 构造（operator/ 的 const path& 实参等）下降到源
        # 字符串实参；拷贝构造等无字符串形态则终止。
        for _ in range(4):
            if node.kind != K.CALL_EXPR:
                break
            info = path_call_info(node)
            if not info or info[1] != "ctor":
                break
            picked = None
            for child in node.get_children():
                child_class, _child_detail = classify_arg(child)
                if child_class is not None and child_class != "path_expr":
                    picked = unwrap_expr(child)
                    break
            if picked is None:
                return
            node = picked

        arg_class, detail = classify_arg(node)
        if arg_class is None or arg_class == "path_expr":
            return
        if node.kind == K.STRING_LITERAL:
            literal_text = literal_token_text(tu, node)
        else:
            literal_text = ""
        location = node.extent.start
        file_path = location.file.name if location.file else None

        if arg_class == "literal_wide":
            self.record_compliance("wide_literal", file_path, location.line)
        elif arg_class == "literal_u8":
            self.record_compliance("u8_literal", file_path, location.line)
        elif arg_class == "literal_u16u32":
            self.record_compliance("wide_literal", file_path, location.line)
        elif arg_class == "wide_type":
            self.record_compliance("wide_type_arg", file_path, location.line)
        elif arg_class == "u8_type":
            self.record_compliance("u8_type_arg", file_path, location.line)
        elif arg_class == "safe_to_string":
            self.record_compliance("to_string_arg", file_path, location.line)
        elif arg_class == "dyn_narrow" and node.kind in (
            K.BINARY_OPERATOR,
            K.CALL_EXPR,
            K.PAREN_EXPR,
        ):
            if all_ascii_components(node, tu):
                self.record_compliance("ascii_concat_arg", file_path, location.line)
            else:
                self.emit(
                    file_path,
                    location.line,
                    location.column,
                    None,
                    flavor,
                    op,
                    context,
                    arg_class,
                    detail,
                    literal_text,
                )
        elif arg_class in ("literal_narrow", "dyn_narrow", "unknown_call"):
            self.emit(
                file_path,
                location.line,
                location.column,
                None,
                flavor,
                op,
                context,
                arg_class,
                detail,
                literal_text,
            )


# ---------------------------------------------------------------------------
# TU 解析（flag 清洗沿用 dasptr_audit.py）
# ---------------------------------------------------------------------------

def tokenize_command(command):
    tokens = []
    buf = []
    state = None
    i = 0
    n = len(command)
    while i < n:
        ch = command[i]
        if state is None:
            if ch in (" ", "\t"):
                if buf:
                    tokens.append("".join(buf))
                    buf = []
            elif ch == "'":
                state = "'"
            elif ch == '"':
                state = '"'
            elif ch == "\\" and i + 1 < n and command[i + 1] == '"':
                buf.append('"')
                i += 1
            else:
                buf.append(ch)
        elif state == "'":
            if ch == "'":
                state = None
            else:
                buf.append(ch)
        else:
            if ch == '"':
                state = None
            elif ch == "\\" and i + 1 < n and command[i + 1] == '"':
                buf.append('"')
                i += 1
            else:
                buf.append(ch)
        i += 1
    if buf:
        tokens.append("".join(buf))
    return tokens


def resolve_resource_dir():
    """resource dir 解析：env CLANG_RESOURCE_DIR 显式配置 > PATH 上 clang 自报。

    clang -print-resource-dir 是编译器对自身安装布局的唯一权威口径，免
    去版本号子目录猜测；PATH 无 clang 或命令失败返回 None，相关解析按
    degraded 语义提示（环境由使用者保证，不做硬编码兜底）。
    """
    environment_resource = os.environ.get("CLANG_RESOURCE_DIR")
    if environment_resource:
        return environment_resource
    clang_executable = shutil.which("clang")
    if clang_executable is None:
        return None
    try:
        probe = subprocess.run(
            [clang_executable, "-print-resource-dir"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return None
    if probe.returncode != 0:
        return None
    return probe.stdout.strip() or None


def resolve_isystem_entries(clang_pp_executable):
    """问 clang driver 的系统头搜索列表（尖括号段，保持 driver 顺序）。

    -E -v 空输入时 driver 在 stderr 自报两组搜索路径；跳过 `#include
    "..."` 引号段，只取 `#include <...> search starts here:` 到
    `End of search list.` 之间的非空行——这是 driver 编译时实际使用
    的搜索序（含 resource dir 自带 include），比任何安装布局推导都权威。
    命令失败或输出无标记段返回 None。
    """
    try:
        probe = subprocess.run(
            [clang_pp_executable, "-xc++", "-E", "-v", "-"],
            input="",
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError:
        return None
    if probe.returncode != 0:
        return None
    entries = []
    in_angle_section = False
    for line in probe.stderr.splitlines():
        stripped = line.strip()
        if stripped == "#include <...> search starts here:":
            in_angle_section = True
            continue
        if stripped == "End of search list.":
            break
        if in_angle_section and stripped:
            entries.append(stripped.replace("\\", "/"))
    return entries or None


_TOOLCHAIN_HINTS = None


def toolchain_hints():
    """返回 (resource_dir, isystem 组)，进程内缓存一次探测结果。

    两项都问 PATH 上的 clang driver（-print-resource-dir / -E -v 搜索
    列表），driver 自报即编译时真相，免做发行版布局猜测；
    CLANG_RESOURCE_DIR 可显式覆盖 resource dir。全 staged 文件/全 TU
    共用缓存，不逐 TU 起子进程。resource dir 缺失时进程内告警一次
    （多进程并行时每 worker 各一次，属诚实的环境报告）。
    """
    global _TOOLCHAIN_HINTS
    if _TOOLCHAIN_HINTS is not None:
        return _TOOLCHAIN_HINTS

    resource_dir = resolve_resource_dir()
    isystem_entries = []
    clang_pp_executable = shutil.which("clang++")
    if clang_pp_executable is not None:
        driver_entries = resolve_isystem_entries(clang_pp_executable)
        if driver_entries is not None:
            isystem_entries = driver_entries
    if resource_dir is None:
        print(
            "[check_path_text_encoding] warning: clang resource dir not "
            "resolvable (set CLANG_RESOURCE_DIR or put clang/clang++ on "
            "PATH); related parses degrade"
        )
    _TOOLCHAIN_HINTS = (resource_dir, isystem_entries)
    return _TOOLCHAIN_HINTS


def clean_flags(args, path):
    args = args[1:]
    normalized_file = path.replace("\\", "/")
    cleaned = []
    skip_next = False
    for argument in args:
        if skip_next:
            skip_next = False
            continue
        if argument.replace("\\", "/") == normalized_file:
            continue
        if argument == "-o":
            skip_next = True
            continue
        if argument == "-c":
            continue
        cleaned.append(argument)
    resource_dir, isystem_entries = toolchain_hints()
    if resource_dir:
        cleaned.extend(["-resource-dir", resource_dir])
    # libclang 不跑 driver 的 mingw sysroot 探测，需补 resource-dir 与
    # 工具库头路径；二者从 PATH 上 clang 动态解析，缺省时该 TU 按
    # degraded 语义在 parse_tu 处失败/跳过。
    for entry in isystem_entries:
        cleaned.extend(["-isystem", entry])
    return cleaned


def parse_tu(index, path, args, allow_degraded=False):
    try:
        tu = index.parse(path, args=args, options=0)
    except Exception as exc:  # noqa: BLE001
        return None, f"parse exception: {exc}"
    if tu is None:
        return None, "parse returned None"
    fatal = [
        diagnostic.spelling
        for diagnostic in tu.diagnostics
        if diagnostic.severity >= cindex.Diagnostic.Error
    ]
    if fatal and not allow_degraded:
        return None, "fatal diagnostics: " + "; ".join(fatal[:3])
    return tu, ("degraded: " + "; ".join(fatal[:2]) if fatal else "")


# ---------------------------------------------------------------------------
# 并行 worker（Windows spawn：函数必须模块顶层可导入；每进程惰性持有
# 独立 Index —— libclang Index 非线程安全，进程隔离后各自创建）
# ---------------------------------------------------------------------------

_WORKER_INDEX = None


def _worker_parse(job):
    """解析单个 TU 并返回该 TU 的全部审计产物（picklable dict）。

    walk 阶段抛出的异常降级为 failed 记录，单 TU 失败不影响整池。
    """
    global _WORKER_INDEX
    path, args, degraded_allowed = job
    result = {
        "path": path,
        "failed": False,
        "error": "",
        "is_cpp": path.endswith((".cpp", ".cc", ".cxx")),
        "includes": [],
        "findings": [],
        "compliance_items": [],
        "counted_ok": False,
    }
    try:
        if _WORKER_INDEX is None:
            _WORKER_INDEX = cindex.Index.create()
        tu, error = parse_tu(
            _WORKER_INDEX, path, args, allow_degraded=degraded_allowed
        )
        if tu is None:
            result["failed"] = True
            result["error"] = error
            return result
        auditor = Auditor()
        auditor.tu_degraded = degraded_allowed and bool(error)
        auditor.walk(tu.cursor, "", tu)
        try:
            for inclusion in tu.get_includes():
                if inclusion.include and inclusion.include.name:
                    result["includes"].append(
                        inclusion.include.name.replace("\\", "/")
                    )
        except Exception:  # noqa: BLE001
            pass
        result["findings"] = list(auditor.findings.values())
        result["compliance_items"] = [
            (kind, entry)
            for kind, entries in auditor.compliance.items()
            for entry in entries
        ]
        result["error"] = error
        result["counted_ok"] = not (degraded_allowed and error)
        return result
    except Exception as exc:  # noqa: BLE001
        result["failed"] = True
        result["error"] = f"worker exception: {exc}"
        return result


def fallback_flags_for(path):
    """EXTRA_FILES 补扫与 staged 补扫共用的 best-effort flags。

    .h 需显式 -x c++-header，否则 libclang 按 C 头解析。工具链提示
    （resource-dir/isystem）从 PATH 上 clang 动态解析，缺省时仅保留
    语言形态 flags（解析大概率 degraded，由调用方提示）。
    """
    arguments = ["-std=c++20", "-x", "c++"]
    resource_dir, isystem_entries = toolchain_hints()
    if resource_dir:
        arguments.extend(["-resource-dir", resource_dir])
    for entry in isystem_entries:
        arguments.extend(["-isystem", entry])
    if path.endswith(".h"):
        arguments.extend(["-x", "c++-header"])
    return arguments


def build_targets(compile_db_path, file_filters, prefix_filters):
    with open(compile_db_path, encoding="utf-8") as handle:
        entries = json.load(handle)

    seen = set()
    targets = []  # (path, args, degraded_allowed)
    for entry in entries:
        path = entry["file"]
        rel = repo_rel(path)
        if rel is None:
            continue
        normalized = path.replace("\\", "/")
        if normalized in seen:
            continue
        if file_filters and not any(flt in normalized for flt in file_filters):
            continue
        if prefix_filters and not any(
            rel.startswith(prefix) for prefix in prefix_filters
        ):
            continue
        seen.add(normalized)
        args = list(entry.get("arguments", []) or [])
        if not args:
            args = tokenize_command(entry.get("command", ""))
        targets.append((normalized, clean_flags(args, normalized), False))

    for extra in EXTRA_FILES:
        normalized = extra.replace("\\", "/")
        if not os.path.isfile(normalized):
            continue
        rel = repo_rel(normalized)
        if file_filters and not any(flt in normalized for flt in file_filters):
            continue
        if prefix_filters and (
            rel is None or not any(rel.startswith(prefix) for prefix in prefix_filters)
        ):
            continue
        resource_dir, isystem_entries = toolchain_hints()
        if resource_dir is None and not isystem_entries:
            print(
                f"[check_path_text_encoding] warning: skipped - extra scan for "
                f"{normalized} (clang toolchain paths not resolvable)",
                file=sys.stderr,
            )
            continue
        targets.append((normalized, fallback_flags_for(normalized), True))

    return targets


def run_serial(targets):
    """--jobs 1 的串行路径：与历史版本行为一致（共享单 Index 逐 TU）。"""
    index = cindex.Index.create()
    auditor = Auditor()
    covered_headers = set()
    covered_cpps = set()
    walked_files = set()
    total = len(targets)

    for position, (path, args, degraded_allowed) in enumerate(targets):
        tu, error = parse_tu(index, path, args, allow_degraded=degraded_allowed)
        if tu is None:
            auditor.parse_errors.append({"file": path, "error": error})
            print(
                f"[{position + 1}/{total}] {path} -> ERROR: {error[:120]}",
                file=sys.stderr,
            )
            continue
        if path.endswith((".cpp", ".cc", ".cxx")):
            covered_cpps.add(path)
        walked_files.add(path)
        try:
            for inclusion in tu.get_includes():
                if inclusion.include and inclusion.include.name:
                    included = inclusion.include.name.replace("\\", "/")
                    covered_headers.add(included)
                    walked_files.add(included)
        except Exception:  # noqa: BLE001
            pass
        auditor.tu_degraded = degraded_allowed and bool(error)
        auditor.walk(tu.cursor, "", tu)
        if not (degraded_allowed and error):
            auditor.parsed_ok += 1
        status = f"degraded({error[:60]})" if degraded_allowed and error else "ok"
        print(
            f"[{position + 1}/{total}] {path} -> {status}, findings={len(auditor.findings)}",
            file=sys.stderr,
        )

    return auditor, covered_headers, covered_cpps, walked_files


def run_parallel(targets, jobs):
    """--jobs N 的并行路径：executor.map 按提交序聚合，与串行等价。"""
    auditor = Auditor()
    covered_headers = set()
    covered_cpps = set()
    walked_files = set()
    total = len(targets)

    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for position, result in enumerate(
            pool.map(_worker_parse, targets, chunksize=1)
        ):
            path = result["path"]
            if result["failed"]:
                auditor.parse_errors.append(
                    {"file": path, "error": result["error"]}
                )
                print(
                    f"[{position + 1}/{total}] {path} -> ERROR: "
                    f"{result['error'][:120]}",
                    file=sys.stderr,
                )
                continue
            if result["is_cpp"]:
                covered_cpps.add(path)
            walked_files.add(path)
            covered_headers.update(result["includes"])
            walked_files.update(result["includes"])
            for finding in result["findings"]:
                key = (finding["file"], finding["line"], finding["col"])
                if key not in auditor.findings:
                    auditor.findings[key] = finding
            for kind, entry in result["compliance_items"]:
                if entry not in auditor.compliance[kind]:
                    auditor.compliance[kind].append(entry)
            if result["counted_ok"]:
                auditor.parsed_ok += 1
            error = result["error"]
            status = f"degraded({error[:60]})" if error else "ok"
            print(
                f"[{position + 1}/{total}] {path} -> {status}, "
                f"findings={len(auditor.findings)}",
                file=sys.stderr,
            )

    return auditor, covered_headers, covered_cpps, walked_files


def merge_with_baseline(baseline_path, filtered_result, walked_files, filtered_tus):
    """过滤跑结果按文件整替换合并进全量基线。

    walked_files（绝对路径集合）覆盖到的文件：findings 与 compliance 全部
    以过滤跑为准（含"清零"—— 该文件不再有条目即从合并结果移除）；未覆盖
    文件沿用基线原值。parse_errors 同口径合并。
    """
    with open(baseline_path, encoding="utf-8") as handle:
        baseline = json.load(handle)

    walked_rel = {rel for rel in (repo_rel(f) for f in walked_files) if rel}

    findings = [
        f for f in baseline.get("findings", [])
        if f["file"] not in walked_rel
        and (f["file"], f["line"]) not in HELPER_FINDING_WHITELIST
    ]
    findings.extend(
        f for f in filtered_result["findings"]
        if (f["file"], f["line"]) not in HELPER_FINDING_WHITELIST
    )
    findings.sort(
        key=lambda f: (f["severity"], f["module"], f["file"], f["line"])
    )

    compliance = {}
    for kind, entries in baseline.get("compliance", {}).items():
        kept = [entry for entry in entries if entry[0] not in walked_rel]
        if kept:
            compliance[kind] = kept
    for kind, entries in filtered_result["compliance"].items():
        compliance.setdefault(kind, []).extend(entries)
    compliance = {
        kind: sorted([tuple(entry) for entry in set(map(tuple, entries))])
        for kind, entries in compliance.items()
    }
    compliance = {
        kind: [list(entry) for entry in entries]
        for kind, entries in compliance.items()
    }

    baseline_errors = baseline.get("coverage", {}).get("parse_errors", [])
    kept_errors = [
        err for err in baseline_errors if repo_rel(err["file"]) not in walked_rel
    ]
    merged_errors = kept_errors + filtered_result["coverage"]["parse_errors"]

    merged = {
        "merged_from": baseline_path.replace("\\", "/"),
        "filtered_tus": filtered_tus,
        "coverage": {
            **baseline.get("coverage", {}),
            "parse_errors": merged_errors,
        },
        "compliance": compliance,
        "findings": findings,
    }
    return merged


# ---------------------------------------------------------------------------
# 全仓模式（audit runner / 手动审计入口）
# ---------------------------------------------------------------------------

def run_full_repo_mode(args):
    prefix_filters = [
        prefix.strip().replace("\\", "/")
        for prefix in args.filter_prefix.split(",")
        if prefix.strip()
    ]

    started = time.perf_counter()
    targets = build_targets(args.compile_db, args.file_filters, prefix_filters)
    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 1)
    if jobs == 1:
        auditor, covered_headers, covered_cpps, walked_files = run_serial(targets)
    else:
        auditor, covered_headers, covered_cpps, walked_files = run_parallel(
            targets, jobs
        )

    # ---- 覆盖率 ----
    scope_headers = set()
    scope_cpps = set()
    for top in SCOPE_TOP:
        top_dir = os.path.join(REPO_ROOT.replace("/", os.sep), top)
        if not os.path.isdir(top_dir):
            continue
        for root, dirs, files in os.walk(top_dir):
            dirs[:] = [d for d in dirs if d not in EXCLUDED_PARTS]
            for name in files:
                rel = repo_rel(os.path.join(root, name).replace("\\", "/"))
                if rel is None:
                    continue
                if name.endswith((".h", ".hpp")):
                    scope_headers.add(rel)
                elif name.endswith((".cpp", ".cc", ".cxx")):
                    scope_cpps.add(rel)

    covered_headers_rel = {
        rel for rel in (repo_rel(h) for h in covered_headers) if rel
    }
    covered_cpps_rel = {
        rel for rel in (repo_rel(c) for c in covered_cpps) if rel
    }

    result = {
        "coverage": {
            "targets": len(targets),
            "parsed_ok": auditor.parsed_ok,
            "parse_errors": auditor.parse_errors,
            "scope_cpp_total": len(scope_cpps),
            "scope_cpp_covered": len(scope_cpps & covered_cpps_rel),
            "scope_cpp_uncovered": sorted(scope_cpps - covered_cpps_rel),
            "scope_header_total": len(scope_headers),
            "scope_header_covered": len(scope_headers & covered_headers_rel),
        },
        "compliance": {
            kind: sorted(values) for kind, values in auditor.compliance.items()
        },
        "findings": sorted(
            auditor.findings.values(),
            key=lambda f: (f["severity"], f["module"], f["file"], f["line"]),
        ),
    }

    if args.merge_baseline:
        result = merge_with_baseline(
            args.merge_baseline, result, walked_files, len(targets)
        )

    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(result, handle, ensure_ascii=False, indent=1)

    by_severity = defaultdict(int)
    for finding in result["findings"]:
        by_severity[finding["severity"]] += 1
    elapsed = time.perf_counter() - started
    print(
        f"done: R1={by_severity['R1']} R2={by_severity['R2']} "
        f"R3={by_severity['R3']} ({len(targets)} TUs, jobs={jobs}, "
        f"{elapsed:.1f}s) -> {args.output}",
        file=sys.stderr,
    )
    return 0


# ---------------------------------------------------------------------------
# 暂存增量模式（pre-commit 第三道检查）
# ---------------------------------------------------------------------------

# 与 check_cpp_naming 的 CPP_EXTS 相同（hook 链内两个检查的消费面一致）
CPP_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".hh", ".cuh"}


def staged_cpp_files():
    """返回暂存区改动（新增/复制/修改）的 C++ 文件，相对仓库根路径。"""
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"],
        cwd=ROOT, capture_output=True, text=True,
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
    """解析 git diff --cached -U0 的 hunk，返回暂存版本（new 侧）的改动行范围。

    context=0 时 new 侧 count 为 0 表示纯删除 hunk，须跳过。
    """
    out = subprocess.run(
        ["git", "diff", "--cached", "-U0", "--", rel_path],
        cwd=ROOT, capture_output=True, text=True,
        encoding="utf-8", errors="replace", check=True,
    ).stdout
    ranges = []
    for line in out.splitlines():
        if not line.startswith("@@"):
            continue
        # @@ -old,oldc +new,newc @@
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


def collect_preset_binary_dirs():
    """收集 CMakePresets.json + CMakeUserPresets.json 全部预设的生效 binaryDir。

    只做 db 两层查找的预设层，不做 build/*/ glob 兜底——glob 会把非预设
    树也拖进候选，多树场景失去「预设即真相」的确定性。
    """
    presets = {}
    for preset_file_name in ("CMakePresets.json", "CMakeUserPresets.json"):
        preset_file = ROOT / preset_file_name
        if not preset_file.is_file():
            continue
        try:
            with open(preset_file, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        # 同名预设 User 文件覆盖公共文件（CMake 语义）
        for preset in data.get("configurePresets", []):
            presets[preset["name"]] = preset

    source_dir_text = str(ROOT).replace("\\", "/")

    def effective_binary_dir(preset_name, visiting):
        preset = presets.get(preset_name)
        if preset is None or preset_name in visiting:
            return None
        own = preset.get("binaryDir")
        if own:
            return own.replace("${sourceDir}", source_dir_text)
        # 自身无 binaryDir 则沿父链取首个定义者（如 ember 继承 mingw-debug）
        inherits = preset.get("inherits")
        parent_names = (
            [inherits] if isinstance(inherits, str) else list(inherits or [])
        )
        next_visiting = visiting | {preset_name}
        for parent_name in parent_names:
            resolved = effective_binary_dir(parent_name, next_visiting)
            if resolved:
                return resolved
        return None

    binary_dirs = set()
    for preset_name in presets:
        resolved = effective_binary_dir(preset_name, set())
        if resolved:
            binary_dirs.add(resolved.replace("\\", "/"))
    return binary_dirs


def load_compile_db_entries(db_path):
    """读取 compile db；不可读/非数组返回 None（供候选 fallback 判定）。"""
    try:
        with open(db_path, encoding="utf-8") as handle:
            entries = json.load(handle)
    except (OSError, ValueError):
        return None
    return entries if isinstance(entries, list) else None


def locate_compile_db():
    """两层查找 compile db：env pin → 预设树探测（mtime 最新优先）。

    返回 (db 路径, entries)；两层全落空返回 (None, None)。
    """
    environment_pin = os.environ.get("DAS_COMPILE_COMMANDS_JSON")
    if environment_pin:
        if os.path.isfile(environment_pin):
            entries = load_compile_db_entries(environment_pin)
            if entries is not None:
                return environment_pin.replace("\\", "/"), entries
            print(
                "[check_path_text_encoding] warning: DAS_COMPILE_COMMANDS_JSON="
                f"{environment_pin} unreadable, falling back to preset probing"
            )
        else:
            print(
                "[check_path_text_encoding] warning: DAS_COMPILE_COMMANDS_JSON="
                f"{environment_pin} not found, falling back to preset probing"
            )

    candidates = []
    for binary_dir in collect_preset_binary_dirs():
        candidate = os.path.join(binary_dir, "compile_commands.json")
        if os.path.isfile(candidate):
            candidates.append(candidate)
    candidates.sort(key=os.path.getmtime, reverse=True)
    for candidate in candidates:
        entries = load_compile_db_entries(candidate)
        if entries is not None:
            return candidate.replace("\\", "/"), entries
        print(
            f"[check_path_text_encoding] warning: compile db {candidate} "
            "unreadable, trying next candidate"
        )
    return None, None


def run_staged_check(file_ranges, compile_db_path):
    """对 (文件, 变更行范围) 列表跑增量检查，返回命中变更行的 findings。

    独立于 CLI 可 import 驱动（历史提交重放等）：调用方自备 file_ranges
    （仓内相对路径 + [(start, end)] 列表）与 compile db 路径。逐文件串行
    解析（staged 集小，不值得拉进程池）；白名单过滤在 Auditor.emit 内完成。
    """
    if cindex is None:
        raise RuntimeError("clang.cindex unavailable in current interpreter")

    with open(compile_db_path, encoding="utf-8") as handle:
        db_entries = json.load(handle)
    entries_by_file = {}
    for entry in db_entries:
        entries_by_file.setdefault(entry["file"].replace("\\", "/"), entry)

    index = cindex.Index.create()
    findings = []
    for relative_path, ranges in file_ranges:
        absolute_path = str((ROOT / relative_path).resolve()).replace("\\", "/")
        entry = entries_by_file.get(absolute_path)
        if entry is not None:
            arguments = list(entry.get("arguments", []) or [])
            if not arguments:
                arguments = tokenize_command(entry.get("command", ""))
            parse_arguments = clean_flags(arguments, absolute_path)
            degraded_allowed = False
        else:
            resource_dir, isystem_entries = toolchain_hints()
            if resource_dir is None and not isystem_entries:
                print(
                    f"[check_path_text_encoding] warning: skipped - "
                    f"{relative_path} semantic check (clang toolchain paths "
                    "not resolvable)"
                )
                continue
            # staged .h / 未构建过的新 .cpp：照 EXTRA_FILES 分支补扫
            print(
                f"[check_path_text_encoding] {relative_path} not in compile db, "
                "best-effort scan"
            )
            parse_arguments = fallback_flags_for(absolute_path)
            degraded_allowed = True

        translation_unit, error = parse_tu(
            index, absolute_path, parse_arguments, allow_degraded=degraded_allowed
        )
        if translation_unit is None:
            print(
                f"[check_path_text_encoding] warning: skipped - "
                f"{relative_path} semantic check (TU parse failed: {error[:80]})"
            )
            continue

        auditor = Auditor()
        auditor.tu_degraded = degraded_allowed and bool(error)
        auditor.walk(translation_unit.cursor, "", translation_unit)

        changed_lines = set()
        for start, end in ranges:
            changed_lines.update(range(start, end + 1))
        for finding in auditor.findings.values():
            if finding["file"] == relative_path and finding["line"] in changed_lines:
                findings.append(finding)
    return findings


def run_staged_mode():
    files = staged_cpp_files()
    if not files:
        print("[check_path_text_encoding] no staged C++ changes")
        return 0

    file_ranges = []
    for relative_path in files:
        ranges = changed_line_ranges(relative_path)
        if ranges:
            file_ranges.append((relative_path, ranges))
    if not file_ranges:
        print("[check_path_text_encoding] no changed lines to check")
        return 0

    compile_db_path, _entries = locate_compile_db()
    if compile_db_path is None:
        print(
            "[check_path_text_encoding] warning: skipped - compile_commands.json "
            "not found (set DAS_COMPILE_COMMANDS_JSON or configure a preset build)"
        )
        return 0
    print(f"[check_path_text_encoding] using compile db: {compile_db_path}")

    findings = run_staged_check(file_ranges, compile_db_path)
    if findings:
        print(
            "[check_path_text_encoding] path text encoding violations "
            "(changed lines only):"
        )
        for finding in sorted(findings, key=lambda item: (item["file"], item["line"])):
            print(
                f"  {finding['file']}:{finding['line']} [{finding['severity']}] "
                f"{finding['flavor']}::{finding['op']} {finding['reason']}"
            )
        return 1
    print("[check_path_text_encoding] path text encoding check passed")
    return 0


# ---------------------------------------------------------------------------
# CLI 入口（模式分派）
# ---------------------------------------------------------------------------

def build_argument_parser():
    parser = argparse.ArgumentParser(
        description="std/boost filesystem path 窄串拼接审计（全仓 + 暂存增量双模式）"
    )
    parser.add_argument(
        "--staged",
        action="store_true",
        help="检查暂存区改动行（pre-commit 增量模式）",
    )
    parser.add_argument(
        "compile_db",
        nargs="?",
        default=None,
        help="compile_commands.json 路径（全仓模式）",
    )
    parser.add_argument(
        "output",
        nargs="?",
        default=None,
        help="输出 findings JSON 路径（全仓模式）",
    )
    parser.add_argument(
        "file_filters",
        nargs="*",
        help="路径子串过滤（冒烟/复核用，与 --filter-prefix 可共存）",
    )
    parser.add_argument(
        "--filter-prefix",
        default="",
        help="逗号分隔的仓内相对路径前缀；只解析命中前缀的 TU",
    )
    parser.add_argument(
        "--merge-baseline",
        default=None,
        help="全量基线 JSON；与 --filter-prefix 组合按文件整替换合并",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="并行进程数；0/1=串行，默认 CPU 逻辑核数",
    )
    return parser


def console_main(argv=None):
    parser = build_argument_parser()
    args = parser.parse_args(argv)

    if not args.staged:
        if not args.compile_db or not args.output:
            parser.error(
                "full-repo mode requires compile_db and output positional "
                "arguments (or pass --staged)"
            )
        prefix_list = [
            prefix.strip()
            for prefix in args.filter_prefix.split(",")
            if prefix.strip()
        ]
        if args.merge_baseline and not prefix_list:
            parser.error("--merge-baseline 仅与 --filter-prefix 组合使用")

    if cindex is None:
        # 环境由使用者保证：当前解释器 import 不了 clang 即按缺失放行，
        # 不做任何「替使用者找解释器」的兜底（hook 层经 DAS_PYTHON 换解释器）
        print(
            "[check_path_text_encoding] warning: skipped - libclang unavailable "
            "(current python cannot import clang)"
        )
        return 0

    if args.staged:
        # 非强制检查 fail-open：检查器自身 bug 不得卡死提交
        try:
            return run_staged_mode()
        except Exception as exc:  # noqa: BLE001
            first_line = (
                str(exc).splitlines()[0] if str(exc) else exc.__class__.__name__
            )
            print(
                "[check_path_text_encoding] warning: internal error, fail-open "
                f"({first_line})",
                file=sys.stderr,
            )
            return 0

    return run_full_repo_mode(args)


if __name__ == "__main__":
    sys.exit(console_main())
