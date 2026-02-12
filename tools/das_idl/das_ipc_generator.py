"""
DAS IPC 代码生成器 - 统一入口

提供 IPC 代理、存根和消息结构的代码生成功能。

用法（通过 das_idl_gen.py）:
    python das_idl_gen.py -i interfaces.idl --ipc --ipc-output-dir ./ipc_gen
    python das_idl_gen.py -i interfaces.idl --ipc-message --ipc-output-dir ./ipc_gen
    python das_idl_gen.py -i interfaces.idl --ipc-proxy --ipc-output-dir ./ipc_gen
    python das_idl_gen.py -i interfaces.idl --ipc-stub --ipc-output-dir ./ipc_gen

生成内容:
    1. IPC 消息结构定义 (messages/<InterfaceName>Messages.h)
    2. IPC 代理代码 (proxy/<InterfaceName>Proxy.h)
    3. IPC 存根代码 (stub/<InterfaceName>Stub.h) - 待实现
"""

from typing import List, Optional
from pathlib import Path

# 导入 IDL 解析器类型
try:
    from . import das_idl_parser as _das_idl_parser
except ImportError:
    import importlib
    import sys
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")

IdlDocument = _das_idl_parser.IdlDocument


def generate_ipc_files(
    document: IdlDocument,
    output_dir: str,
    cache_dir: Optional[str] = None,
    base_name: Optional[str] = None,
    idl_file_path: Optional[str] = None,
    generate_proxy: bool = False,
    generate_stub: bool = False,
    generate_message: bool = True
) -> List[str]:
    """生成 IPC 代码文件

    Args:
        document: IDL 文档对象
        output_dir: 输出目录
        cache_dir: 中间缓存目录（可选）
        base_name: 基础文件名（可选）
        idl_file_path: IDL 文件路径（可选）
        generate_proxy: 是否生成代理代码
        generate_stub: 是否生成存根代码
        generate_message: 是否生成消息结构

    Returns:
        生成的文件路径列表
    """
    generated_files = []

    # 确保输出目录存在
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    # 生成消息结构
    if generate_message:
        from das_ipc_message_generator import generate_ipc_message_files
        message_files = generate_ipc_message_files(
            document=document,
            output_dir=output_dir,
            base_name=base_name,
            idl_file_path=idl_file_path
        )
        generated_files.extend(message_files)

    # 生成代理代码
    if generate_proxy:
        from das_ipc_proxy_generator import generate_ipc_proxy_files
        proxy_files = generate_ipc_proxy_files(
            document=document,
            output_dir=output_dir,
            base_name=base_name,
            idl_file_path=idl_file_path
        )
        generated_files.extend(proxy_files)

    # 生成存根代码
    if generate_stub:
        from das_ipc_stub_generator import generate_ipc_stub_files
        stub_files = generate_ipc_stub_files(
            document=document,
            output_dir=output_dir,
            base_name=base_name,
            idl_file_path=idl_file_path
        )
        generated_files.extend(stub_files)

    return generated_files


# 测试代码
if __name__ == '__main__':
    parse_idl_file = _das_idl_parser.parse_idl_file

    import sys
    if len(sys.argv) > 1:
        idl_file = sys.argv[1]
        output_dir = sys.argv[2] if len(sys.argv) > 2 else "./ipc_output"

        print(f"Parsing IDL file: {idl_file}")
        document = parse_idl_file(idl_file)

        print(f"Generating IPC files to: {output_dir}")
        files = generate_ipc_files(
            document=document,
            output_dir=output_dir,
            idl_file_path=idl_file,
            generate_message=True
        )

        print(f"\nGenerated {len(files)} files:")
        for f in files:
            print(f"  - {f}")
    else:
        print("Usage: python das_ipc_generator.py <idl_file> [output_dir]")
