"""
DAS C++ Implements 模板生成器
生成类似 WinRT winrt::implements 的实现基类模板

功能:
1. 自动生成 AddRef/Release 引用计数实现
2. 自动生成 QueryInterface 实现（基于 IDL 中的继承链信息）
3. 提供 Make<T>() 工厂方法，返回 DasPtr<Interface>
4. 支持 CRTP 模式，用户只需继承模板并实现业务方法

生成文件命名: Das.Xxx.Implements.hpp
"""

import os
from pathlib import Path
from datetime import datetime, timezone
from typing import List, Set, Dict, Optional
from das_idl_parser import (
    IdlDocument, InterfaceDef, EnumDef, MethodDef, PropertyDef,
    ParameterDef, TypeInfo, ParamDirection
)


class CppImplementsTypeMapper:
    """C++ Implements 类型映射器"""

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
        'void': 'void',
    }

    @staticmethod
    def is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型 (以 I 开头的类型)"""
        return type_name.startswith('I') and len(type_name) > 1 and type_name[1:2].isupper()

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
    def get_impl_base_name(cls, interface_name: str) -> str:
        """获取实现基类名称: IDasXxx -> DasXxxImplBase"""
        if interface_name.startswith('IDas'):
            return interface_name[1:] + "ImplBase"  # IDasXxx -> DasXxxImplBase
        elif interface_name.startswith('I'):
            return interface_name[1:] + "ImplBase"  # IXxx -> XxxImplBase
        return interface_name + "ImplBase"


class CppImplementsGenerator:
    """C++ Implements 模板生成器 - 生成类似 WinRT winrt::implements 风格的代码"""

    # 已知的接口继承关系（从 IDasBase 出发）
    # 这将从 IDL 文件中自动解析
    KNOWN_INHERITANCE = {
        'IDasBase': [],
        'IDasSwigBase': [],
        'IDasTypeInfo': ['IDasBase'],
        'IDasSwigTypeInfo': ['IDasSwigBase'],
    }

    def __init__(self, document: IdlDocument, namespace: str = "Das", idl_file_name: str = None):
        self.document = document
        self.namespace = namespace
        self.indent = "    "  # 4 空格缩进
        self.idl_file_name = idl_file_name  # IDL文件名
        # 构建接口继承图
        self._inheritance_map: Dict[str, str] = {}  # interface -> base_interface
        self._build_inheritance_map()

    def _build_inheritance_map(self):
        """从文档中构建接口继承图"""
        for interface in self.document.interfaces:
            self._inheritance_map[interface.name] = interface.base_interface

    def _get_full_inheritance_chain(self, interface_name: str) -> List[str]:
        """获取接口的完整继承链（从最顶层到当前接口）

        例如: IDasTouch -> [IDasBase, IDasTypeInfo, IDasInput, IDasTouch]
        """
        chain = []
        current = interface_name

        while current:
            chain.insert(0, current)
            # 先检查文档中的接口
            if current in self._inheritance_map:
                current = self._inheritance_map[current]
            # 再检查已知的内置继承关系
            elif current in self.KNOWN_INHERITANCE:
                bases = self.KNOWN_INHERITANCE[current]
                current = bases[0] if bases else None
            else:
                break

        return chain

    def _to_namespace_path(self, namespace: str) -> str:
        """将命名空间转换为文件路径格式，例如: DAS::ExportInterface -> DAS.ExportInterface"""
        if not namespace:
            return ""
        return namespace.replace("::", ".")

    def _get_import_includes(self, all_defined_interfaces: set = None, idl_main_header: str = None) -> list:
        """根据 imports 生成 include 路径列表

        import语句使用相对路径，但生成时只使用文件名。
        例如: import "../DasTypes.idl"; -> #include "DasTypes.h"
              import "./IDasImage.idl"; -> #include "IDasImage.h"
              import "../ExportInterface/IDasCapture.idl"; -> #include "IDasCapture.h"

        每个 implements 类型文件都会包含自己的 IDL 文件生成的主头文件（.h 文件）。

        Args:
            all_defined_interfaces: 整个IDL文件中定义的所有接口名称集合（不传则使用self.document中的接口）
            idl_main_header: 当前IDL文件生成的主头文件名（例如 "IDasPluginManager.h"）
        """
        includes = []

        # 首先收集文档中所有定义的接口名
        if all_defined_interfaces is not None:
            defined_interfaces = all_defined_interfaces
        else:
            defined_interfaces = set(iface.name for iface in self.document.interfaces)

        # 从import语句生成include列表
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
            # 生成对应的.h文件名
            h_path = import_name + '.h'
            includes.append(h_path)

        # 如果提供了IDL主头文件名，则添加它（每个 implements 类型文件都要包含自己的 .h 文件）
        if idl_main_header:
            includes.append(idl_main_header)

        return includes

    def _file_header(self, guard_name: str, interface_names: List[str], all_defined_interfaces: set = None, idl_main_header: str = None) -> str:
        """生成文件头"""
        timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

        # 从import语句生成include列表
        import_includes = self._get_import_includes(all_defined_interfaces, idl_main_header)

        # 为每个导入生成 #include 语句
        import_includes_str = ""
        if import_includes:
            import_includes_str = "\n".join(f'#include "{inc}"' for inc in sorted(set(import_includes)))
            import_includes_str = f"\n{import_includes_str}\n"

        # 生成IDL文件名注释
        idl_file_comment = ""
        if self.idl_file_name:
            idl_file_comment = f"// Source IDL file: {self.idl_file_name}\n"

        return f"""// This file is automatically generated by DAS IDL Generator
// Generated at: {timestamp}
{idl_file_comment}// !!! DO NOT EDIT !!!
//
// Implements base templates for DAS interfaces
// Similar to WinRT winrt::implements<T, I...>
// Provides automatic reference counting, QueryInterface implementation,
// and Make<T>() factory method.

#ifndef {guard_name}
#define {guard_name}

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <atomic>
#include <type_traits>
#include <utility>
{import_includes_str}
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
        """生成命名空间开始标记，支持 C++17 嵌套命名空间语法"""
        if namespace:
            return f"namespace {namespace} {{\n"
        return ""

    def _generate_namespace_close(self, namespace: str) -> str:
        """生成命名空间结束标记"""
        if namespace:
            return f"}} // namespace {namespace}\n"
        return ""

    def _generate_impl_base_class(self, interface: InterfaceDef) -> str:
        """生成实现基类模板"""
        raw_name = interface.name
        impl_base_name = CppImplementsTypeMapper.get_impl_base_name(raw_name)

        # 获取完整继承链
        inheritance_chain = self._get_full_inheritance_chain(raw_name)

        lines = []

        # 类注释
        lines.append(f"/**")
        lines.append(f" * @brief Implementation base template for {raw_name}")
        lines.append(f" * ")
        lines.append(f" * Inherit from this class and implement the pure virtual methods.")
        lines.append(f" * Reference counting and QueryInterface are automatically handled.")
        lines.append(f" * ")
        lines.append(f" * Inheritance chain: {' -> '.join(inheritance_chain)}")
        lines.append(f" * ")
        lines.append(f" * Usage:")
        lines.append(f" *   class MyImpl final : public {impl_base_name}<MyImpl>")
        lines.append(f" *   {{")
        lines.append(f" *       // Implement {raw_name} methods here...")
        lines.append(f" *   }};")
        lines.append(f" *   ")
        lines.append(f" *   // Create instance:")
        lines.append(f" *   DasPtr<{raw_name}> ptr = MyImpl::Make();")
        lines.append(f" */")

        # 类定义 - 使用 CRTP 模式
        lines.append(f"template <typename TImpl>")
        lines.append(f"class {impl_base_name} : public {raw_name}")
        lines.append("{")

        # private 部分
        lines.append("private:")
        lines.append(f"{self.indent}std::atomic<uint32_t> ref_count_{{0}};")
        lines.append("")

        # protected 部分（给子类访问）
        lines.append("protected:")
        lines.append(f"{self.indent}/// @brief Protected destructor - prevent direct deletion, use Release() instead")
        lines.append(f"{self.indent}virtual ~{impl_base_name}() = default;")
        lines.append("")

        # public 部分
        lines.append("public:")
        lines.append(f"{self.indent}/// @brief Default constructor")
        lines.append(f"{self.indent}{impl_base_name}() = default;")
        lines.append("")

        # AddRef 实现 - 返回 uint32_t
        lines.append(f"{self.indent}/// @brief Increment reference count")
        lines.append(f"{self.indent}/// @return New reference count")
        lines.append(f"{self.indent}uint32_t DAS_STD_CALL AddRef() override")
        lines.append(f"{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}return ++ref_count_;")
        lines.append(f"{self.indent}}}")
        lines.append("")

        # Release 实现 - 返回 uint32_t
        lines.append(f"{self.indent}/// @brief Decrement reference count, delete when zero")
        lines.append(f"{self.indent}/// @return New reference count")
        lines.append(f"{self.indent}uint32_t DAS_STD_CALL Release() override")
        lines.append(f"{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}const auto count = --ref_count_;")
        lines.append(f"{self.indent}{self.indent}if (count == 0)")
        lines.append(f"{self.indent}{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}{self.indent}// Prevent double deletion if AddRef/Release called during destructor")
        lines.append(f"{self.indent}{self.indent}{self.indent}ref_count_ = 1;")
        lines.append(f"{self.indent}{self.indent}{self.indent}delete static_cast<TImpl*>(this);")
        lines.append(f"{self.indent}{self.indent}{self.indent}return 0;")
        lines.append(f"{self.indent}{self.indent}}}")
        lines.append(f"{self.indent}{self.indent}return count;")
        lines.append(f"{self.indent}}}")
        lines.append("")

        # QueryInterface 实现
        lines.append(f"{self.indent}/// @brief Query for interface support")
        lines.append(f"{self.indent}/// @param iid Interface GUID to query")
        lines.append(f"{self.indent}/// @param pp_out_object Output pointer for the interface")
        lines.append(f"{self.indent}/// @return DAS_S_OK if interface is supported, DAS_E_NO_INTERFACE otherwise")
        lines.append(f"{self.indent}DasResult DAS_STD_CALL QueryInterface(const DasGuid& iid, void** pp_out_object) override")
        lines.append(f"{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}if (pp_out_object == nullptr)")
        lines.append(f"{self.indent}{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}{self.indent}return DAS_E_INVALID_POINTER;")
        lines.append(f"{self.indent}{self.indent}}}")
        lines.append("")

        # 按继承链顺序检查每个接口
        for i, iface_name in enumerate(inheritance_chain):
            if i == 0:
                lines.append(f"{self.indent}{self.indent}if (iid == DasIidOf<{iface_name}>())")
            else:
                lines.append(f"{self.indent}{self.indent}else if (iid == DasIidOf<{iface_name}>())")
            lines.append(f"{self.indent}{self.indent}{{")
            lines.append(f"{self.indent}{self.indent}{self.indent}*pp_out_object = static_cast<{iface_name}*>(static_cast<TImpl*>(this));")
            lines.append(f"{self.indent}{self.indent}{self.indent}AddRef();")
            lines.append(f"{self.indent}{self.indent}{self.indent}return DAS_S_OK;")
            lines.append(f"{self.indent}{self.indent}}}")

        # 默认情况
        lines.append("")
        lines.append(f"{self.indent}{self.indent}*pp_out_object = nullptr;")
        lines.append(f"{self.indent}{self.indent}return DAS_E_NO_INTERFACE;")
        lines.append(f"{self.indent}}}")
        lines.append("")

        # Make 工厂方法
        lines.append(f"{self.indent}/// @brief Factory method to create instance wrapped in DasPtr")
        lines.append(f"{self.indent}/// @tparam Args Constructor argument types")
        lines.append(f"{self.indent}/// @param args Constructor arguments")
        lines.append(f"{self.indent}/// @return DasPtr<{raw_name}> holding the new instance")
        lines.append(f"{self.indent}template <typename... Args>")
        lines.append(f"{self.indent}static DasPtr<{raw_name}> Make(Args&&... args)")
        lines.append(f"{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}auto* p = new TImpl(std::forward<Args>(args)...);")
        lines.append(f"{self.indent}{self.indent}p->AddRef();")
        lines.append(f"{self.indent}{self.indent}return DasPtr<{raw_name}>::Attach(p);")
        lines.append(f"{self.indent}}}")
        lines.append("")

        # MakeAndAttach 方法 - 直接返回原始指针（已 AddRef）
        lines.append(f"{self.indent}/// @brief Factory method to create instance and return raw pointer (already AddRef'd)")
        lines.append(f"{self.indent}/// @tparam Args Constructor argument types")
        lines.append(f"{self.indent}/// @param args Constructor arguments")
        lines.append(f"{self.indent}/// @return Raw pointer to the new instance (caller owns reference)")
        lines.append(f"{self.indent}template <typename... Args>")
        lines.append(f"{self.indent}static TImpl* MakeRaw(Args&&... args)")
        lines.append(f"{self.indent}{{")
        lines.append(f"{self.indent}{self.indent}auto* p = new TImpl(std::forward<Args>(args)...);")
        lines.append(f"{self.indent}{self.indent}p->AddRef();")
        lines.append(f"{self.indent}{self.indent}return p;")
        lines.append(f"{self.indent}}}")

        lines.append("};")

        return "\n".join(lines) + "\n"

    def generate_implements_header(self, base_name: str, all_defined_interfaces: set = None, idl_main_header: str = None) -> str:
        """生成 C++ Implements 头文件

        Args:
            base_name: 基础名称
            all_defined_interfaces: 整个IDL文件中定义的所有接口名称集合
            idl_main_header: 当前IDL文件生成的主头文件名（例如 "IDasPluginManager.h"）
        """
        guard_name = f"DAS_{base_name.upper()}_IMPLEMENTS_HPP"

        interface_names = [iface.name for iface in self.document.interfaces]

        content = self._file_header(guard_name, interface_names, all_defined_interfaces, idl_main_header)

        # 按命名空间分组
        namespace_groups: Dict[str, List[InterfaceDef]] = {}
        no_namespace_interfaces: List[InterfaceDef] = []

        for interface in self.document.interfaces:
            if interface.namespace:
                if interface.namespace not in namespace_groups:
                    namespace_groups[interface.namespace] = []
                namespace_groups[interface.namespace].append(interface)
            else:
                no_namespace_interfaces.append(interface)

        # 生成无命名空间的实现基类
        if no_namespace_interfaces:
            for interface in no_namespace_interfaces:
                content += self._generate_impl_base_class(interface)
                content += "\n"

        # 生成有命名空间的代码
        for namespace_name, interfaces in sorted(namespace_groups.items()):
            content += self._generate_namespace_open(namespace_name)

            for interface in interfaces:
                content += self._generate_impl_base_class(interface)
                content += "\n"

            content += self._generate_namespace_close(namespace_name)

        content += self._file_footer(guard_name)
        return content


def generate_cpp_implements_files(document: IdlDocument, output_dir: str, base_name: str,
                                    namespace: str = "Das", idl_file_path: str = None) -> List[str]:
    """生成 C++ Implements 文件

    为每个接口生成独立的文件，文件名格式为: Namespace.InterfaceName.Implements.hpp
    例如: DAS.PluginInterface.IDasStopToken.Implements.hpp

    所有文件都直接生成在 output_dir 目录下，不创建子目录。

    Args:
        document: IDL文档对象
        output_dir: 输出目录
        base_name: 基础名称
        namespace: 命名空间
        idl_file_path: IDL文件路径（可选，用于确定主头文件名）

    Returns:
        生成的文件列表
    """
    import os
    from pathlib import Path

    generated_files = []

    # 收集整个文档中定义的所有接口名称
    all_defined_interfaces = set(iface.name for iface in document.interfaces)

    # 从IDL文件路径中提取主头文件名
    idl_main_header = None
    if idl_file_path:
        idl_filename = os.path.basename(idl_file_path)
        # 将.idl替换为.h
        if idl_filename.endswith('.idl'):
            idl_main_header = idl_filename[:-4] + '.h'
        else:
            idl_main_header = idl_filename + '.h'

    # 为每个接口生成独立的Implements文件
    for interface in document.interfaces:
        # 创建只包含单个接口的document
        from copy import deepcopy
        single_interface_doc = deepcopy(document)
        single_interface_doc.interfaces = [interface]

        # 从IDL文件路径中提取文件名
        idl_file_name = None
        if idl_file_path:
            idl_file_name = os.path.basename(idl_file_path)

        generator = CppImplementsGenerator(single_interface_doc, namespace, idl_file_name)

        # 所有文件都直接生成在 output_dir 目录下
        actual_output_dir = output_dir

        # 确保输出目录存在
        os.makedirs(actual_output_dir, exist_ok=True)

        # 将命名空间转换为文件路径格式 (DAS::ExportInterface -> DAS.ExportInterface)
        namespace_path = generator._to_namespace_path(interface.namespace) if interface.namespace else ""

        # 构建文件名 - 使用接口名而不是IDL文件名
        if namespace_path:
            filename = f"{namespace_path}.{interface.name}.Implements.hpp"
        else:
            filename = f"{interface.name}.Implements.hpp"

        filepath = os.path.join(actual_output_dir, filename)

        # 生成内容 - 传递整个文档中定义的所有接口名称和IDL主头文件名
        content = generator.generate_implements_header(interface.name, all_defined_interfaces, idl_main_header)

        if content:  # 只有内容非空时才写入文件
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)

            print(f"Generated: {filepath}")
            generated_files.append(filepath)

    return generated_files


# 测试代码
if __name__ == '__main__':
    from das_idl_parser import parse_idl

    test_idl = '''
    namespace Das::PluginInterface {

    [uuid("02F6A16A-01FD-4303-886A-9B60373EBE8C")]
    interface IDasInput : IDasTypeInfo {
        DasResult Click(int32_t x, int32_t y);
    }

    [uuid("DDB17BB3-E6B2-4FD8-8E06-C037EEF18D65")]
    interface IDasTouch : IDasInput {
        DasResult Swipe(DasPoint from, DasPoint to, int32_t duration_ms);
    }

    }
    '''

    doc = parse_idl(test_idl)
    generator = CppImplementsGenerator(doc)

    print("=== 生成的 C++ Implements 头文件 ===")
    print(generator.generate_implements_header("IDasInput"))
