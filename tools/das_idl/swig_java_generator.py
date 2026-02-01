"""Java SWIG 生成器

生成 Java 特定的 SWIG .i 文件代码

核心设计：
1. 通过 SWIG typemap 完全隐藏 [out] 参数，让 Java 用户完全不感知 C++ 的通过参数传递返回值的模式。
2. 对于带 [out] 参数的方法，Java 端：
    - 主方法直接返回 DasRetXxx 包装类，包含错误码和结果
    - 便捷方法（Ez后缀）直接返回结果，失败时抛出 DasException
"""

from das_idl_parser import InterfaceDef, MethodDef, ParameterDef, ParamDirection
from typing import Any, Optional, Dict

from swig_lang_generator_base import SwigLangGenerator


class JavaSwigGenerator(SwigLangGenerator):
    """Java 特定的 SWIG 代码生成器"""

    # 预定义的返回类型映射表
    # 这些类型在项目中已通过 DAS_DEFINE_RET_TYPE/DAS_DEFINE_RET_POINTER 宏定义
    # 使用这些预定义类型，而不是生成新的类型
    _PREDEFINED_RET_TYPES: Dict[str, str] = {
        # 接口类型 -> 预定义的返回类型名
        'IDasReadOnlyString': 'DasRetReadOnlyString',
        # 值类型 -> 预定义的返回类型名
        'int64_t': 'DasRetInt',
        'uint64_t': 'DasRetUInt',
        'DasReadOnlyString': 'DasRetReadOnlyString',
    }
    def __init__(self) -> None:
        super().__init__()
        # 已生成 typemap 的方法集合，避免重复定义 (interface::method)
        self._generated_method_typemaps: set[str] = set()
        # 已生成的 DasRetXxx 类集合
        self._generated_ret_classes: set[str] = set()
        # 已生成 javacode typemap 的接口集合
        self._generated_javacode_interfaces: set[str] = set()
        # 所有接口列表（用于检查接口继承链）
        self._all_interfaces: list[InterfaceDef] = []
        # 所有枚举列表（用于查找枚举的 FORCE_DWORD 值）
        self._all_enums: list = []

    def set_all_interfaces(self, all_interfaces: list[InterfaceDef]) -> None:
        self._all_interfaces = all_interfaces

    def set_all_enums(self, all_enums: list) -> None:
        self._all_enums = all_enums

    def _get_enum_default_value(self, type_name: str) -> str | None:
        """获取枚举类型的 FORCE_DWORD 默认值

        Args:
            type_name: 类型名称（可能包含命名空间前缀）

        Returns:
            枚举的 FORCE_DWORD 值字符串，如果不是枚举则返回 None
        """
        from das_idl_parser import EnumDef

        # 提取类型名（去除命名空间前缀）
        if "::" in type_name:
            type_name = type_name.split("::")[-1]

        # 在所有枚举中查找
        for enum_def in self._all_enums:
            if enum_def.name == type_name:
                # 查找 FORCE_DWORD 值
                for enum_value in enum_def.values:
                    if enum_value.name.endswith("FORCE_DWORD"):
                        # 构造完全限定名
                        namespace = enum_def.namespace if enum_def.namespace else ""
                        if namespace:
                            return f"{namespace}::{type_name}::{enum_value.name}"
                        else:
                            return f"{type_name}::{enum_value.name}"

        return None

    @staticmethod
    def _get_java_type(type_name: str) -> str:
        """获取 Java 类型"""
        JAVA_TYPE_MAP = {
            'bool': 'boolean',
            'int8': 'byte',
            'int16': 'short',
            'int32': 'int',
            'int64': 'long',
            'int64_t': 'long',
            'uint8': 'short',
            'uint16': 'int',
            'uint32': 'long',
            'uint64': 'java.math.BigInteger',
            'uint64_t': 'java.math.BigInteger',
            'float': 'float',
            'double': 'double',
            'size_t': 'long',
            'int': 'int',
            'uint': 'long',
            'DasBool': 'boolean',
            'DasGuid': 'DasGuid',
            'DasString': 'String',
            'DasReadOnlyString': 'String',
            'DasResult': 'DasResult',
        }

        if type_name in JAVA_TYPE_MAP:
            return JAVA_TYPE_MAP[type_name]

        return type_name

    @staticmethod
    def _is_interface_type(type_name: str) -> bool:
        """判断是否是接口类型（以 I 开头，后跟大写字母）"""
        simple_name = type_name.split('::')[-1]
        return simple_name.startswith('I') and len(simple_name) > 1 and simple_name[1:2].isupper()

    @staticmethod
    def _is_void_pointer_pointer(type_name: str) -> bool:
        """判断是否是 void** 类型"""
        simple_name = type_name.split('::')[-1].strip()
        return simple_name == 'void'

    @staticmethod
    def _normalize_type_name_for_class(type_name: str) -> str:
        """规范化类型名用于生成类名

        例如：
        - size_t -> SizeT
        - int32_t -> Int32T
        - DasGuid -> DasGuid
        - IDasVariantVector -> DasVariantVector
        - unsigned char -> UnsignedChar
        - unsigned char* -> UnsignedCharPtr
        """
        simple_name = type_name.split('::')[-1]

        # 处理指针类型
        is_pointer = simple_name.endswith('*')
        if is_pointer:
            simple_name = simple_name.rstrip('*').strip()

        # 处理带空格的类型名（如 unsigned char, unsigned int 等）
        if ' ' in simple_name:
            parts = simple_name.split()
            result = ''.join(part.capitalize() for part in parts)
            if is_pointer:
                result += 'Ptr'
            return result

        # 处理以 I 开头的接口类型
        if simple_name.startswith('I') and len(simple_name) > 1 and simple_name[1:2].isupper():
            result = simple_name[1:]  # 去掉前缀 I
            if is_pointer:
                result += 'Ptr'
            return result

        # 处理 _t 后缀的类型
        if simple_name.endswith('_t'):
            base = simple_name[:-2]  # 去掉 _t
            # 将下划线分隔转换为 PascalCase
            parts = base.split('_')
            result = ''.join(part.capitalize() for part in parts) + 'T'
            if is_pointer:
                result += 'Ptr'
            return result

        # 处理下划线分隔的类型名
        if '_' in simple_name:
            parts = simple_name.split('_')
            result = ''.join(part.capitalize() for part in parts)
            if is_pointer:
                result += 'Ptr'
            return result

        # 其他情况：首字母大写
        if simple_name and simple_name[0].islower():
            result = simple_name[0].upper() + simple_name[1:]
        else:
            result = simple_name

        if is_pointer:
            result += 'Ptr'
        return result

    @classmethod
    def _get_ret_class_name(cls, out_type: str) -> str:
        """根据 [out] 参数类型生成返回包装类名

        首先检查预定义的返回类型映射表，如果有则使用预定义名称。
        否则生成新的类名。

        例如：
        - IDasVariantVector -> DasRetVariantVector (预定义)
        - IDasReadOnlyString -> DasRetReadOnlyString (预定义)
        - IDasGuidVector -> DasRetGuidVector (生成)
        - size_t -> DasRetSizeT (生成)
        - DasGuid -> DasRetGuid (预定义)
        """
        simple_name = out_type.split('::')[-1]

        # 优先使用预定义的返回类型
        if simple_name in cls._PREDEFINED_RET_TYPES:
            return cls._PREDEFINED_RET_TYPES[simple_name]

        # 否则生成新的类名
        normalized = cls._normalize_type_name_for_class(out_type)
        return f"DasRet{normalized}"

    @classmethod
    def _is_predefined_ret_type(cls, out_type: str) -> bool:
        """判断返回类型是否是预定义的（不需要生成类定义）"""
        simple_name = out_type.split('::')[-1]
        return simple_name in cls._PREDEFINED_RET_TYPES

    def get_language_name(self) -> str:
        return 'java'

    def get_swig_define(self) -> str:
        return 'SWIGJAVA'

    def handles_out_param_completely(self) -> bool:
        """Java 生成器完全处理 [out] 参数

        通过 %ignore + %extend 方案替换原始方法，
        让 Java 用户完全不感知 C++ 的通过参数传递返回值的模式。
        """
        return True

    @staticmethod
    def _is_binary_buffer_method(method: MethodDef) -> bool:
        """判断方法是否有 [binary_buffer] 标记"""
        return method.attributes.get('binary_buffer', False) if method.attributes else False

    @staticmethod
    def _is_binary_buffer_interface(interface: InterfaceDef) -> bool:
        """判断接口是否有任何 [binary_buffer] 标记的方法

        如果接口有任何一个方法有 [binary_buffer] 标记，则认为是 binary_buffer 接口。
        这样的接口需要特殊处理（在 generate_binary_buffer_helpers 中统一生成 javacode）。
        """
        for method in interface.methods:
            if method.attributes and method.attributes.get('binary_buffer', False):
                return True
        return False

    def _get_out_param_methods(self, interface: InterfaceDef, exclude_binary_buffer: bool = True) -> list[tuple[MethodDef, ParameterDef]]:
        """获取接口中所有带有 [out] 参数的方法

        Args:
            interface: 接口定义
            exclude_binary_buffer: 是否排除 [binary_buffer] 标记的方法

        Returns:
            方法和其第一个 [out] 参数的元组列表
        """
        result: list[tuple[MethodDef, ParameterDef]] = []
        for method in interface.methods:
            # 如果需要排除 [binary_buffer] 方法，跳过它们
            if exclude_binary_buffer and self._is_binary_buffer_method(method):
                continue
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if out_params:
                out_param = out_params[0]
                # 检查是否是 void** 类型，如果是则打印警告并跳过
                if self._is_void_pointer_pointer(out_param.type_info.base_type):
                    print(f"Warning: Skipping method {interface.name}::{method.name} - void** output parameter is not supported for Java binding")
                    continue
                # 每个方法只取第一个 [out] 参数
                result.append((method, out_param))
        return result

    def _to_include_guard(self, name: str) -> str:
        """将类名转换为 include guard 宏名

        将驼峰命名转换为全大写下划线命名
        例如: DasRetDasReadOnlyGuidVector -> DAS_RET_DAS_READ_ONLY_GUID_VECTOR

        Args:
            name: 类名

        Returns:
            include guard 宏名
        """
        # 使用正则表达式将驼峰命名转换为全大写下划线命名
        import re
        # 在每个大写字母前添加下划线，然后转换为大写，最后去除开头的下划线
        result = re.sub(r'([A-Z])', r'_\1', name).upper()
        # 去除开头的下划线
        if result.startswith('_'):
            result = result[1:]
        return result

    def _generate_ret_class(self, out_type: str) -> str:
        """生成 DasRetXxx 返回包装类

        这个类包含错误码和结果值，用于 Java 端接收带 [out] 参数方法的返回值。

        对于接口类型（如 IDasVariantVector），value 是指针类型
        对于值类型（如 size_t、DasGuid），value 是值类型

        注意：如果是预定义的返回类型，则不生成类定义（类已存在）

        Args:
            out_type: [out] 参数的类型名

        Returns:
            Java 类定义（通过 %inline 嵌入），如果是预定义类型则返回空字符串
        """
        ret_class_name = self._get_ret_class_name(out_type)

        if ret_class_name in self._generated_ret_classes:
            return ""
        self._generated_ret_classes.add(ret_class_name)

        # 如果是预定义的返回类型，不需要生成类定义
        if self._is_predefined_ret_type(out_type):
            return ""

        java_type = self._get_java_type(out_type)
        is_interface = self._is_interface_type(out_type)

        if is_interface:
            java_type = out_type
            # 接口类型：存储指针，使用完全限定名
            namespace = self.get_type_namespace(out_type)
            if namespace:
                # 检查 out_type 是否已经包含命名空间前缀，避免重复
                if out_type.startswith(f"{namespace}::"):
                    cpp_value_type = f"{out_type}*"
                else:
                    cpp_value_type = f"{namespace}::{out_type}*"
            else:
                cpp_value_type = f"{out_type}*"
            cpp_default_value = "nullptr"
        else:
            # 值类型：存储值本身
            namespace = self.get_type_namespace(out_type)
            if namespace:
                # 检查 out_type 是否已经包含命名空间前缀，避免重复
                if out_type.startswith(f"{namespace}::"):
                    cpp_value_type = out_type
                else:
                    cpp_value_type = f"{namespace}::{out_type}"
            else:
                cpp_value_type = out_type

            # 判断是否是枚举类型
            enum_default_value = self._get_enum_default_value(out_type)
            if enum_default_value:
                # 枚举类型：使用 FORCE_DWORD 值
                cpp_default_value = enum_default_value
            else:
                # 基本类型：不填默认值，使用 () 默认初始化
                cpp_default_value = ""
            include_guard = self._to_include_guard(ret_class_name)
            return f"""
// ============================================================================
// {ret_class_name} - 返回包装类（接口类型）
// 用于封装带有 [out] 参数的方法返回值
// ============================================================================
%inline %{{

#ifndef {include_guard}
#define {include_guard}
struct {ret_class_name} {{
    DasResult error_code;
    {cpp_value_type} value;

    {ret_class_name}() : error_code(DAS_E_UNDEFINED_RETURN_VALUE){f", value({cpp_default_value})" if cpp_default_value else ""} {{}}

    DasResult GetErrorCode() const {{ return error_code; }}

    {cpp_value_type} GetValue() const {{ return value; }}

    bool IsOk() const {{ return DAS::IsOk(error_code); }}
}};
#endif // {include_guard}

%}}

// 为 {ret_class_name} 添加 Java 便捷方法
%typemap(javacode) {ret_class_name} %{{
    /**
     * 获取值，如果操作失败则抛出异常
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public {java_type} getValueOrThrow() throws DasException {{
        if (!IsOk()) {{
            throw new DasException(GetErrorCode());
        }}
        return GetValue();
    }}
%}}
 """

    def _has_idas_readonly_string_param(self, method: MethodDef) -> bool:
        """判断方法是否有 IDasReadOnlyString* 参数（非 [out]）"""
        for p in method.parameters:
            if p.direction != ParamDirection.OUT:
                if p.type_info.base_type == 'IDasReadOnlyString':
                    return True
        return False

    def _has_idas_typeinfo_in_interface_hierarchy(
        self, interface: InterfaceDef, all_interfaces: list[InterfaceDef]
    ) -> bool:
        """检查接口的继承链上是否存在 IDasTypeInfo

        Args:
            interface: 接口定义
            all_interfaces: 所有接口列表（用于查找基接口）

        Returns:
            如果继承链上存在 IDasTypeInfo 则返回 True
        """
        current = interface

        while current:
            # 检查当前接口是否是 IDasTypeInfo
            if current.name == 'IDasTypeInfo':
                return True

            # 查找基接口
            if current.base_interface:
                # 在所有接口中查找基接口
                base_interface = None
                for intf in all_interfaces:
                    if intf.name == current.base_interface:
                        base_interface = intf
                        break

                if base_interface:
                    current = base_interface
                else:
                    # 找不到基接口，停止搜索
                    break
            else:
                # 没有基接口，停止搜索
                break

        return False

    @staticmethod
    def _is_idas_readonly_string_out_param(out_type: str) -> bool:
        """判断是否是 IDasReadOnlyString** 类型的 [out] 参数"""
        simple_name = out_type.split('::')[-1]
        return simple_name == 'IDasReadOnlyString'

    def _generate_extend_wrapper(self, interface: InterfaceDef, method: MethodDef, out_param: ParameterDef) -> str:
        """生成 %extend 包装方法

        由于 SWIG typemap 难以同时修改返回类型和隐藏参数，
        我们使用 %extend 添加新方法，同时用 %ignore 隐藏原方法。

        Args:
            interface: 接口定义
            method: 方法定义
            out_param: [out] 参数定义

        Returns:
            %extend 包装代码
        """
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        out_type = out_param.type_info.base_type
        ret_class_name = self._get_ret_class_name(out_type)
        is_interface = self._is_interface_type(out_type)
        is_idas_readonly_string_out = self._is_idas_readonly_string_out_param(out_type)

        # 收集非 [out] 参数
        in_params: list[ParameterDef] = []
        for p in method.parameters:
            if p.direction != ParamDirection.OUT:
                in_params.append(p)

        # 检查是否需要生成 DasReadOnlyString 参数版本
        has_idas_readonly_string = self._has_idas_readonly_string_param(method)

        # 生成 C++ 参数列表
        cpp_params: list[str] = []
        call_args: list[str] = []
        for p in in_params:
            cpp_type = self._get_cpp_type(p.type_info.base_type)
            if self._is_interface_type(p.type_info.base_type):
                cpp_type = f"{p.type_info.base_type}*"
            cpp_params.append(f"{cpp_type} {p.name}")
            call_args.append(p.name)

        cpp_params_str = ", ".join(cpp_params)

        lines: list[str] = []

        # 对于 IDasReadOnlyString** 类型的 [out] 参数，需要特殊处理
        # 因为 DasRetReadOnlyString.value 是 DasReadOnlyString 类型（包装类），
        # 而方法需要的是 IDasReadOnlyString** 类型
        if is_idas_readonly_string_out:
            call_args_with_temp = call_args.copy()
            call_args_with_temp.append("&p_out_string")
            call_args_str = ", ".join(call_args_with_temp)

            lines.append(f"""
// 隐藏原始的 {method.name} 方法
%ignore {qualified_interface}::{method.name};

// 添加返回 {ret_class_name} 的包装方法
%extend {qualified_interface} {{
    {ret_class_name} {method.name}({cpp_params_str}) {{
        {ret_class_name} result;
        IDasReadOnlyString* p_out_string = nullptr;
        result.error_code = $self->{method.name}({call_args_str});
        result.value = p_out_string;  // DasReadOnlyString 可以从 IDasReadOnlyString* 隐式构造
        return result;
    }}
}}
""")
        else:
            # 调用原始方法的参数（包含 out 参数）
            # 接口类型：传递 &result.value（因为 value 是指针，所以传递指针的地址）
            # 值类型：传递 &result.value（因为 value 是值，所以传递值的地址）
            call_args.append("&result.value")
            call_args_str = ", ".join(call_args)

            lines.append(f"""
// 隐藏原始的 {method.name} 方法
%ignore {qualified_interface}::{method.name};

// 添加返回 {ret_class_name} 的包装方法
%extend {qualified_interface} {{
    {ret_class_name} {method.name}({cpp_params_str}) {{
        {ret_class_name} result;
        result.error_code = $self->{method.name}({call_args_str});
        return result;
    }}
}}
""")

        # 如果方法有 IDasReadOnlyString* 参数，生成额外的 DasReadOnlyString 参数版本
        if has_idas_readonly_string:
            # 生成 DasReadOnlyString 参数版本
            das_cpp_params: list[str] = []
            das_call_args: list[str] = []
            for p in in_params:
                if p.type_info.base_type == 'IDasReadOnlyString':
                    das_cpp_params.append(f"DasReadOnlyString {p.name}")
                    das_call_args.append(f"{p.name}.Get()")
                else:
                    cpp_type = self._get_cpp_type(p.type_info.base_type)
                    if self._is_interface_type(p.type_info.base_type):
                        cpp_type = f"{p.type_info.base_type}*"
                    das_cpp_params.append(f"{cpp_type} {p.name}")
                    das_call_args.append(p.name)

            das_cpp_params_str = ", ".join(das_cpp_params)

            # 对于 IDasReadOnlyString** 类型的 [out] 参数，需要特殊处理
            if is_idas_readonly_string_out:
                das_call_args.append("&p_out_string")
                das_call_args_str = ", ".join(das_call_args)

                lines.append(f"""
// 添加 DasReadOnlyString 参数版本的 {method.name} 方法
%extend {qualified_interface} {{
    {ret_class_name} {method.name}({das_cpp_params_str}) {{
        {ret_class_name} result;
        IDasReadOnlyString* p_out_string = nullptr;
        result.error_code = $self->{method.name}({das_call_args_str});
        result.value = p_out_string;  // DasReadOnlyString 可以从 IDasReadOnlyString* 隐式构造
        return result;
    }}
}}
""")
            else:
                das_call_args.append("&result.value")
                das_call_args_str = ", ".join(das_call_args)

                lines.append(f"""
// 添加 DasReadOnlyString 参数版本的 {method.name} 方法
%extend {qualified_interface} {{
    {ret_class_name} {method.name}({das_cpp_params_str}) {{
        {ret_class_name} result;
        result.error_code = $self->{method.name}({das_call_args_str});
        return result;
    }}
}}
""")

        return "\n".join(lines)

    def _generate_convenience_method(
        self,
        interface: InterfaceDef,
        method: MethodDef,
        out_param: ParameterDef,
    ) -> str:
        """生成便捷方法（Ez 后缀）

        这个方法直接返回结果，失败时抛出 DasException。

        Args:
            method: 方法定义
            out_param: [out] 参数定义

        Returns:
            Java 便捷方法代码
        """
        method_name = method.name
        out_type = out_param.type_info.base_type
        ret_class_name = self._get_ret_class_name(out_type)

        # 收集非 [out] 参数
        in_params: list[ParameterDef] = []
        for p in method.parameters:
            if p.direction != ParamDirection.OUT:
                in_params.append(p)

        # 生成 Java 参数列表
        java_params: list[str] = []
        param_names: list[str] = []
        for p in in_params:
            java_type = self._get_java_type(p.type_info.base_type)
            if self._is_interface_type(p.type_info.base_type):
                java_type = p.type_info.base_type
            java_params.append(f"{java_type} {p.name}")
            param_names.append(p.name)

        java_params_str = ", ".join(java_params)
        call_params_str = ", ".join(param_names)

        # 确定返回类型
        if self._is_interface_type(out_type):
            return_type = out_type
        else:
            return_type = self._get_java_type(out_type)

        result_lines: list[str] = []

        # 检查接口的继承链是否包含 IDasTypeInfo
        has_idas_typeinfo = self._has_idas_typeinfo_in_interface_hierarchy(
            interface, self._all_interfaces
        )

        # 根据接口类型生成不同的异常处理代码
        if has_idas_typeinfo:
            # 继承链上存在 IDasTypeInfo：使用 CreateDasExceptionStringWithTypeInfoSwig
            exception_code = f"""
    public {return_type} {method_name}Ez({java_params_str}) throws DasException {{
        {ret_class_name} ret = {method_name}({call_params_str});
        if (!ret.IsOk()) {{
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.setFile("{interface.name}.java");
            sourceInfo.setLine(-1);
            sourceInfo.setFunction("{method_name}Ez");
            throw new DasException(
                ret.GetErrorCode(),
                das.CreateDasExceptionStringWithTypeInfoSwig(
                    ret.GetErrorCode(),
                    sourceInfo,
                    this
                )
            );
        }}
        return ret.GetValue();
    }}"""
        else:
            # 继承链上不存在 IDasTypeInfo：使用 CreateDasExceptionStringSwig
            exception_code = f"""
    public {return_type} {method_name}Ez({java_params_str}) throws DasException {{
        {ret_class_name} ret = {method_name}({call_params_str});
        if (!ret.IsOk()) {{
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.setFile("{interface.name}.java");
            sourceInfo.setLine(-1);
            sourceInfo.setFunction("{method_name}Ez");
            throw new DasException(
                ret.GetErrorCode(),
                das.CreateDasExceptionStringSwig(ret.GetErrorCode(), sourceInfo)
            );
        }}
        return ret.GetValue();
    }}"""

        result_lines.append(f"""
    /**
     * {method_name} 的便捷版本
     * 直接返回结果，失败时抛出异常
     * @return {return_type} 结果
     * @throws DasException 当操作失败时
     */""")
        result_lines.append(exception_code)

        # 检查是否有 IDasReadOnlyString* 参数，如果有则生成 String 参数版本的便捷方法
        has_idas_readonly_string = self._has_idas_readonly_string_param(method)
        if has_idas_readonly_string:
            # 生成 String 参数版本的便捷方法（标记为 final）
            string_java_params: list[str] = []
            string_call_params: list[str] = []
            conversion_code_lines: list[str] = []

            for p in in_params:
                if p.type_info.base_type == 'IDasReadOnlyString':
                    string_java_params.append(f"String {p.name}")
                    string_call_params.append(f"das_{p.name}")
                    conversion_code_lines.append(f"DasReadOnlyString das_{p.name} = new DasReadOnlyString({p.name});")
                else:
                    java_type = self._get_java_type(p.type_info.base_type)
                    if self._is_interface_type(p.type_info.base_type):
                        java_type = p.type_info.base_type
                    string_java_params.append(f"{java_type} {p.name}")
                    string_call_params.append(p.name)

            string_java_params_str = ", ".join(string_java_params)
            string_call_params_str = ", ".join(string_call_params)
            conversion_code = "\n        ".join(conversion_code_lines)

            # 根据接口类型生成不同的异常处理代码
            if has_idas_typeinfo:
                string_exception_code = f"""
    public final {return_type} {method_name}({string_java_params_str}) throws DasException {{
        {conversion_code}
        {ret_class_name} ret = {method_name}({string_call_params_str});
        if (DuskAutoScript.IsFailed(ret.GetErrorCode())) {{
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.setFile("{interface.name}.java");
            sourceInfo.setLine(-1);
            sourceInfo.setFunction("{method_name}");
            throw new DasException(
                ret.GetErrorCode(),
                das.CreateDasExceptionStringWithTypeInfoSwig(
                    ret.GetErrorCode(),
                    sourceInfo,
                    this
                )
            );
        }}
        return ret.GetValue();
    }}"""
            else:
                string_exception_code = f"""
    public final {return_type} {method_name}({string_java_params_str}) throws DasException {{
        {conversion_code}
        {ret_class_name} ret = {method_name}({string_call_params_str});
        if (DuskAutoScript.IsFailed(ret.GetErrorCode())) {{
            DasExceptionSourceInfoSwig sourceInfo = new DasExceptionSourceInfoSwig();
            sourceInfo.setFile("{interface.name}.java");
            sourceInfo.setLine(-1);
            sourceInfo.setFunction("{method_name}");
            throw new DasException(
                ret.GetErrorCode(),
                das.CreateDasExceptionStringSwig(ret.GetErrorCode(), sourceInfo)
            );
        }}
        return ret.GetValue();
    }}"""

            result_lines.append(f"""

    /**
     * {method_name} 的便捷版本（使用 Java String 参数）
     * 直接返回结果，失败时抛出异常
     * @return {return_type} 结果
     * @throws DasException 当操作失败时
     */""")
            result_lines.append(string_exception_code)

        return "".join(result_lines)

    def generate_pre_include_directives(self, interface: InterfaceDef) -> str:
        """生成必须在 %include 之前的 SWIG 指令

        包括：
        1. 生成 DasRetXxx 返回包装类
        2. 生成 %ignore 和 %extend 包装
        3. 生成 %typemap(javacode) 便捷方法（包括 castFrom 和 createFromPtr）

        注意：对于 binary_buffer 接口（如 IDasBinaryBuffer），不生成任何代码，
        因为所有代码会在 generate_binary_buffer_helpers 中统一生成。
        """
        # 如果接口有任何 [binary_buffer] 方法，跳过处理
        # 这些接口的所有代码会在 generate_binary_buffer_helpers 中统一生成
        if self._is_binary_buffer_interface(interface):
            return ""

        lines: list[str] = []
        qualified_interface_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name

        # 获取带有 [out] 参数的方法（已自动排除 [binary_buffer] 方法和 void** 类型）
        out_param_methods = self._get_out_param_methods(interface)

        lines.append("#ifdef SWIGJAVA")

        # 1. 生成所有需要的 DasRetXxx 类（跳过预定义类型）
        if out_param_methods:
            generated_ret_types: set[str] = set()
            for method, out_param in out_param_methods:
                out_type = out_param.type_info.base_type
                if out_type not in generated_ret_types:
                    generated_ret_types.add(out_type)
                    ret_class_code = self._generate_ret_class(out_type)
                    if ret_class_code:
                        lines.append(ret_class_code)

            # 2. 生成 %ignore 和 %extend 包装
            for method, out_param in out_param_methods:
                lines.append(self._generate_extend_wrapper(interface, method, out_param))

        # 3. 生成 %typemap(javacode) 方法（包括便捷方法、castFrom 和 createFromPtr）
        if qualified_interface_name not in self._generated_javacode_interfaces:
            self._generated_javacode_interfaces.add(qualified_interface_name)

            lines.append(f"""
// ============================================================================
// {interface.name} 的 Java 辅助方法
// ============================================================================
%typemap(javacode) {qualified_interface_name} %{{""")

            # 生成 [out] 参数的便捷方法
            for method, out_param in out_param_methods:
                lines.append(self._generate_convenience_method(interface, method, out_param))

            # 生成 castFrom 和 createFromPtr 方法
            lines.append(self._generate_interface_helper_methods(interface))

            lines.append("%}")

        lines.append("#endif // SWIGJAVA")

        return "\n".join(lines)

    def generate_out_param_wrapper(self, interface: InterfaceDef, method: MethodDef, param: ParameterDef) -> str:
        """生成 [out] 参数的语言特定包装代码

        由于我们使用 %extend 方案，这里不需要额外的包装代码。
        """
        return ""

    def _generate_interface_helper_methods(self, interface: InterfaceDef) -> str:
        """为接口生成 Java 辅助方法（castFrom 和 createFromPtr）

        这些方法用于支持类型安全的接口转换：
        - castFrom(IDasBase base): 从 IDasBase 转换到目标类型（零开销，需确保类型正确）
        - createFromPtr(long ptr, boolean own): 内部工厂方法，供 as() 使用

        Args:
            interface: 接口定义

        Returns:
            Java 代码字符串
        """
        interface_name = interface.name

        return f"""
    /**
     * 从 IDasBase 转换到 {interface_name}（零开销转换）
     * <p>
     * 此方法执行零 C++ 调用的转换，适用于你确定类型正确的场景。
     * 如果不确定类型，请使用 as() 方法，它会通过 QueryInterface 验证类型。
     * </p>
     * @param base 源对象，必须拥有内存所有权（swigCMemOwn == true）
     * @return 转换后的 {interface_name} 对象
     * @throws IllegalStateException 如果 base 为 null 或不拥有内存所有权
     */
    public static {interface_name} castFrom(IDasBase base) {{
        if (base == null) {{
            throw new IllegalStateException("Cannot cast from null IDasBase");
        }}
        if (!base.swigCMemOwn) {{
            throw new IllegalStateException(
                "Cannot cast from IDasBase: source object does not own memory. " +
                "The object may have already been converted or released.");
        }}
        long ptr = IDasBase.getCPtr(base);
        base.swigCMemOwn = false;  // 转移所有权
        return new {interface_name}(ptr, true);
    }}

    /**
     * 内部工厂方法，从 C++ 指针创建 {interface_name} 实例
     * <p>
     * 此方法供 as() 反射调用使用。
     * </p>
     * @param cPtr C++ 对象指针
     * @param cMemoryOwn 是否拥有内存所有权
     * @return 新的 {interface_name} 实例
     */
    public static {interface_name} createFromPtr(long cPtr, boolean cMemoryOwn) {{
        return new {interface_name}(cPtr, cMemoryOwn);
    }}"""

    def _get_cpp_type(self, type_name: str) -> str:
        """获取 C++ 类型"""
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
        }
        return TYPE_MAP.get(type_name, type_name)

    def generate_binary_buffer_helpers(self, interface: Any, method_name: str, size_method_name: str) -> str:
        """生成 Java 的二进制缓冲区辅助方法

        对于有 [binary_buffer] 标记的方法，生成 ByteBuffer 访问接口：
        - getDataAsDirectBuffer(): 返回直接 ByteBuffer（零拷贝）
        - JNI native 方法：创建 DirectByteBuffer

        实现原理：
        1. 将 GetData 和 GetSize 重命名为私有方法（GetData_internal, GetSize_internal）
        2. 通过 %typemap(in) 将 [out] 参数转换为 jlongArray 输出
        3. 通过 %native 声明 JNI 方法创建 DirectByteBuffer
        4. 在 javacode 中调用私有方法获取地址和大小，然后创建 ByteBuffer

        注意：对于 binary_buffer 接口，不为其他 [out] 方法生成便捷方法，
        因为这些方法已被重命名为私有方法，用户应通过 getDataAsDirectBuffer() 访问数据。
        """
        qualified_name = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        native_name = f'{interface.name}_createDirectByteBuffer'
        # 用于 JNI 函数名的格式（将 :: 替换为 _）
        jni_class_name = qualified_name.replace('::', '_')

        return f"""
#ifdef SWIGJAVA
%typemap(javaclassmodifiers) {qualified_name} "public class"

// 将 GetData 和 GetSize 重命名为内部方法，并设为私有
%rename("GetData_internal") {qualified_name}::GetData;
%javamethodmodifiers {qualified_name}::GetData "private";
%rename("GetSize_internal") {qualified_name}::{size_method_name};
%javamethodmodifiers {qualified_name}::{size_method_name} "private";

// GetData 的 [out] 参数转换为 jlongArray 输出（存储指针地址）
%typemap(jni) unsigned char** pp_out_data "jlongArray"
%typemap(jtype) unsigned char** pp_out_data "long[]"
%typemap(jstype) unsigned char** pp_out_data "long[]"
%typemap(javain) unsigned char** pp_out_data "$javainput"
%typemap(in) unsigned char** pp_out_data (unsigned char* temp = nullptr) {{
    $1 = &temp;
}}
%typemap(argout) unsigned char** pp_out_data {{
    // 将指针地址写入 Java long[] 数组
    jlong ptr_value = (jlong)(intptr_t)temp$argnum;
    JCALL4(SetLongArrayRegion, jenv, $input, 0, 1, &ptr_value);
}}

// GetSize 的 [out] 参数转换为 jlongArray 输出
%typemap(jni) uint64_t* p_out_size "jlongArray"
%typemap(jtype) uint64_t* p_out_size "long[]"
%typemap(jstype) uint64_t* p_out_size "long[]"
%typemap(javain) uint64_t* p_out_size "$javainput"
%typemap(in) uint64_t* p_out_size (uint64_t temp = 0) {{
    $1 = &temp;
}}
%typemap(argout) uint64_t* p_out_size {{
    // 将大小写入 Java long[] 数组
    jlong size_value = (jlong)temp$argnum;
    JCALL4(SetLongArrayRegion, jenv, $input, 0, 1, &size_value);
}}

%typemap(javacode) {qualified_name} %{{
    private static native java.nio.ByteBuffer {native_name}(long address, int capacity);

    /**
     * 获取数据的直接 ByteBuffer（零拷贝）
     * @return ByteBuffer 包含二进制数据
     * @throws RuntimeException 如果获取数据失败
     */
    public java.nio.ByteBuffer getDataAsDirectBuffer() {{
        long[] ptrHolder = new long[1];
        long[] sizeHolder = new long[1];

        int hr = GetData_internal(ptrHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data pointer, error code: " + hr);
        }}

        hr = {size_method_name}_internal(sizeHolder);
        if (hr < 0) {{
            throw new RuntimeException("Failed to get data size, error code: " + hr);
        }}

        return {native_name}(ptrHolder[0], (int)sizeHolder[0]);
    }}
%}}

%native({native_name}) jobject {native_name}(jlong address, jint capacity);
%{{
JNIEXPORT jobject JNICALL Java_{jni_class_name}_{native_name}(
    JNIEnv *jenv, jclass jcls, jlong address, jint capacity) {{
    return jenv->NewDirectByteBuffer((void*)address, capacity);
}}
%}}
#endif
"""
