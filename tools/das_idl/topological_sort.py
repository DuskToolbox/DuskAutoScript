#!/usr/bin/env python3
"""
拓扑排序工具 - 使用 Kahn's algorithm 对 SWIG 接口进行排序

读取 swig_deps.json 中的依赖关系图，输出排序后的接口列表。

依赖关系：interface A 依赖 interface B → B 必须在 A 之前处理
"""

import json
import sys
import argparse
from pathlib import Path
from collections import deque


def load_dependencies(deps_file: Path) -> dict:
    """加载依赖关系图

    Args:
        deps_file: swig_deps.json 文件路径

    Returns:
        依赖关系图: {interface: [dependencies], ...}
    """
    with open(deps_file, 'r', encoding='utf-8') as f:
        data = json.load(f)

    return data['dependency_graph']


def topological_sort_kahn(graph: dict) -> list:
    """使用 Kahn's algorithm 进行拓扑排序

    Args:
        graph: 依赖关系图 {node: [dependencies], ...}

    Returns:
        排序后的节点列表
    """
    # 计算所有节点
    all_nodes = set(graph.keys())
    for deps in graph.values():
        all_nodes.update(deps)

    # 计算入度（被依赖次数）
    in_degree = {node: 0 for node in all_nodes}
    for node, deps in graph.items():
        for dep in deps:
            in_degree[node] += 1

    # 初始化队列：入度为 0 的节点
    queue = deque([node for node in all_nodes if in_degree[node] == 0])

    # 排序队列以稳定输出
    queue = deque(sorted(queue))

    result = []

    while queue:
        # 按字母顺序取出节点
        queue = deque(sorted(queue))
        node = queue.popleft()
        result.append(node)

        # 找到所有依赖此节点的节点
        dependent_nodes = []
        for n, deps in graph.items():
            if node in deps:
                dependent_nodes.append(n)

        # 减少这些节点的入度
        for dependent in dependent_nodes:
            in_degree[dependent] -= 1
            if in_degree[dependent] == 0:
                queue.append(dependent)

    # 检查是否有环
    if len(result) != len(all_nodes):
        cycle_nodes = all_nodes - set(result)
        raise ValueError(f"发现循环依赖，涉及节点: {cycle_nodes}")

    return result


def main():
    """主函数"""
    parser = argparse.ArgumentParser(
        description="对 SWIG 接口进行拓扑排序",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python tools/topological_sort.py
  python tools/topological_sort.py --deps-file swig_deps.json --output sorted_interfaces.txt
        """
    )

    parser.add_argument(
        '--deps-file',
        default='swig_deps.json',
        help='输入的依赖关系 JSON 文件路径 (默认: swig_deps.json)'
    )

    parser.add_argument(
        '--output',
        default='sorted_interfaces.txt',
        help='输出的排序列表文件路径 (默认: sorted_interfaces.txt)'
    )

    args = parser.parse_args()

    deps_file = Path(args.deps_file)
    output_file = Path(args.output)

    if not deps_file.exists():
        print(f"错误: 依赖文件不存在: {deps_file}", file=sys.stderr)
        sys.exit(1)

    # 加载依赖关系
    graph = load_dependencies(deps_file)
    print(f"加载依赖关系: {len(graph)} 个接口")

    # 执行拓扑排序
    sorted_interfaces = topological_sort_kahn(graph)
    print(f"拓扑排序完成: {len(sorted_interfaces)} 个接口")

    # 输出到文件
    output_file.parent.mkdir(parents=True, exist_ok=True)
    with open(output_file, 'w', encoding='utf-8') as f:
        for interface in sorted_interfaces:
            f.write(f"{interface}\n")

    print(f"排序结果已写入: {output_file}")

    # 验证依赖关系
    interface_positions = {name: i for i, name in enumerate(sorted_interfaces)}
    violations = []
    for interface, deps in graph.items():
        for dep in deps:
            if interface_positions[interface] < interface_positions[dep]:
                violations.append((interface, dep))

    if violations:
        print(f"\n警告: 发现 {len(violations)} 个依赖关系违反", file=sys.stderr)
        for interface, dep in violations[:5]:  # 只显示前 5 个
            print(f"  {interface} 依赖于 {dep}，但位置相反", file=sys.stderr)
        if len(violations) > 5:
            print(f"  ... 还有 {len(violations) - 5} 个", file=sys.stderr)
    else:
        print("\n[OK] 依赖关系验证通过")


if __name__ == '__main__':
    main()
