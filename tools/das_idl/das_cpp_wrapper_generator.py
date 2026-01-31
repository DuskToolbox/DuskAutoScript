"""
DAS C++ 包装代码生成器
生成便捷 C++ 包装类

功能:
1. RAII 智能指针包装 (基于 DasPtr<T>)
2. [out] 参数转换为返回值
3. [get]/[set] 属性转换为便捷方法调用
4. DasResult 错误自动抛出 DasException

依赖边界规则:
- Wrapper头文件（Das.Xxx.hpp）仅包含类声明
- 包装实现文件（Das.Xxx.impl.hpp）仅包含方法实现
- Wrapper头文件 -> ABI头文件（单向依赖）
- 包装实现文件 -> Wrapper头文件（单向依赖）
- 禁止循环依赖

生成文件命名: Das.Xxx.hpp (类似 winrt/Windows.Foundation.h)
"""

import os
import glob
from pathlib import Path
from datetime import datetime, timezone
import importlib
import sys
from typing import List, Set, Optional

# 既支持作为包内模块导入（tools.das_idl.*），也支持直接脚本运行。
# 注意：避免使用 `from das_idl_parser import ...` 这种“隐式相对导入”形式，
# 否则 LSP/pyright 会报 reportImplicitRelativeImport。
try:
    from . import das_idl_parser as _das_idl_parser
except ImportError:  # pragma: no cover
    # 直接运行该脚本时（__package__ 为空），确保同目录可被导入。
    this_dir = str(Path(__file__).resolve().parent)
    if this_dir not in sys.path:
        sys.path.insert(0, this_dir)
    _das_idl_parser = importlib.import_module("das_idl_parser")

IdlDocument = _das_idl_parser.IdlDocument
InterfaceDef = _das_idl_parser.InterfaceDef
EnumDef = _das_idl_parser.EnumDef
MethodDef = _das_idl_parser.MethodDef
PropertyDef = _das_idl_parser.PropertyDef
ParameterDef = _das_idl_parser.ParameterDef
TypeInfo = _das_idl_parser.TypeInfo
ParamDirection = _das_idl_parser.ParamDirection


class CppWrapperTypeMapper:
    """C++ 包装类型映射器"""

    # IDL 类型到 C++ 类型的映射
    TYPE_MAP = {
        'bool': 'bool',
        'int8': 'int8_t',
        'int16': 'int16_t',
        'int32': 'int32_t',
        'int64': 'int64_t',
        'uint8': 'uint8_t',
        'uint16': 'uint16_t',
        'uint32': 'uint32_t',
        'uint64': 'uint64_t',
        'float': 'float',
        'double': 'double',
        'size_t': 'size_t',
        'int': 'int32_t',
        'uint': 'uint32_t',
        'DasResult': 'DasResult',
        'DasBool': 'DasBool',
        'DasGuid': 'DasGuid',
        'DasString': 'DasReadOnlyString',
        'DasReadOnlyString': 'DasReadOnlyString',
        'IDasBase': 'DasBase',
        'void': 'void',
    }

    @staticmethod
    def is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型 (以 I 开头的类型)"""
        return type_name.startswith('I') and len(type_name) > 1 and type_name[1:2].isupper()

    @staticmethod
    def is_string_type(type_name: str) -> bool:
        """判断是否是字符串类型"""
        return type_name in ('DasString', 'DasReadOnlyString', 'IDasReadOnlyString')

    @classmethod
    def get_cpp_type(cls, type_info: TypeInfo) -> str:
        """将 IDL 类型转换为 C++ 类型"""
        base = cls.TYPE_MAP.get(type_info.base_type, type_info.base_type)

        result = ""
        if type_info.is_const:
            result += "const "

        result += base

        if type_info.is_pointer:
            result += "*" * type_info.pointer_level

        if type_info.is_reference:
            result += "&"

        return result

    @classmethod
    def get_wrapper_class_name(cls, interface_name: str) -> str:
        """获取包装类名称: IDasXxx -> DasXxx

        支持带命名空间的接口类型，例如：
        - IDasComponent -> DasComponent
        - ::Das::PluginInterface::IDasComponent -> ::Das::PluginInterface::DasComponent
        """
        # 处理带命名空间的类型（如 ::Das::PluginInterface::IDasComponent）
        if '::' in interface_name:
            parts = interface_name.split('::')
            last_part = parts[-1]

            # 转换最后一部分的接口名为包装类名
            if last_part.startswith('IDas'):
                wrapper_name = last_part[1:]
            elif last_part.startswith('I'):
                wrapper_name = last_part[1:]
            else:
                wrapper_name = last_part

            # 保留命名空间前缀，只替换最后一部分
            if parts[0] == '':  # 全局命名空间前缀
                return '::' + '::'.join(parts[1:-1] + [wrapper_name])
            return '::'.join(parts[:-1] + [wrapper_name])

        # 处理不带命名空间的简单接口名
        if interface_name.startswith('IDas'):
            return interface_name[1:]
        if interface_name.startswith('I'):
            return interface_name[1:]
        return interface_name

    @classmethod
    def get_return_type_for_out_param(cls, type_info: TypeInfo, has_binary_buffer: bool = False) -> str:
        """获取 [out] 参数作为返回值时的类型"""
        base = type_info.base_type

        # 处理 [binary_buffer] 属性：对于 unsigned char** 返回 unsigned char*
        if has_binary_buffer and base == 'unsigned char' and type_info.pointer_level == 2:
            return 'unsigned char*'

        # IDasBase 特殊处理：返回 DasBase
        if base == 'IDasBase':
            return 'DasBase'

        # 接口类型返回智能指针包装
        if cls.is_interface_type(base):
            # 对于带命名空间的接口类型，get_wrapper_class_name 已经返回完整的命名空间限定符
            # 例如: ::Das::PluginInterface::IDasComponent -> ::Das::PluginInterface::DasComponent
            wrapper_name = cls.get_wrapper_class_name(base)
            return wrapper_name

        # 字符串类型
        if cls.is_string_type(base):
            return 'DasReadOnlyString'

        # 基本类型直接返回
        return cls.TYPE_MAP.get(base, base)


class CppWrapperGenerator:
    """C++ 包装代码生成器 - 生成类似 C++/WinRT 风格的代码"""

    def __init__(self, document: IdlDocument, namespace: str = "Das", idl_file_path: Optional[str] = None):
        self.document = document
        self.namespace = namespace
        self.idl_file_path = idl_file_path  # 保存IDL文件路径
        self.idl_file_name = os.path.basename(idl_file_path) if idl_file_path else None  # IDL文件名
        self.indent = "    "  # 4 空格缩进
        # 收集依赖的接口类型
        self._dependent_interfaces: Set[str] = set()
        # 缓存类型命名空间映射
        self._type_namespace_map = None
        # 导入的IDL文档映射: {文件名: IdlDocument}
        self._imported_documents = {}
        # 解析导入的IDL文件
        self._parse_imported_documents()

    def _parse_imported_documents(self):
        """解析导入的 IDL 文件，构建导入文档映射"""
        if not self.idl_file_path or not self.document.imports:
            return

        from pathlib import Path
        parse_idl_file = _das_idl_parser.parse_idl_file

        # 获取当前 IDL 文件所在的目录（相对于 idl 根目录）
        idl_path = Path(self.idl_file_path)
        parts_lower = [p.lower() for p in idl_path.parts]
        if 'idl' not in parts_lower:
            return

        idx = parts_lower.index('idl')
        idl_dir = Path(*idl_path.parts[:idx + 1])
        current_idl_dir = idl_path.parent if idl_path != idl_dir else idl_dir

        # 递归解析导入的 IDL 文件
        parsed_files = set()

        def parse_imports_recursive(import_list, base_dir):
            for import_def in import_list:
                import_path_str = import_def.idl_path.strip('"')
                import_path = Path(base_dir) / import_path_str

                # 规范化路径
                try:
                    import_path = import_path.resolve()
                except:
                    continue

                # 检查是否已解析
                import_key = str(import_path)
                if import_key in parsed_files:
                    continue

                parsed_files.add(import_key)

                # 解析 IDL 文件
                try:
                    if import_path.exists():
                        imported_doc = parse_idl_file(str(import_path))
                        # 使用文件名（不含扩展名）作为键
                        import_filename = import_path.stem
                        self._imported_documents[import_filename] = imported_doc

                        # 递归解析导入的文件的导入
                        if imported_doc.imports:
                            import_base_dir = import_path.parent
                            parse_imports_recursive(imported_doc.imports, import_base_dir)
                except Exception as e:
                    print(f"警告: 无法解析导入的 IDL 文件 {import_path}: {e}")

        parse_imports_recursive(self.document.imports, current_idl_dir)

    def _generate_enum_to_string(self, enum: EnumDef) -> str:
        """为枚举生成 ToString 方法（窄字符和宽字符两个版本，直接返回字符串字面量指针）"""
        lines = []

        lines.append(f"// ToString functions for {enum.name}")
        lines.append(f"inline const char* ToString({enum.name} value) noexcept")
        lines.append("{")
        lines.append(f"{self.indent}switch (value)")
        lines.append(f"{self.indent}{{")

        # 生成窄字符版本的 switch case
        for value in enum.values:
            lines.append(f"{self.indent}{self.indent}case {enum.name}::{value.name}:")
            lines.append(f"{self.indent}{self.indent}{self.indent}return \"{value.name}\";")

        lines.append(f"{self.indent}{self.indent}default:")
        lines.append(f"{self.indent}{self.indent}{self.indent}return \"<Unknown>\";")
        lines.append(f"{self.indent}}}")
        lines.append("}")
        lines.append("")

        # 生成宽字符版本
        lines.append(f"inline const wchar_t* ToWString({enum.name} value) noexcept")
        lines.append("{")
        lines.append(f"{self.indent}switch (value)")
        lines.append(f"{self.indent}{{")

        for value in enum.values:
            lines.append(f"{self.indent}{self.indent}case {enum.name}::{value.name}:")
            lines.append(f"{self.indent}{self.indent}{self.indent}return L\"{value.name}\";")

        lines.append(f"{self.indent}{self.indent}default:")
        lines.append(f"{self.indent}{self.indent}{self.indent}return L\"<Unknown>\";")
        lines.append(f"{self.indent}}}")
        lines.append("}")
        lines.append("")

        return "\n".join(lines)

    def _to_namespace_path(self, namespace: str) -> str:
        """将命名空间转换为文件路径格式，例如: DAS::ExportInterface -> DAS.ExportInterface"""
        if not namespace:
            return ""
        return namespace.replace("::", ".")

    def _get_import_includes(self) -> List[str]:
        """根据 imports 生成 include 路径列表

        import语句使用相对路径，需要转换为正确的abi头文件路径。
        例如: import "../DasBasicTypes.idl"; -> #include "DasTypes.h"
              import "./IDasImage.idl"; -> #include "ExportInterface/IDasImage.h"
              import "IDasImage.idl"; -> #include "ExportInterface/IDasImage.h"
              import "../ExportInterface/IDasCapture.idl"; -> #include "ExportInterface/IDasCapture.h"

        同时生成wrapper头文件的include，例如: import "IDasVariantVector.idl"; -> #include "Das.ExportInterface.IDasVariantVector.hpp"

        ABI头文件与wrapper文件在同一目录中。

        当IDL文件导入其他IDL文件时（例如 IDasImage.idl 导入 IDasBinaryBuffer.idl），
        生成的wrapper头文件应同时包含 ABI 头文件和 wrapper 头文件：
        - #include "IDasBinaryBuffer.h"         (ABI 头文件)
        - #include "Das.ExportInterface.IDasBinaryBuffer.hpp"  (Wrapper 头文件)
        """
        includes = []

        # 获取当前IDL文件的目录（相对于idl目录）
        current_idl_dir = ""
        if self.idl_file_path:
            from pathlib import Path
            idl_path = Path(self.idl_file_path)
            parts_lower = [p.lower() for p in idl_path.parts]
            if 'idl' in parts_lower:
                idx = parts_lower.index('idl')
                idl_dir = Path(*idl_path.parts[:idx+1])
                if idl_path != idl_dir:
                    current_idl_dir = str(idl_path.parent.relative_to(idl_dir))
                    if current_idl_dir == '.':
                        current_idl_dir = ""

        for imp in self.document.imports:
            # 从IDL文件路径中提取文件名（不含扩展名）
            idl_path = imp.idl_path
            # 移除引号（如果有）
            idl_path = idl_path.strip('"')

            # 获取文件名部分
            import_name = os.path.basename(idl_path)
            # 移除.idl扩展名
            if import_name.endswith('.idl'):
                import_name = import_name[:-4]

            # 解析import路径，生成对应的.h文件路径
            # import路径是相对于当前IDL文件所在目录的
            from pathlib import PurePath

            # 解析import路径，规范化处理 .. 和 .
            import_rel_path = PurePath(idl_path)

            # 计算import文件的完整相对路径（相对于idl目录）
            if current_idl_dir:
                current_dir = PurePath(current_idl_dir)
                # 拼接路径
                full_path = current_dir / import_rel_path
            else:
                full_path = import_rel_path

            # 规范化路径，解析 . 和 ..
            normalized = full_path

            # 处理 ../ 和 ./
            parts = []
            for part in normalized.parts:
                if part == '.':
                    continue
                elif part == '..':
                    if parts:
                        parts.pop()
                else:
                    parts.append(part)

            # 构建ABI头文件路径
            if parts:
                if len(parts) > 1:
                    # 有目录
                    import_dir = '/'.join(parts[:-1])
                    h_path = f"{import_dir}/{import_name}.h"
                else:
                    # 只有文件
                    h_path = f"{import_name}.h"
            else:
                h_path = f"{import_name}.h"

            # 添加 ABI 头文件
            includes.append(h_path)

            # 注意：wrapper头文件的include由 _generate_dependent_wrapper_includes() 在类声明后生成
            # 这里不再生成wrapper include，以避免在文件头部错误包含wrapper文件

        return includes

    def _get_wrapper_impl_includes(self) -> List[str]:
        """获取包装实现文件的 include 列表（Das.ExportInterface.*.hpp）

        这个方法返回所有 import 语句对应的包装实现文件路径，
        用于在类声明块之后、ctor/dtor 实现之前 include。

        Returns:
            包装实现文件路径列表，例如: ["Das.ExportInterface.IDasImage.hpp"]
        """
        includes = []

        # 获取当前IDL文件的目录（相对于idl目录）
        current_idl_dir = ""
        if self.idl_file_path:
            from pathlib import Path
            idl_path = Path(self.idl_file_path)
            parts_lower = [p.lower() for p in idl_path.parts]
            if 'idl' in parts_lower:
                idx = parts_lower.index('idl')
                idl_dir = Path(*idl_path.parts[:idx+1])
                if idl_path != idl_dir:
                    current_idl_dir = str(idl_path.parent.relative_to(idl_dir))
                    if current_idl_dir == '.':
                        current_idl_dir = ""

        for imp in self.document.imports:
            # 从IDL文件路径中提取文件名（不含扩展名）
            idl_path = imp.idl_path
            idl_path = idl_path.strip('"')

            # 获取文件名部分
            import_name = os.path.basename(idl_path)
            if import_name.endswith('.idl'):
                import_name = import_name[:-4]

            # 构建wrapper头文件路径
            wrapper_filename = None
            for interface in self.document.interfaces:
                if interface.name == import_name:
                    if interface.namespace:
                        namespace_path = self._to_namespace_path(interface.namespace)
                        wrapper_filename = f"{namespace_path}.{import_name}.hpp"
                    else:
                        wrapper_filename = f"{import_name}.hpp"
                    break
            for enum in self.document.enums:
                if enum.name == import_name:
                    if enum.namespace:
                        namespace_path = self._to_namespace_path(enum.namespace)
                        wrapper_filename = f"{namespace_path}.{import_name}.hpp"
                    else:
                        wrapper_filename = f"{import_name}.hpp"
                    break

            # 如果在当前文档中找不到，尝试从导入的文档中查找
            if wrapper_filename is None and import_name in self._imported_documents:
                imported_doc = self._imported_documents[import_name]

                for interface in imported_doc.interfaces:
                    if interface.name == import_name:
                        if interface.namespace:
                            namespace_path = self._to_namespace_path(interface.namespace)
                            wrapper_filename = f"{namespace_path}.{import_name}.hpp"
                        else:
                            wrapper_filename = f"{import_name}.hpp"
                        break

                if wrapper_filename is None:
                    for enum in imported_doc.enums:
                        if enum.name == import_name:
                            if enum.namespace:
                                namespace_path = self._to_namespace_path(enum.namespace)
                                wrapper_filename = f"{namespace_path}.{import_name}.hpp"
                            else:
                                wrapper_filename = f"{import_name}.hpp"
                            break

            # 只添加包装实现文件（.hpp 文件）
            if wrapper_filename:
                includes.append(wrapper_filename)

        return includes

    def _file_header(self, guard_name: str, interface_names: List[str]) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        import_includes = self._get_import_includes()

        # ABI 头文件：一个 IDL 文件对应一个 ABI 头文件（IDasImage.idl -> IDasImage.h）
        abi_includes = []
        if self.idl_file_name:
            abi_header = os.path.splitext(self.idl_file_name)[0] + ".h"
            abi_includes.append(abi_header)

        includes = import_includes + abi_includes

        includes_str = "\n".join(f'#include "{inc}"' for inc in sorted(set(includes)))

        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"

        type_includes_str = "#include <das/DasException.hpp>\n#include <das/DasBase.hpp>\n#include <das/DasString.hpp>\n"

        namespace_declarations = {}
        type_namespace_map = self._collect_type_namespace_mapping()

        # 验证依赖的接口类型是否存在
        self._validate_dependent_interfaces()

        for interface in self.document.interfaces:
            current_ns = interface.namespace

            for type_name in self._dependent_interfaces:
                wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(type_name)
                if wrapper_type in type_namespace_map:
                    type_ns = type_namespace_map[wrapper_type]
                    type_included = False
                    for inc in includes:
                        if wrapper_type in inc and '.hpp' in inc:
                            type_included = True
                            break

                    if type_ns and type_ns != current_ns and not type_included:
                        abi_name = type_name
                        if abi_name.startswith('::'):
                            abi_name = abi_name[2:]
                        abi_name = abi_name.replace('::', '/')
                        abi_include = f"{abi_name}.h"
                        if abi_include not in includes:
                            includes.append(abi_include)

                        if type_ns not in namespace_declarations:
                            namespace_declarations[type_ns] = set()
                        namespace_declarations[type_ns].add(wrapper_type)

        forward_declarations_str = ""
        if namespace_declarations:
            lines = []
            for namespace in sorted(namespace_declarations.keys()):
                # _generate_namespace_open/_close 会返回末尾带换行的多行字符串。
                # 这里逐行拼接，避免 join 时引入多余空行。
                namespace_open = self._generate_namespace_open(namespace).rstrip("\n")
                if namespace_open:
                    lines.extend(namespace_open.splitlines())

                indent = "    " * len(namespace.split("::"))
                for type_name in sorted(namespace_declarations[namespace]):
                    lines.append(f"{indent}class {type_name};")

                namespace_close = self._generate_namespace_close(namespace).rstrip("\n")
                if namespace_close:
                    lines.extend(namespace_close.splitlines())

            forward_declarations_str = "\n".join(lines)

        return f"""#if !defined({guard_name})
#define {guard_name}

// This file is automatically generated by DAS IDL Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!
//
// wrapper classes for DAS interfaces
// Provides RAII, exception-based error handling, and convenient APIs
//

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
{type_includes_str}

{includes_str}
{forward_declarations_str}
"""

    def _file_footer(self, guard_name: str) -> str:
        """生成文件尾"""
        return f"""
#endif // {guard_name}
"""

    def _to_upper_snake(self, name: str) -> str:
        """将 PascalCase 转换为 UPPER_SNAKE_CASE"""
        result = []
        for i, c in enumerate(name):
            if c.isupper() and i > 0:
                result.append('_')
            result.append(c.upper())
        return ''.join(result)

    def _generate_namespace_open(self, namespace: str) -> str:
        """生成命名空间开始标记，支持 C++17 嵌套命名空间语法（Allman 风格）"""
        if namespace:
            # 将嵌套命名空间转换为 Allman 风格
            # 例如："Das::Core" -> "namespace Das\n{\n    namespace Core\n{"
            parts = namespace.split("::")
            result = []
            for i, part in enumerate(parts):
                indent = "    " * i
                result.append(f"{indent}namespace {part}")
                result.append(f"{indent}{{")
            return "\n".join(result) + "\n"
        return ""

    def _generate_namespace_close(self, namespace: str) -> str:
        """生成命名空间结束标记（Allman 风格）"""
        if namespace:
            # 将嵌套命名空间转换为 Allman 风格（反向关闭）
            # 例如："Das::Core" -> "    }\n}\n// namespace Das::Core"
            parts = namespace.split("::")
            result = []
            # 从内层到外层关闭，indent 从 len(parts)-1 递减到 0
            for i in range(len(parts) - 1, -1, -1):
                indent = "    " * i
                result.append(f"{indent}}}")
            result.append(f"// namespace {namespace}")
            return "\n".join(result) + "\n"
        return ""

    def _collect_dependent_interfaces(self, interface: InterfaceDef):
        """收集接口依赖的其他接口类型"""
        for method in interface.methods:
            if CppWrapperTypeMapper.is_interface_type(method.return_type.base_type):
                self._dependent_interfaces.add(method.return_type.base_type)

            for param in method.parameters:
                if CppWrapperTypeMapper.is_interface_type(param.type_info.base_type):
                    self._dependent_interfaces.add(param.type_info.base_type)

        for prop in interface.properties:
            if CppWrapperTypeMapper.is_interface_type(prop.type_info.base_type):
                self._dependent_interfaces.add(prop.type_info.base_type)

        for method in interface.methods:
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    wrapper_type = CppWrapperTypeMapper.get_return_type_for_out_param(param.type_info)
                    if wrapper_type and wrapper_type != "void":
                        # 对于[out]参数，参数的base_type就是接口类型
                        self._dependent_interfaces.add(param.type_info.base_type)

    def _collect_type_namespace_mapping(self) -> dict[str, Optional[str]]:
        """收集所有接口类型到其命名空间的映射

        Returns:
            字典，键为类型名（如 DasVariantVector），值为命名空间（如 Das::ExportInterface）
        """
        if self._type_namespace_map is not None:
            return self._type_namespace_map

        mapping = {}

        for interface in self.document.interfaces:
            wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
            mapping[wrapper_name] = interface.namespace
            mapping[interface.name] = interface.namespace

        for doc_name, doc in self._imported_documents.items():
            for interface in doc.interfaces:
                wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
                mapping[wrapper_name] = interface.namespace
                mapping[interface.name] = interface.namespace
            for enum in doc.enums:
                mapping[enum.name] = enum.namespace

        self._type_namespace_map = mapping
        return mapping

    def _validate_dependent_interfaces(self) -> None:
        """验证依赖的接口类型是否存在

        检查所有依赖的接口类型是否在 import 的 IDL 文件或当前 IDL 文件中定义。
        如果某个接口类型不存在，报错并退出。

        跳过内置接口类型（IDasReadOnlyString、IDasBase、IDasSwigBase）。

        Raises:
            RuntimeError: 当接口类型未找到时
        """
        builtin_interfaces = {'IDasReadOnlyString', 'IDasBase', 'IDasSwigBase'}

        for type_name in self._dependent_interfaces:
            # 跳过内置接口类型
            simple_name = type_name.split('::')[-1]
            if simple_name in builtin_interfaces:
                continue

            # 跳过非接口类型（不是以'I'开头的类型，如int64_t、uint64_t）
            if not simple_name.startswith('I'):
                continue

            found = False

            for interface in self.document.interfaces:
                if interface.name == simple_name:
                    found = True
                    break

            if not found:
                for doc in self._imported_documents.values():
                    for interface in doc.interfaces:
                        if interface.name == simple_name:
                            found = True
                            break
                    if found:
                        break

            if not found:
                error_msg = (
                    f"Error: Interface type '{type_name}' not found.\n"
                    f"Please ensure that interface is defined in an IDL file and "
                    f"imported in {self.idl_file_path}"
                )
                raise RuntimeError(error_msg)

    def _collect_type_to_idl_file_mapping(self) -> dict[str, str]:
        """收集接口类型名到其所在IDL文件名的映射

        因为一个IDL文件对应一个wrapper hpp文件，所以生成include路径时
        需要使用IDL文件名而不是接口类型名。

        Returns:
            字典，键为接口类型名（如 IDasInputFactory），值为IDL文件名（如 IDasInput）
        """
        mapping = {}

        # 当前文档中的接口 -> 当前IDL文件名
        if self.idl_file_name:
            current_idl_basename = os.path.splitext(self.idl_file_name)[0]
            for interface in self.document.interfaces:
                mapping[interface.name] = current_idl_basename

        # 导入文档中的接口 -> 对应IDL文件名
        for doc_name, doc in self._imported_documents.items():
            for interface in doc.interfaces:
                mapping[interface.name] = doc_name

        return mapping

    def _find_interface_definition(self, interface_name: str) -> Optional[InterfaceDef]:
        """查找接口定义

        Args:
            interface_name: 接口名称（如 IDasTypeInfo）

        Returns:
            找到的 InterfaceDef，未找到返回 None
        """
        # 先在当前文档中查找
        for interface in self.document.interfaces:
            if interface.name == interface_name:
                return interface

        # 在导入的文档中查找
        for doc in self._imported_documents.values():
            for interface in doc.interfaces:
                if interface.name == interface_name:
                    return interface

        return None

    def _collect_all_methods_and_properties(self, interface: InterfaceDef, visited: Optional[Set[str]] = None) -> tuple[list, list]:
        """递归收集接口的所有方法和属性（包括继承的）

        Args:
            interface: 接口定义
            visited: 已访问的接口集合，用于避免循环继承

        Returns:
            元组 (all_methods, all_properties)
            - all_methods: 所有方法列表（从基类到派生类排序，派生类方法覆盖基类同名方法）
            - all_properties: 所有属性列表（从基类到派生类排序，派生类属性覆盖基类同名属性）
        """
        if visited is None:
            visited = set()

        # 防止循环继承
        if interface.name in visited:
            return [], []

        visited.add(interface.name)

        # 如果有基类且基类不是 IDasBase，递归收集基类的方法和属性
        all_methods = []
        all_properties = []

        if interface.base_interface and interface.base_interface != "IDasBase":
            base_interface = self._find_interface_definition(interface.base_interface)
            if base_interface:
                base_methods, base_properties = self._collect_all_methods_and_properties(base_interface, visited)
                all_methods.extend(base_methods)
                all_properties.extend(base_properties)

        # 添加当前接口的方法和属性（派生类覆盖基类同名方法/属性）
        # 使用字典来跟踪方法/属性签名，实现覆盖逻辑
        method_signatures = {}
        for method in all_methods:
            signature = (method.name, tuple(p.type_info.base_type for p in method.parameters))
            method_signatures[signature] = method

        for method in interface.methods:
            signature = (method.name, tuple(p.type_info.base_type for p in method.parameters))
            method_signatures[signature] = method

        # 重建方法列表（按添加顺序，派生类的会覆盖基类的）
        all_methods = list(method_signatures.values())

        # 同样处理属性
        property_names = {}
        for prop in all_properties:
            property_names[prop.name] = prop

        for prop in interface.properties:
            property_names[prop.name] = prop

        all_properties = list(property_names.values())

        return all_methods, all_properties


    def _get_qualified_type_name(self, type_name: str, current_namespace: str) -> str:
        """获取带命名空间限定符的完整类型名

        Args:
            type_name: 类型名称（如 DasVariantVector 或 ::Das::PluginInterface::DasComponent）
            current_namespace: 当前命名空间（如 Das::PluginInterface）

        Returns:
            完整的类型名，如果类型在不同命名空间则添加命名空间限定符
        """
        # 如果类型已经包含命名空间限定符（全局命名空间或嵌套命名空间），直接返回
        if '::' in type_name:
            return type_name

        type_namespace_map = self._collect_type_namespace_mapping()

        # 如果类型在映射中且不在当前命名空间，添加命名空间限定符
        if type_name in type_namespace_map:
            type_ns = type_namespace_map[type_name]
            if type_ns and type_ns != current_namespace:
                return f"{type_ns}::{type_name}"

        return type_name

    def _get_dasptr_qualified_type(self, current_namespace: Optional[str]) -> str:
        """获取 DasPtr 的完全限定名称

        根据 current_namespace 决定是否需要添加 DAS:: 前缀：
        - 如果 current_namespace 为空（全局命名空间），返回 DAS::DasPtr
        - 如果 current_namespace 以 Das 开头（例如 Das 或 Das::PluginInterface），返回 DasPtr
        - 否则返回 DAS::DasPtr

        Args:
            current_namespace: 当前命名空间（如 "Das", "Das::PluginInterface", None）

        Returns:
            DasPtr 类型名（根据命名空间决定是否带 DAS:: 前缀）
        """
        if current_namespace is None:
            # 全局命名空间，需要 DAS:: 前缀
            return "DAS::DasPtr"

        # 检查当前命名空间是否以 Das 开头
        if current_namespace == "Das" or current_namespace.startswith("Das::"):
            # 已在 Das 命名空间内，直接使用 DasPtr
            return "DasPtr"

        # 不在 Das 命名空间内，需要 DAS:: 前缀
        return "DAS::DasPtr"

    def _generate_wrapper_class_declaration(self, interface: InterfaceDef, namespace_depth: int = 0) -> str:
        """生成包装类声明（仅声明，不包含实现）

        根据依赖边界规则，wrapper头文件仅包含类声明。

        Args:
            interface: 接口定义
            namespace_depth: 命名空间嵌套深度（默认 0，表示无命名空间或外层命名空间）
        """
        self._collect_dependent_interfaces(interface)

        # 收集所有方法和属性（包括继承的）
        all_methods, all_properties = self._collect_all_methods_and_properties(interface)

        raw_name = interface.name
        wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(raw_name)
        current_namespace = interface.namespace

        # 获取 DasPtr 的完全限定名称
        dasptr_qualified_type = self._get_dasptr_qualified_type(current_namespace)

        # 基于命名空间深度计算缩进
        # 无命名空间：0 空格（类体内容 4 空格）
        # 1 层命名空间（Das）：4 空格（类体内容 8 空格）
        # 2 层命名空间（Das::PluginInterface）：8 空格（类体内容 12 空格）
        class_indent = "    " * namespace_depth
        member_indent = "    " * (namespace_depth + 1)

        lines = []

        lines.append(f"{class_indent}/**")
        lines.append(f"{class_indent} * @brief wrapper for {raw_name}")
        lines.append(f"{class_indent} * ")
        lines.append(f"{class_indent}* Provides RAII memory management, exception-based error handling,")
        lines.append(f"{class_indent}* and convenient property/method APIs.")
        lines.append(f"{class_indent}*/")

        lines.append(f"{class_indent}class {wrapper_name}")
        lines.append(f"{class_indent}{{")
        lines.append(f"{class_indent}private:")
        lines.append(f"{member_indent}{dasptr_qualified_type}<{raw_name}> ptr_;")
        lines.append("")
        lines.append(f"{class_indent}public:")

        lines.append(f"{member_indent}/// @brief 默认构造函数，创建空包装")
        lines.append(f"{member_indent}{wrapper_name}() noexcept = default;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 从原始接口指针构造（获取所有权）")
        lines.append(f"{member_indent}explicit(false) {wrapper_name}({raw_name}* p) noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 从 DasPtr 构造")
        lines.append(f"{member_indent}{wrapper_name}({dasptr_qualified_type}<{raw_name}> ptr) noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 获取底层原始接口指针")
        lines.append(f"{member_indent}{raw_name}* Get() const noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 获取底层 DasPtr")
        lines.append(f"{member_indent}const {dasptr_qualified_type}<{raw_name}>& GetPtr() const noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 隐式转换到原始指针")
        lines.append(f"{member_indent}operator {raw_name}*() const noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 检查是否持有有效对象")
        lines.append(f"{member_indent}explicit operator bool() const noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 访问原始接口成员")
        lines.append(f"{member_indent}{raw_name}* operator->() const noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 附加到现有指针")
        lines.append(f"{member_indent}static {wrapper_name} Attach({raw_name}* p) noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 获取指针的指针")
        lines.append(f"{member_indent}{raw_name}** Put() noexcept;")
        lines.append("")

        lines.append(f"{member_indent}/// @brief 获取 void 指针的指针")
        lines.append(f"{member_indent}void** PutVoid() noexcept;")
        lines.append("")

        for method in all_methods:
            method_decl = self._generate_method_wrapper(interface, method, mode='declaration', namespace_depth=namespace_depth)
            lines.append(method_decl)

        for prop in all_properties:
            prop_decl, _ = self._generate_property_wrapper(interface, prop, mode='declaration')

            # _generate_property_wrapper 当前使用 self.indent（固定 4 空格）。
            # 为了让声明缩进与 wrapper class 成员一致，这里根据 namespace_depth
            # 将首层缩进从 self.indent 替换为 member_indent。
            if namespace_depth > 0:
                reindented_lines = []
                for line in prop_decl.splitlines(keepends=True):
                    if line.strip() and line.startswith(self.indent):
                        reindented_lines.append(member_indent + line[len(self.indent):])
                    else:
                        reindented_lines.append(line)
                prop_decl = "".join(reindented_lines)

            lines.append(prop_decl)

        lines.append(f"{class_indent}}};")
        lines.append("")
        return "\n".join(lines) + "\n"

    def _generate_wrapper_class_implementation(self, interface: InterfaceDef) -> str:
        """生成包装类实现（仅实现，不包含声明）

        根据依赖边界规则，包装实现文件仅包含方法实现。
        """
        # 收集所有方法和属性（包括继承的）
        all_methods, all_properties = self._collect_all_methods_and_properties(interface)

        raw_name = interface.name
        wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(raw_name)
        current_namespace = interface.namespace

        # 获取 DasPtr 的完全限定名称
        dasptr_qualified_type = self._get_dasptr_qualified_type(current_namespace)

        lines = []

        lines.append(f"{wrapper_name}::{wrapper_name}({raw_name}* p) noexcept : ptr_(p) {{}}")
        lines.append("")
        lines.append(f"{wrapper_name}::{wrapper_name}({dasptr_qualified_type}<{raw_name}> ptr) noexcept : ptr_(std::move(ptr)) {{}}")
        lines.append("")
        lines.append(f"{wrapper_name}::operator {raw_name}*() const noexcept {{ return ptr_.Get(); }}")
        lines.append("")
        lines.append(f"{wrapper_name}::operator bool() const noexcept {{ return ptr_ != nullptr; }}")
        lines.append("")
        lines.append(f"{wrapper_name} {wrapper_name}::Attach({raw_name}* p) noexcept")
        lines.append("{")
        lines.append(f"    {wrapper_name} wrapper;")
        lines.append(f"    wrapper.ptr_ = {dasptr_qualified_type}<{raw_name}>::Attach(p);")
        lines.append(f"    return wrapper;")
        lines.append(f"}}")
        lines.append("")
        lines.append(f"{raw_name}** {wrapper_name}::Put() noexcept")
        lines.append("{")
        lines.append(f"    return ptr_.Put();")
        lines.append(f"}}")
        lines.append("")
        lines.append(f"void** {wrapper_name}::PutVoid() noexcept")
        lines.append("{")
        lines.append(f"    return ptr_.PutVoid();")
        lines.append(f"}}")
        lines.append("")
        lines.append(f"{raw_name}* {wrapper_name}::Get() const noexcept")
        lines.append("{")
        lines.append(f"    return ptr_.Get();")
        lines.append(f"}}")
        lines.append("")
        lines.append(f"const {dasptr_qualified_type}<{raw_name}>& {wrapper_name}::GetPtr() const noexcept")
        lines.append("{")
        lines.append(f"    return ptr_;")
        lines.append(f"}}")
        lines.append("")
        lines.append(f"{raw_name}* {wrapper_name}::operator->() const noexcept")
        lines.append("{")
        lines.append(f"    return ptr_.Get();")
        lines.append(f"}}")
        lines.append("")

        for method in all_methods:
            method_impl = self._generate_method_wrapper(interface, method, mode='implementation')
            lines.append(method_impl)

        for prop in all_properties:
            _, prop_impls = self._generate_property_wrapper(interface, prop, mode='implementation')
            lines.extend(prop_impls)

        return "\n".join(lines) + "\n"

    def _generate_method_wrapper(self, interface: InterfaceDef, method: MethodDef, mode: str = 'inline', namespace_depth: int = 0) -> str:
        """生成方法包装

        Args:
            interface: 接口定义
            method: 方法定义
            mode: 生成模式，'inline'（默认，向后兼容）、'declaration'（只生成声明）、'implementation'（只生成类外实现）
            namespace_depth: 命名空间嵌套深度（默认 0）

        Returns:
            生成的代码字符串
        """
        lines = []

        wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
        current_namespace = interface.namespace
        has_binary_buffer = method.attributes.get('binary_buffer', False) if method.attributes else False

        in_params = []
        out_params = []

        for param in method.parameters:
            if param.direction == ParamDirection.OUT:
                out_params.append(param)
            else:
                in_params.append(param)

        if out_params:
            if len(out_params) == 1:
                ret_type = CppWrapperTypeMapper.get_return_type_for_out_param(out_params[0].type_info, has_binary_buffer)
                ret_type = self._get_qualified_type_name(ret_type, current_namespace)
            else:
                ret_types = [CppWrapperTypeMapper.get_return_type_for_out_param(p.type_info) for p in out_params]
                ret_types = [self._get_qualified_type_name(t, current_namespace) for t in ret_types]
                ret_type = f"std::tuple<{', '.join(ret_types)}>"
        else:
            if method.return_type.base_type == 'DasResult' or method.return_type.base_type == 'bool':
                ret_type = "void"
            else:
                ret_type = CppWrapperTypeMapper.get_cpp_type(method.return_type)

        param_decls = []
        for param in in_params:
            param_type = CppWrapperTypeMapper.get_cpp_type(param.type_info)
            if CppWrapperTypeMapper.is_interface_type(param.type_info.base_type):
                wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(param.type_info.base_type)
                wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                param_decls.append(f"const {wrapper_type}& {param.name}")
            elif CppWrapperTypeMapper.is_string_type(param.type_info.base_type):
                param_decls.append(f"const {param_type}& {param.name}")
            elif param.type_info.is_reference and not param.type_info.is_const:
                param_decls.append(f"{param_type} {param.name}")
            else:
                param_decls.append(f"{param_type} {param.name}")

        param_str = ", ".join(param_decls)

        if mode == 'declaration':
            # 使用动态缩进，基于命名空间深度
            indent = "    " * (namespace_depth + 1)
            lines.append(f"{indent}/// @brief 调用 {interface.name}::{method.name}")
            if out_params:
                lines.append(f"{indent}/// @return {'返回输出值' if len(out_params) == 1 else '返回多个输出值的 tuple'}")
            lines.append(f"{indent}/// @throws DasException 当操作失败时")
            lines.append(f"{indent}{ret_type} {method.name}({param_str}) const;")
            lines.append("")
            return "\n".join(lines)

        elif mode == 'implementation':
            body_indent = self.indent
            lines.append(f"{ret_type} {wrapper_name}::{method.name}({param_str}) const")
            lines.append("{")

            for param in out_params:
                base_type = param.type_info.base_type
                if CppWrapperTypeMapper.is_interface_type(base_type):
                    qualified_base_type = self._get_qualified_type_name(base_type, current_namespace)
                    lines.append(f"{body_indent}{qualified_base_type}* {param.name}_raw = nullptr;")
                else:
                    if has_binary_buffer and base_type == 'unsigned char' and param.type_info.pointer_level == 2:
                        cpp_type = 'unsigned char*'
                        lines.append(f"{body_indent}{cpp_type} {param.name}_value{{}};")
                    else:
                        cpp_type = CppWrapperTypeMapper.TYPE_MAP.get(base_type, base_type)
                        lines.append(f"{body_indent}{cpp_type} {param.name}_value{{}};")

            call_args = []
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    base_type = param.type_info.base_type
                    if CppWrapperTypeMapper.is_interface_type(base_type):
                        call_args.append(f"&{param.name}_raw")
                    else:
                        call_args.append(f"&{param.name}_value")
                else:
                    if CppWrapperTypeMapper.is_interface_type(param.type_info.base_type):
                        call_args.append(f"{param.name}.Get()")
                    elif CppWrapperTypeMapper.is_string_type(param.type_info.base_type):
                        call_args.append(f"{param.name}.Get()")
                    else:
                        call_args.append(param.name)

            call_args_str = ", ".join(call_args)

            if method.return_type.base_type == 'DasResult' or method.return_type.base_type == 'bool':
                lines.append(f"{body_indent}const DasResult result = ptr_->{method.name}({call_args_str});")
            else:
                lines.append(f"{body_indent}auto result = ptr_->{method.name}({call_args_str});")

            lines.append(f"{body_indent}DAS_THROW_IF_FAILED(result);")

            if out_params:
                if len(out_params) == 1:
                    param = out_params[0]
                    base_type = param.type_info.base_type
                    if CppWrapperTypeMapper.is_interface_type(base_type):
                        wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                        wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                        lines.append(f"{body_indent}return {wrapper_type}::Attach({param.name}_raw);")
                    else:
                        lines.append(f"{body_indent}return {param.name}_value;")
                else:
                    ret_values = []
                    for param in out_params:
                        base_type = param.type_info.base_type
                        if CppWrapperTypeMapper.is_interface_type(base_type):
                            wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                            wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                            ret_values.append(f"{wrapper_type}::Attach({param.name}_raw)")
                        else:
                            ret_values.append(f"{param.name}_value")
                    lines.append(f"{body_indent}return {{{', '.join(ret_values)}}};")
            elif ret_type != "void" and method.return_type.base_type != 'DasResult':
                lines.append(f"{body_indent}return result;")

            lines.append("}")
            lines.append("")
            return "\n".join(lines)

        else:
            lines.append(f"{self.indent}/// @brief 调用 {interface.name}::{method.name}")
            if out_params:
                lines.append(f"{self.indent}/// @return {'返回输出值' if len(out_params) == 1 else '返回多个输出值的 tuple'}")
            lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
            lines.append(f"{self.indent}{ret_type} {method.name}({param_str})")
            lines.append(f"{self.indent}{{")

            body_indent = self.indent * 2

            for param in out_params:
                base_type = param.type_info.base_type
                if CppWrapperTypeMapper.is_interface_type(base_type):
                    qualified_base_type = self._get_qualified_type_name(base_type, current_namespace)
                    lines.append(f"{body_indent}{qualified_base_type}* {param.name}_raw = nullptr;")
                else:
                    if has_binary_buffer and base_type == 'unsigned char' and param.type_info.pointer_level == 2:
                        cpp_type = 'unsigned char*'
                        lines.append(f"{body_indent}{cpp_type} {param.name}_value{{}};")
                    else:
                        cpp_type = CppWrapperTypeMapper.TYPE_MAP.get(base_type, base_type)
                        lines.append(f"{body_indent}{cpp_type} {param.name}_value{{}};")

            call_args = []
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    base_type = param.type_info.base_type
                    if CppWrapperTypeMapper.is_interface_type(base_type):
                        call_args.append(f"&{param.name}_raw")
                    else:
                        call_args.append(f"&{param.name}_value")
                else:
                    if CppWrapperTypeMapper.is_interface_type(param.type_info.base_type):
                        call_args.append(f"{param.name}.Get()")
                    elif CppWrapperTypeMapper.is_string_type(param.type_info.base_type):
                        call_args.append(f"{param.name}.Get()")
                    else:
                        call_args.append(param.name)

            call_args_str = ", ".join(call_args)

            if method.return_type.base_type == 'DasResult' or method.return_type.base_type == 'bool':
                lines.append(f"{body_indent}const DasResult result = ptr_->{method.name}({call_args_str});")
                lines.append(f"{body_indent}DAS_THROW_IF_FAILED(result);")
            else:
                lines.append(f"{body_indent}auto result = ptr_->{method.name}({call_args_str});")

            if out_params:
                if len(out_params) == 1:
                    param = out_params[0]
                    base_type = param.type_info.base_type
                    if CppWrapperTypeMapper.is_interface_type(base_type):
                        wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                        wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                        lines.append(f"{body_indent}return {wrapper_type}::Attach({param.name}_raw);")
                    else:
                        lines.append(f"{body_indent}return {param.name}_value;")
                else:
                    ret_values = []
                    for param in out_params:
                        base_type = param.type_info.base_type
                        if CppWrapperTypeMapper.is_interface_type(base_type):
                            wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                            wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                            ret_values.append(f"{wrapper_type}::Attach({param.name}_raw)")
                        else:
                            ret_values.append(f"{param.name}_value")
                    lines.append(f"{body_indent}return {{{', '.join(ret_values)}}};")
            elif ret_type != "void" and method.return_type.base_type != 'DasResult':
                lines.append(f"{body_indent}return result;")

            lines.append(f"{self.indent}}}")
            lines.append("")

            return "\n".join(lines)

    def _generate_property_wrapper(self, interface: InterfaceDef, prop: PropertyDef, mode: str = 'inline'):
        """生成属性包装

        Args:
            interface: 接口定义
            prop: 属性定义
            mode: 生成模式，'inline'（默认，向后兼容）、'declaration'（只生成声明）、'implementation'（只生成类外实现）

        Returns:
            'inline' 模式返回字符串，其他模式返回元组 (declaration, [implementations])
        """
        wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
        current_namespace = interface.namespace

        base_type = prop.type_info.base_type
        is_interface = CppWrapperTypeMapper.is_interface_type(base_type)
        is_string = CppWrapperTypeMapper.is_string_type(base_type)
        cpp_type = CppWrapperTypeMapper.TYPE_MAP.get(base_type, base_type)

        if mode == 'inline':
            lines = []

            if prop.has_getter:
                if is_interface:
                    ret_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                    ret_type = self._get_qualified_type_name(ret_type, current_namespace)
                    lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}{ret_type} {prop.name}() const")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}{base_type}* p_out = nullptr;")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Get{prop.name}(&p_out);")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}{self.indent}return {ret_type}::Attach(p_out);")
                    lines.append(f"{self.indent}}}")
                elif is_string:
                    lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}DasReadOnlyString {prop.name}() const")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}IDasReadOnlyString* p_out = nullptr;")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Get{prop.name}(&p_out);")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}{self.indent}return DasReadOnlyString(p_out);")
                    lines.append(f"{self.indent}}}")
                else:
                    lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}{cpp_type} {prop.name}() const")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}{cpp_type} value{{}};")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Get{prop.name}(&value);")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}{self.indent}return value;")
                    lines.append(f"{self.indent}}}")
                lines.append("")

            if prop.has_setter:
                if is_interface:
                    wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                    wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                    lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}void {prop.name}(const {wrapper_type}& value)")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Set{prop.name}(value.Get());")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}}}")
                elif is_string:
                    lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}void {prop.name}(const DasReadOnlyString& value)")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Set{prop.name}(value.Get());")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}}}")
                else:
                    lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    lines.append(f"{self.indent}void {prop.name}({cpp_type} value)")
                    lines.append(f"{self.indent}{{")
                    lines.append(f"{self.indent}{self.indent}const DasResult result = ptr_->Set{prop.name}(value);")
                    lines.append(f"{self.indent}{self.indent}DAS_THROW_IF_FAILED(result);")
                    lines.append(f"{self.indent}}}")
                lines.append("")

            return "\n".join(lines)

        else:
            declaration_lines = []
            implementation_lines = []

            if prop.has_getter:
                if is_interface:
                    ret_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                    ret_type = self._get_qualified_type_name(ret_type, current_namespace)
                    declaration_lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}{ret_type} {prop.name}() const;")

                    implementation_lines.append(f"{ret_type} {wrapper_name}::{prop.name}() const")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}{base_type}* p_out = nullptr;")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Get{prop.name}(&p_out);")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append(f"{self.indent}return {ret_type}::Attach(p_out);")
                    implementation_lines.append("}")
                    implementation_lines.append("")
                elif is_string:
                    declaration_lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}DasReadOnlyString {prop.name}() const;")

                    implementation_lines.append(f"DasReadOnlyString {wrapper_name}::{prop.name}() const")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}IDasReadOnlyString* p_out = nullptr;")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Get{prop.name}(&p_out);")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append(f"{self.indent}return DasReadOnlyString(p_out);")
                    implementation_lines.append("}")
                    implementation_lines.append("")
                else:
                    declaration_lines.append(f"{self.indent}/// @brief 获取 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}{cpp_type} {prop.name}() const;")

                    implementation_lines.append(f"{cpp_type} {wrapper_name}::{prop.name}() const")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}{cpp_type} value{{}};")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Get{prop.name}(&value);")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append(f"{self.indent}return value;")
                    implementation_lines.append("}")
                    implementation_lines.append("")

            if prop.has_setter:
                if is_interface:
                    wrapper_type = CppWrapperTypeMapper.get_wrapper_class_name(base_type)
                    wrapper_type = self._get_qualified_type_name(wrapper_type, current_namespace)
                    declaration_lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}void {prop.name}(const {wrapper_type}& value);")

                    implementation_lines.append(f"void {wrapper_name}::{prop.name}(const {wrapper_type}& value)")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Set{prop.name}(value.Get());")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append("}")
                    implementation_lines.append("")
                elif is_string:
                    declaration_lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}void {prop.name}(const DasReadOnlyString& value);")

                    implementation_lines.append(f"void {wrapper_name}::{prop.name}(const DasReadOnlyString& value)")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Set{prop.name}(value.Get());")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append("}")
                    implementation_lines.append("")
                else:
                    declaration_lines.append(f"{self.indent}/// @brief 设置 {prop.name} 属性")
                    declaration_lines.append(f"{self.indent}/// @throws DasException 当操作失败时")
                    declaration_lines.append(f"{self.indent}void {prop.name}({cpp_type} value);")

                    implementation_lines.append(f"void {wrapper_name}::{prop.name}({cpp_type} value)")
                    implementation_lines.append("{")
                    implementation_lines.append(f"{self.indent}const DasResult result = ptr_->Set{prop.name}(value);")
                    implementation_lines.append(f"{self.indent}DAS_THROW_IF_FAILED(result);")
                    implementation_lines.append("}")
                    implementation_lines.append("")

            declaration = "\n".join(declaration_lines) + "\n"
            return (declaration, implementation_lines)

    def generate_wrapper_header(self, base_name: str) -> str:
        """生成 C++ 包装头文件"""
        guard_name = f"DAS_{base_name.upper()}_WRAPPER_HPP"

        interface_names = [iface.name for iface in self.document.interfaces]

        for interface in self.document.interfaces:
            self._collect_dependent_interfaces(interface)

        content = self._file_header(guard_name, interface_names)

        def _generate_dependent_wrapper_includes(current_namespace: Optional[str]) -> str:
            """基于依赖的接口类型，生成对应wrapper hpp的include。

            关键约束：一个IDL文件对应一个wrapper hpp文件，
            所以必须使用IDL文件名而不是接口类型名来生成include路径。
            例如：IDasInputFactory定义在IDasInput.idl中，
            所以应该include "Das.PluginInterface.IDasInput.hpp"
            而不是 "Das.PluginInterface.IDasInputFactory.hpp"（这个文件不存在）。

            Args:
                current_namespace: 当前生成块所在的命名空间

            Returns:
                include 语句块（末尾包含一个空行），若无依赖则返回空字符串。
            """
            if not current_namespace:
                return ""

            type_namespace_map = self._collect_type_namespace_mapping()
            type_to_idl_map = self._collect_type_to_idl_file_mapping()

            includes: Set[str] = set()
            for type_name in self._dependent_interfaces:
                type_ns = type_namespace_map.get(type_name)

                if not type_ns:
                    continue

                # 跳过非接口类型（如enum）
                simple_name = type_name.split('::')[-1]
                if not simple_name.startswith('I'):
                    continue

                # 获取依赖接口所在的IDL文件名
                idl_file_name = type_to_idl_map.get(type_name, type_name)

                # 检查是否需要include：
                # 1. 如果依赖接口在不同命名空间，必须include
                # 2. 如果依赖接口在同一命名空间但在不同IDL文件，也必须include
                current_idl_basename = self.idl_file_name.rsplit('.', 1)[0] if self.idl_file_name else None
                if type_ns != current_namespace or (type_ns == current_namespace and idl_file_name != current_idl_basename):
                    ns_path = self._to_namespace_path(type_ns)
                    includes.add(f"{ns_path}.{idl_file_name}.hpp")

            if not includes:
                return ""

            include_lines = [f'#include "{inc}"' for inc in sorted(includes)]
            return "\n".join(include_lines) + "\n\n"

        def _indent_and_inline_impl(impl: str, wrapper_name: str, indent_prefix: str) -> str:
            """将类外实现追加到头文件：整体缩进 + 为定义签名添加 inline。

            注意：实现被放进 .hpp 后必须为 inline，否则会产生 ODR 问题。
            """

            out_lines = []
            for line in impl.splitlines():
                if line == "":
                    out_lines.append("")
                    continue

                stripped = line.lstrip()
                leading_ws_len = len(line) - len(stripped)

                # 判断"函数定义签名行"：包含 Wrapper:: 且包含 '('。
                # implementation 生成器中，函数定义签名行不以 inline 开头。
                # 支持两种格式：
                # 1. DasImage::GetSize()
                # 2. DasSize DasImage::GetSize()
                is_def_sig = (
                    ("::" in stripped and f"{wrapper_name}::" in stripped)
                    and "(" in stripped
                    and not stripped.startswith("//")
                    and not stripped.startswith("#")
                    and not stripped.startswith("inline ")
                    and not stripped.startswith("return ")
                )

                if is_def_sig:
                    # 添加 inline 关键字到函数定义前
                    # 支持两种格式：
                    # 1. DasImage::GetSize() -> inline DasImage::GetSize()
                    # 2. DasSize DasImage::GetSize() -> inline DasSize DasImage::GetSize()
                    out_lines.append(
                        f"{indent_prefix}{' ' * leading_ws_len}inline {stripped}"
                    )
                else:
                    out_lines.append(f"{indent_prefix}{line}")

            return "\n".join(out_lines) + "\n"

        # 按命名空间分组
        namespace_groups = {}
        no_namespace_enums = []
        no_namespace_interfaces = []

        for enum in self.document.enums:
            if enum.namespace:
                if enum.namespace not in namespace_groups:
                    namespace_groups[enum.namespace] = {'enums': [], 'interfaces': []}
                namespace_groups[enum.namespace]['enums'].append(enum)
            else:
                no_namespace_enums.append(enum)

        for interface in self.document.interfaces:
            if interface.namespace:
                if interface.namespace not in namespace_groups:
                    namespace_groups[interface.namespace] = {'enums': [], 'interfaces': []}
                namespace_groups[interface.namespace]['interfaces'].append(interface)
            else:
                no_namespace_interfaces.append(interface)

        # 生成无命名空间的枚举 ToString 方法
        for enum in no_namespace_enums:
            content += self._generate_enum_to_string(enum)
            content += "\n"

        # 前向声明所有无命名空间的包装类
        if no_namespace_interfaces:
            content += "// Forward declarations\n"
            for interface in no_namespace_interfaces:
                wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
                content += f"class {wrapper_name};\n"
            content += "\n"

            # 生成所有无命名空间的包装类
            for interface in no_namespace_interfaces:
                # 无命名空间的类缩进深度为 0，但内容需要 1 层缩进（4 空格）
                content += self._generate_wrapper_class_declaration(interface, namespace_depth=0)
                wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
                impl_content = self._generate_wrapper_class_implementation(interface)
                content += _indent_and_inline_impl(impl_content, wrapper_name, indent_prefix="")
                content += "\n"

        # 生成有命名空间的代码（支持 C++17 嵌套命名空间语法）
        for namespace_name, items in sorted(namespace_groups.items()):
            content += self._generate_namespace_open(namespace_name)

            # 枚举 ToString 方法
            for enum in items['enums']:
                content += self._generate_enum_to_string(enum)
                content += "\n"

            # 前向声明包装类
            if items['interfaces']:
                # 计算正确的缩进深度（基于 namespace 嵌套层级）
                ns_depth = len(namespace_name.split("::"))
                ns_indent = "    " * ns_depth
                content += f"{ns_indent}// Forward declarations\n"
                for interface in items['interfaces']:
                    wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
                    content += f"{ns_indent}class {wrapper_name};\n"

                # 前向声明依赖接口的包装类（只处理接口类型，以'I'开头）
                type_namespace_map = self._collect_type_namespace_mapping()
                for dep_interface_name in sorted(self._dependent_interfaces):
                    dep_ns = type_namespace_map.get(dep_interface_name)
                    if dep_ns == namespace_name and dep_interface_name.startswith('I'):
                        dep_wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(dep_interface_name)
                        content += f"{ns_indent}class {dep_wrapper_name};\n"
                content += "\n"

            # 生成包装类
            ns_depth = len(namespace_name.split("::"))
            for interface in items['interfaces']:
                content += self._generate_wrapper_class_declaration(interface, namespace_depth=ns_depth)
                content += "\n"

            # 在类声明之后、类外实现之前 include 依赖 wrapper 文件。
            # 注意：#include 不能放在 namespace 作用域内，否则会把被 include 文件“嵌套进当前 namespace”。
            # 所以这里采用：关闭命名空间 -> include -> 重新打开命名空间 的方式。
            dependent_includes = _generate_dependent_wrapper_includes(namespace_name)
            if dependent_includes:
                content += self._generate_namespace_close(namespace_name)
                content += dependent_includes
                content += self._generate_namespace_open(namespace_name)

            # 生成类外实现，并追加到同一个命名空间内（header-only, all inline）
            ns_indent = "    " * ns_depth
            for interface in items['interfaces']:
                wrapper_name = CppWrapperTypeMapper.get_wrapper_class_name(interface.name)
                impl_content = self._generate_wrapper_class_implementation(interface)
                content += _indent_and_inline_impl(impl_content, wrapper_name, indent_prefix=ns_indent)
                content += "\n"

            content += self._generate_namespace_close(namespace_name)

        content += self._file_footer(guard_name)
        return content

    def generate_wrapper_implementation(self, base_name: str) -> str:
        """生成 C++ 包装实现文件（.cpp）"""
        content = f"// Wrapper implementation for {base_name}\n\n"

        # 包含对应的包装头文件
        content += f"#include <das/_autogen/idl/wrapper/{base_name}.hpp>\n\n"

        # 生成所有接口的实现（不使用命名空间包装）
        for interface in self.document.interfaces:
            impl_content = self._generate_wrapper_class_implementation(interface)
            content += impl_content
            content += "\n\n"

        return content


def generate_cpp_wrapper_files(document: IdlDocument, output_dir: str, base_name: str,
                                 namespace: str = "Das", idl_file_path: Optional[str] = None) -> List[str]:
    """生成 C++ 包装文件

    生成单个文件，文件名格式为: Namespace.FileName.hpp
    例如: DAS.ExportInterface.IDasGuidVector.hpp

    所有文件都扁平化存储在 output_dir 中，不使用子目录
    例如: idl/ExportInterface/IDasPluginManager.idl -> output_dir/DAS.ExportInterface.IDasPluginManager.hpp
    """
    import os

    generator = CppWrapperGenerator(document, namespace, idl_file_path)

    # 扁平化存储：不创建子目录
    actual_output_dir = output_dir

    # 确保输出目录存在
    os.makedirs(actual_output_dir, exist_ok=True)

    generated_files = []

    # 找出第一个有命名空间的接口或枚举，用于文件名
    namespace_prefix = None
    for enum in document.enums:
        if enum.namespace:
            namespace_prefix = enum.namespace
            break

    if namespace_prefix is None:
        for interface in document.interfaces:
            if interface.namespace:
                namespace_prefix = interface.namespace
                break

    # 将命名空间转换为文件路径格式 (DAS::ExportInterface -> DAS.ExportInterface)
    namespace_path = generator._to_namespace_path(namespace_prefix) if namespace_prefix else ""

    # 构建文件名
    if namespace_path:
        header_filename = f"{namespace_path}.{base_name}.hpp"
        impl_filename = f"{namespace_path}.{base_name}.cpp"
    else:
        header_filename = f"{base_name}.hpp"
        impl_filename = f"{base_name}.cpp"

    # 生成头文件（包含声明和内联实现）
    header_filepath = os.path.join(actual_output_dir, header_filename)
    header_content = generator.generate_wrapper_header(base_name)

    if header_content:
        with open(header_filepath, 'w', encoding='utf-8') as f:
            f.write(header_content)
        print(f"Generated: {header_filepath}")
        generated_files.append(header_filepath)

    # 注意：不再生成 .cpp 实现文件，所有实现都在头文件中以 inline 方式提供

    return generated_files


# 测试代码
if __name__ == '__main__':
    parse_idl = _das_idl_parser.parse_idl

    test_idl = '''
    [uuid("d5bd3213-b7c4-1b94-0e99-5cefFF064f8d")]
    interface IDasGuidVector : IDasBase {
        DasResult Size([out] size_t* p_out_size);
        DasResult At(size_t index, [out] DasGuid* p_out_iid);
        DasResult Find(const DasGuid& iid);
        DasResult PushBack(const DasGuid& iid);
    }

    [uuid("a1234567-89ab-cdef-0123-456789abcdef")]
    interface IDasTestResult : IDasBase {
        [get, set] int32 Id
        [get] DasReadOnlyString Name
        [get, set] DasGuid Guid
        [get] IDasGuidVector Items
    }
    '''

    doc = parse_idl(test_idl)
    generator = CppWrapperGenerator(doc)

    print("=== 生成的 C++ 包装头文件 ===")
    print(generator.generate_wrapper_header("TestInterfaces"))

    print("=== 生成的 C++ 包装头文件 ===")
    print(generator.generate_wrapper_header("TestInterfaces"))


    doc = parse_idl(test_idl)
    generator = CppWrapperGenerator(doc)

    print("=== 生成的 C++ 包装头文件 ===")
    print(generator.generate_wrapper_header("TestInterfaces"))

    print("=== 生成的 C++ 包装头文件 ===")
    print(generator.generate_wrapper_header("TestInterfaces"))
