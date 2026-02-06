"""Java SWIG 生成器

生成 Java 特定的 SWIG .i 文件代码

核心设计：
1. 通过 SWIG typemap 完全隐藏 [out] 参数，让 Java 用户完全不感知 C++ 的通过参数传递返回值的模式。
2. 对于带 [out] 参数的方法，Java 端：
    - 主方法直接返回 DasRetXxx 包装类，包含错误码和结果
    - 便捷方法（Ez后缀）直接返回结果，失败时抛出 DasException

============================================================================
DasRetXxx 类的生命周期管理策略
============================================================================

DasRetXxx 类用于封装多返回值方法的返回结果。由于这些类可能包含
接口指针类型成员，需要正确管理引用计数，避免内存泄漏或悬垂指针。

设计原则：
1. 删除复制构造函数和复制赋值运算符，防止意外的浅拷贝
2. 提供移动构造函数和移动赋值运算符，使用 std::swap 高效转移资源所有权
3. 接口指针成员的生命周期管理：
   - Get 方法：在返回指针前调用 AddRef() 增加引用计数
   - Set 方法：在设置新指针前先 Release() 旧指针，再设置新指针并 AddRef()
   - 析构函数：如果接口指针有值则调用 Release() 释放资源

通过 &value 成员获取的指针具有完全所有权，可以安全地调用 Release()。
这确保接口指针的生命周期由 DasRetXxx 类管理，符合 COM 风格的引用计数规则。
============================================================================
"""

from das_idl_parser import InterfaceDef, MethodDef, ParameterDef, ParamDirection, PropertyDef, TypeInfo
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
        # 注意：int64_t 和 uint64_t 不在此列表中，让代码生成器生成 DasRetInt 和 DasRetUInt
        # 'int64_t': 'DasRetInt',  # 由代码生成器生成
        # 'uint64_t': 'DasRetUInt',  # 由代码生成器生成
        'DasReadOnlyString': 'DasRetReadOnlyString',
        # void** 类型 -> 映射到 DasRetBase（返回 IDasBase）
        'void': 'DasRetBase',
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
    def _to_java_method_name(prefix: str, prop_name: str) -> str:
        """将属性名和前缀组合为 Java 小驼峰方法名
        
        将 C++ PascalCase 的 Get/Set + 属性名转换为 Java 小驼峰命名：
        - ("get", "Score") -> "getScore"
        - ("set", "Score") -> "setScore"
        - ("get", "score") -> "getScore"
        - ("set", "match_rect") -> "setMatchRect"
        - ("get", "FileName") -> "getFileName"
        - ("is", "Ok") -> "isOk"
        
        Args:
            prefix: 方法前缀，如 "get", "set", "is"
            prop_name: 属性名（可能是 PascalCase 或 snake_case）
            
        Returns:
            Java 小驼峰方法名
        """
        # 如果是 snake_case（含下划线），转为 PascalCase
        if '_' in prop_name:
            parts = prop_name.split('_')
            pascal_name = ''.join(part.capitalize() for part in parts if part)
        else:
            # 确保首字母大写（PascalCase）
            pascal_name = prop_name[0].upper() + prop_name[1:] if prop_name else prop_name
        
        return f"{prefix}{pascal_name}"

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

    def generate_pre_include_directives(self, interface: InterfaceDef) -> str:
        """生成必须在 %include 之前的 SWIG 指令
        
        分别生成 %ignore 和 %extend 代码来包装带 [out] 参数的方法，
        并将它们收集到不同的全局字典中，以便分别聚合到 
        DasTypeMapsIgnore.i（%ignore）和 DasTypeMapsExtend.i（%extend）。
        
        同时生成 Ez 便捷方法的 javacode typemap，直接返回写入当前接口的 .i 文件。
        
        Args:
            interface: 接口定义
            
        Returns:
            javacode typemap 代码（如果有 Ez 便捷方法）
        """
        # 收集当前接口的所有 Ez 便捷方法
        ez_methods: list[str] = []
        
        # 为 [binary_buffer] 方法生成 %ignore 指令（阻止 SWIG 默认行为）
        for method in interface.methods:
            if self._is_binary_buffer_method(method):
                # 生成 %ignore 代码并存储
                ignore_code = self._generate_ignore_directive_for_binary_buffer(interface, method)
                if self._context:
                    typemap_key = f"{interface.namespace}::{interface.name}::{method.name}"
                    if not hasattr(self._context, '_global_typemaps_ignore'):
                        self._context._global_typemaps_ignore = {}
                    if typemap_key not in self._context._global_typemaps_ignore:
                        self._context._global_typemaps_ignore[typemap_key] = ignore_code
        
        # 处理带 [out] 参数的方法（排除 [binary_buffer]）
        for method, out_param in self._get_out_param_methods(interface, exclude_binary_buffer=True):
            # 生成 %ignore 代码并存储
            ignore_code = self._generate_ignore_directive(interface, method, out_param)
            if self._context:
                typemap_key = f"{interface.namespace}::{interface.name}::{method.name}"
                if not hasattr(self._context, '_global_typemaps_ignore'):
                    self._context._global_typemaps_ignore = {}
                if typemap_key not in self._context._global_typemaps_ignore:
                    self._context._global_typemaps_ignore[typemap_key] = ignore_code
            
            # 生成 %extend 代码并存储
            self._generate_extend_wrapper(interface, method, out_param)
            
            # 生成 Ez 便捷方法
            ez_method_code = self._generate_ez_method(interface, method, out_param)
            if ez_method_code:
                ez_methods.append(ez_method_code)
        
        # 处理不带 [out] 参数但有 IDasReadOnlyString* 输入参数的方法
        for method in self._get_methods_with_string_params_only(interface):
            # 生成 %ignore 代码并存储
            ignore_code = self._generate_ignore_directive_for_string_method(interface, method)
            if self._context:
                typemap_key = f"{interface.namespace}::{interface.name}::{method.name}"
                if not hasattr(self._context, '_global_typemaps_ignore'):
                    self._context._global_typemaps_ignore = {}
                if typemap_key not in self._context._global_typemaps_ignore:
                    self._context._global_typemaps_ignore[typemap_key] = ignore_code
            
            # 生成 %extend 代码并存储
            extend_code = self._generate_extend_wrapper_for_string_method(interface, method)
            if self._context:
                typemap_key = f"{interface.namespace}::{interface.name}::{method.name}_string"
                if typemap_key not in self._context._global_typemaps:
                    self._context._global_typemaps[typemap_key] = extend_code
        
        # 检查是否需要为接口生成 javacode typemap
        # 需要生成的条件：
        # 1. 有 Ez 便捷方法，或者
        # 2. 尚未为该接口生成过 javacode typemap（用于生成接口辅助方法）
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        need_javacode = len(ez_methods) > 0 or qualified_interface not in self._generated_javacode_interfaces
        
        if need_javacode:
            # 标记已生成，避免重复
            self._generated_javacode_interfaces.add(qualified_interface)
            
            # 构建 javacode 内容
            javacode_parts: list[str] = []
            
            # 1. Ez 便捷方法（如果有）
            if ez_methods:
                javacode_parts.append("// ============================================================================")
                javacode_parts.append("// Ez 便捷方法")
                javacode_parts.append("// 这些方法接受 Java String 参数，并在错误时抛出 DasException")
                javacode_parts.append("// ============================================================================")
                javacode_parts.extend(ez_methods)
            
            # 2. 接口辅助方法（castFrom 和 createFromPtr）
            javacode_parts.append("// ============================================================================")
            javacode_parts.append("// 接口辅助方法")
            javacode_parts.append("// castFrom: 从 IDasBase 零开销转换到目标类型")
            javacode_parts.append("// createFromPtr: 内部工厂方法，供 as() 使用")
            javacode_parts.append("// ============================================================================")
            javacode_parts.append(self._generate_interface_helper_methods(interface))
            
            javacode_content = "\n".join(javacode_parts)
            return f"""
#ifdef SWIGJAVA
// ============================================================================
// {interface.name} Java 辅助方法
// ============================================================================
%typemap(javacode) {qualified_interface} %{{
{javacode_content}
%}}
#endif // SWIGJAVA
"""
        
        return ""
    
    def _generate_ez_method(self, interface: InterfaceDef, method: MethodDef, out_param: ParameterDef) -> str:
        """生成 Ez 便捷方法的 Java 代码
        
        Ez 便捷方法接受 Java String 参数，调用内部的 Dispatch 方法，
        检查返回的错误码，失败时抛出 DasException，成功时返回结果。
        
        Args:
            interface: 接口定义
            method: 方法定义
            out_param: [out] 参数定义
            
        Returns:
            Java 便捷方法代码字符串，如果不需要生成则返回空字符串
        """
        # 所有带 [out] 参数的方法都需要生成 Ez 版本
        
        # 获取返回类型
        out_type = out_param.type_info.base_type
        ret_class_name = self._get_ret_class_name(out_type)
        is_interface = self._is_interface_type(out_type)
        java_return_type = out_type if is_interface else self._get_java_type(out_type)
        
        # 收集非 [out] 参数
        # 注意：对于属性 getter，参数的 direction 可能不是 OUT，但它实际上是输出参数
        # 通过 p is out_param 来识别这种情况
        in_params: list[ParameterDef] = []
        for p in method.parameters:
            if p.direction != ParamDirection.OUT and p is not out_param:
                in_params.append(p)
        
        # 生成 Java 参数列表（将 IDasReadOnlyString* 转换为 String）
        java_params: list[str] = []
        call_args: list[str] = []
        for p in in_params:
            if p.type_info.base_type == 'IDasReadOnlyString':
                java_params.append(f"String {p.name}")
                call_args.append(f"DasReadOnlyString.fromString({p.name})")
            else:
                java_type = self._get_java_type(p.type_info.base_type)
                if self._is_interface_type(p.type_info.base_type):
                    java_type = p.type_info.base_type
                java_params.append(f"{java_type} {p.name}")
                call_args.append(p.name)
        
        java_params_str = ", ".join(java_params)
        call_args_str = ", ".join(call_args)
        
        # 确定 Java 侧方法名（属性方法用小驼峰）
        if method.attributes and method.attributes.get('_is_property'):
            prop_name = method.attributes['_prop_name']
            if method.name.startswith('Get'):
                java_method_name = self._to_java_method_name('get', prop_name)
            elif method.name.startswith('Set'):
                java_method_name = self._to_java_method_name('set', prop_name)
            else:
                java_method_name = method.name
        else:
            java_method_name = method.name
        
        return f"""
    /**
     * {java_method_name} 的便捷方法（Ez 版本）
     * <p>
     * 接受 Java String 参数，失败时抛出 DasException。
     * </p>
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public final {java_return_type} {java_method_name}Ez({java_params_str}) throws DasException {{
        {ret_class_name} result = {java_method_name}({call_args_str});
        if (DuskAutoScript.IsFailed(result.getErrorCode())) {{
            throw DasException.fromErrorCode(result.getErrorCode(), "{interface.name}.{java_method_name}");
        }}
        return result.getValue();
    }}"""

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

    @staticmethod
    def _is_property_getter_method(method: MethodDef, interface: InterfaceDef) -> bool:
        """判断方法是否是属性 getter（通过 interface.properties 列表判断）

        Args:
            method: 方法定义
            interface: 接口定义

        Returns:
            如果是属性 getter 返回 True，否则返回 False
        """
        # 检查方法名是否匹配某个属性的 getter (Get + 属性名)
        for prop in interface.properties:
            if not prop.has_getter:
                continue
            if method.name == f"Get{prop.name}":
                return True
        return False

    def _get_out_param_methods(self, interface: InterfaceDef, exclude_binary_buffer: bool = True) -> list[tuple[MethodDef, ParameterDef]]:
        """获取接口中所有带有 [out] 参数的方法

        包括：
        1. 明确标记为 [out] 的参数
        2. 属性 getter 方法（通过 interface.properties 列表生成虚拟方法）

        Args:
            interface: 接口定义
            exclude_binary_buffer: 是否排除 [binary_buffer] 标记的方法

        Returns:
            方法和其第一个 [out] 参数的元组列表
        """
        result: list[tuple[MethodDef, ParameterDef]] = []
        
        # 首先处理 interface.methods 中的显式方法
        for method in interface.methods:
            # 如果需要排除 [binary_buffer] 方法，跳过它们
            if exclude_binary_buffer and self._is_binary_buffer_method(method):
                continue

            # 优先查找明确的 [out] 参数
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if out_params:
                # 如果有多个 [out] 参数，跳过此方法，由多返回值处理逻辑处理
                if len(out_params) >= 2:
                    continue
                out_param = out_params[0]
                # 每个方法只取第一个 [out] 参数
                # void** 类型会被特殊处理，映射到 DasRetBase（返回 IDasBase）
                result.append((method, out_param))
            # 如果没有明确的 [out] 参数，检查是否是属性 getter
            elif self._is_property_getter_method(method, interface):
                # 将属性 getter 的最后一个（唯一一个）参数视为 [out] 参数
                result.append((method, method.parameters[0]))
        
        # 然后为属性 getter 创建虚拟方法（如果它们不在 interface.methods 中）
        # ABI 生成器会为每个属性的 getter 生成方法：
        # - 基本类型: GetXxx(type* p_out)
        # - 接口类型: GetXxx(IXxx** pp_out)
        # - 字符串类型: GetXxx(IDasReadOnlyString** pp_out)
        for prop in interface.properties:
            if not prop.has_getter:
                continue
            
            method_name = f"Get{prop.name}"
            # 检查这个方法是否已经在 interface.methods 中
            already_in_methods = any(m.name == method_name for m in interface.methods)
            if already_in_methods:
                continue
            
            # 创建虚拟的方法定义
            base_type = prop.type_info.base_type
            is_interface = self._is_interface_type(base_type)
            is_string = base_type == 'IDasReadOnlyString'
            
            # 确定输出类型和指针级别
            if is_interface or is_string:
                # 接口和字符串类型：type** pp_out
                out_type = base_type if is_interface else 'IDasReadOnlyString'
                pointer_level = 2
            else:
                # 基本类型：type* p_out
                out_type = base_type
                pointer_level = 1
            
            # 创建虚拟参数
            out_param = ParameterDef(
                name='p_out',
                type_info=TypeInfo(
                    base_type=out_type,
                    is_pointer=True,
                    pointer_level=pointer_level,
                    is_const=False,
                    is_reference=False
                ),
                direction=ParamDirection.OUT
            )
            
            # 创建虚拟方法
            virtual_method = MethodDef(
                name=method_name,
                return_type=TypeInfo(base_type='DasResult'),
                parameters=[out_param],
                attributes={'_is_property': True, '_prop_name': prop.name}
            )
            
            result.append((virtual_method, out_param))

        return result

    def _get_multi_out_param_methods(self, interface: InterfaceDef) -> list[tuple[MethodDef, list[ParameterDef]]]:
        """获取接口中所有带有多个 [out] 参数的方法
        
        Args:
            interface: 接口定义
            
        Returns:
            方法和其所有 [out] 参数列表的元组列表
        """
        result: list[tuple[MethodDef, list[ParameterDef]]] = []
        
        for method in interface.methods:
            if self._is_binary_buffer_method(method):
                continue
            
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            
            if len(out_params) >= 2:
                result.append((method, out_params))
        
        return result

    def _get_methods_with_string_params_only(self, interface: InterfaceDef) -> list[MethodDef]:
        """获取接口中所有不带 [out] 参数但有 IDasReadOnlyString* 输入参数的方法

        这些方法需要生成 DasReadOnlyString 参数版本，以便 Java 用户可以直接使用。
        包括属性 setter 中使用 IDasReadOnlyString 类型的方法。

        Args:
            interface: 接口定义

        Returns:
            方法列表
        """
        result: list[MethodDef] = []
        for method in interface.methods:
            # 跳过 [binary_buffer] 方法
            if self._is_binary_buffer_method(method):
                continue
            # 跳过有 [out] 参数的方法（它们已经在 _get_out_param_methods 中处理）
            out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
            if out_params:
                continue
            # 检查是否有 IDasReadOnlyString* 输入参数
            if JavaSwigGenerator._has_idas_readonly_string_param(method):
                result.append(method)
        
        # 为属性 setter 创建虚拟方法（如果属性类型为 IDasReadOnlyString）
        # ABI 生成器会为 [set] IDasReadOnlyString 属性生成:
        #   SetXxx(::IDasReadOnlyString* p_value)
        for prop in interface.properties:
            if not prop.has_setter:
                continue
            if prop.type_info.base_type != 'IDasReadOnlyString':
                continue
            
            method_name = f"Set{prop.name}"
            # 检查是否已经在 interface.methods 中
            already_in_methods = any(m.name == method_name for m in interface.methods)
            if already_in_methods:
                continue
            
            # 创建虚拟的 setter 方法
            setter_param = ParameterDef(
                name='p_value',
                type_info=TypeInfo(
                    base_type='IDasReadOnlyString',
                    is_pointer=True,
                    pointer_level=1,
                    is_const=False,
                    is_reference=False
                ),
                direction=ParamDirection.IN
            )
            
            virtual_method = MethodDef(
                name=method_name,
                return_type=TypeInfo(base_type='DasResult'),
                parameters=[setter_param],
                attributes={'_is_property': True, '_prop_name': prop.name}
            )
            
            result.append(virtual_method)
        
        return result

    def _generate_ignore_directive_for_binary_buffer(self, interface: InterfaceDef, method: MethodDef) -> str:
        """为带 [binary_buffer] 标记的方法生成 %ignore 指令

        这些方法返回二进制缓冲区，Java无法直接处理C++指针类型，因此需要忽略原始方法。
        后续会通过 generate_binary_buffer_helpers 生成 Java 友好的辅助方法。

        Args:
            interface: 接口定义
            method: 方法定义

        Returns:
            SWIG %ignore 指令代码（带完整参数签名）
        """
        qualified_interface = f"{interface.namespace}::{interface.name}"

        # 构建完整参数签名
        param_signatures_with_prefix = []
        param_signatures_without_prefix = []
        current_namespace = interface.namespace

        for param in method.parameters:
            param_type = param.type_info.base_type
            param_type_with_prefix = param_type

            # 对于全局命名空间的接口类型，添加 :: 前缀
            # 注意：unsigned char 和 size_t 是 C++ 原生类型，不能添加 :: 前缀（会导致非法语法）
            if param_type == 'IDasReadOnlyString':
                param_type_with_prefix = f'::{param_type}'
            # unsigned char 和 size_t 保持原样，不添加 :: 前缀
            # 对于其他接口类型，检查命名空间
            elif self._is_interface_type(param_type):
                namespace = self.get_type_namespace(param_type)
                if namespace:
                    if namespace == current_namespace:
                        param_type_with_prefix = param_type
                    else:
                        param_type_with_prefix = f'::{namespace}::{param_type}'

            # 构建完整的类型签名，包括 const、指针和引用修饰符
            if param.type_info.is_pointer:
                stars = '*' * param.type_info.pointer_level
                param_signatures_with_prefix.append(f"{param_type_with_prefix}{stars}")
                param_signatures_without_prefix.append(f"{param_type}{stars}")
            else:
                param_signatures_with_prefix.append(param_type_with_prefix)
                param_signatures_without_prefix.append(param_type)

        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)

        result = f"""
// 隐藏原始的 {method.name} 方法（[binary_buffer] 标记，Java不支持此类参数，带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        # 如果两个签名不同，添加不带前缀的版本
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（[binary_buffer] 标记，Java不支持此类参数，不带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""
        return result

    def _generate_ignore_directive_for_string_method(self, interface: InterfaceDef, method: MethodDef) -> str:
        """为只有 IDasReadOnlyString* 输入参数的方法生成 %ignore 指令

        Args:
            interface: 接口定义
            method: 方法定义

        Returns:
            SWIG %ignore 指令代码（带完整参数签名）
        """
        qualified_interface = f"{interface.namespace}::{interface.name}"

        # 构建完整参数签名
        # 同时生成带 :: 前缀和不带前缀的版本
        param_signatures_with_prefix = []
        param_signatures_without_prefix = []
        current_namespace = interface.namespace
        for param in method.parameters:
            param_type = param.type_info.base_type
            param_type_with_prefix = param_type
            # 对于全局命名空间的类型，添加 :: 前缀
            if param_type == 'IDasReadOnlyString':
                param_type_with_prefix = '::IDasReadOnlyString'
            elif self._is_interface_type(param_type):
                namespace = self.get_type_namespace(param_type)
                if namespace:
                    if namespace == current_namespace:
                        param_type_with_prefix = param_type
                    else:
                        param_type_with_prefix = f'::{namespace}::{param_type}'
            if param.type_info.is_pointer:
                stars = '*' * param.type_info.pointer_level
                const_prefix = "const " if param.type_info.is_const else ""
                param_signatures_with_prefix.append(f"{const_prefix}{param_type_with_prefix}{stars}")
                param_signatures_without_prefix.append(f"{const_prefix}{param_type}{stars}")
            elif param.type_info.is_reference:
                # 处理引用类型（如 const DasGuid&）
                if param.type_info.is_const:
                    param_signatures_with_prefix.append(f"const {param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"const {param_type}&")
                else:
                    param_signatures_with_prefix.append(f"{param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"{param_type}&")
            else:
                param_signatures_with_prefix.append(param_type_with_prefix)
                param_signatures_without_prefix.append(param_type)

        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)

        result = f"""
// 隐藏原始的 {method.name} 方法（IDasReadOnlyString* 参数版本，带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        # 如果两个签名不同，添加不带前缀的版本
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（IDasReadOnlyString* 参数版本，不带 :: 前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""
        return result

    def _generate_extend_wrapper_for_string_method(self, interface: InterfaceDef, method: MethodDef) -> str:
        """为只有 IDasReadOnlyString* 输入参数的方法生成 %extend 包装

        生成使用 DasReadOnlyString 参数的版本，方便 Java 用户使用。

        Args:
            interface: 接口定义
            method: 方法定义

        Returns:
            SWIG %extend 代码
        """
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        
        # 生成 DasReadOnlyString 参数版本
        das_cpp_params: list[str] = []
        das_call_args: list[str] = []
        for p in method.parameters:
            if p.type_info.base_type == 'IDasReadOnlyString':
                das_cpp_params.append(f"DasReadOnlyString {p.name}")
                das_call_args.append(f"{p.name}.Get()")
            else:
                cpp_type = self._get_cpp_type(p.type_info.base_type)
                if self._is_interface_type(p.type_info.base_type):
                    namespace = self.get_type_namespace(p.type_info.base_type)
                    if namespace:
                        cpp_type = f"{namespace}::{p.type_info.base_type}*"
                    else:
                        cpp_type = f"{p.type_info.base_type}*"
                das_cpp_params.append(f"{cpp_type} {p.name}")
                das_call_args.append(p.name)
        
        das_cpp_params_str = ", ".join(das_cpp_params)
        das_call_args_str = ", ".join(das_call_args)
        
        # 获取返回类型
        return_type = method.return_type.base_type if method.return_type else "DasResult"
        
        # 确定 Java 侧方法名
        cpp_method_name = method.name
        if method.attributes and method.attributes.get('_is_property'):
            prop_name = method.attributes['_prop_name']
            if method.name.startswith('Get'):
                java_method_name = self._to_java_method_name('get', prop_name)
            elif method.name.startswith('Set'):
                java_method_name = self._to_java_method_name('set', prop_name)
            else:
                java_method_name = method.name
        else:
            java_method_name = method.name
        
        return f"""
// 添加 DasReadOnlyString 参数版本的 {method.name} 方法
%extend {qualified_interface} {{
    {return_type} {java_method_name}({das_cpp_params_str}) {{
        return $self->{cpp_method_name}({das_call_args_str});
    }}
}}
"""

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

    def _generate_ret_class(self, out_type: str, interface_name: Optional[str] = None) -> str:
        """生成 DasRetXxx 返回包装类

        这个类包含错误码和结果值，用于 Java 端接收带 [out] 参数方法的返回值。

        对于接口类型（如 IDasVariantVector），value 是指针类型
        对于值类型（如 size_t、DasGuid），value 是值类型

        注意：如果是预定义的返回类型，则不生成类定义（类已存在）

        Args:
            out_type: [out] 参数的类型名
            interface_name: 可选的接口名，用于查找对应的 header block

        Returns:
            Java 类定义（通过 %inline 嵌入），如果是预定义类型则返回空字符串
        """
        ret_class_name = self._get_ret_class_name(out_type)

        if ret_class_name in self._generated_ret_classes:
            if self.debug:
                print(f"[DEBUG] Ret class {ret_class_name} already generated, skipping")
            return ""
        self._generated_ret_classes.add(ret_class_name)
        if self.debug:
            print(f"[DEBUG] _generate_ret_class called for {out_type}, ret_class_name={ret_class_name}")

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
            include_guard = self._to_include_guard(ret_class_name)
            
            # 确定需要包含的头文件
            header_includes = []
            # 添加 utility 头文件用于 std::swap（移动语义）
            header_includes.append('#include <utility>')
            if "DasGuid" in out_type:
                header_includes.append('#include <DasBasicTypes.h>')
            
            if self._is_interface_type(out_type):
                simple_interface_name = out_type.split('::')[-1]
                if simple_interface_name == 'IDasBase':
                    header_includes.append('#include <das/IDasBase.h>')
                else:
                    idl_file_name = self.get_interface_idl_file(out_type)
                    if idl_file_name:
                        header_includes.append(f'#include <das/_autogen/idl/abi/{idl_file_name}.h>')
                    else:
                        header_includes.append(f'#include <das/_autogen/idl/abi/{simple_interface_name}.h>')
            else:
                # 尝试作为枚举类型查找
                idl_file_name = self.get_enum_idl_file(out_type)
                if idl_file_name:
                    header_includes.append(f'#include <das/_autogen/idl/abi/{idl_file_name}.h>')
            
            if interface_name and self._context and hasattr(self._context, '_global_header_blocks'):
                if interface_name in self._context._global_header_blocks:
                    header_block_data = self._context._global_header_blocks[interface_name]
                    header_block_content = header_block_data.get('code', '') if isinstance(header_block_data, dict) else header_block_data
                    for line in header_block_content.split('\n'):
                        if '#include' in line and '<' in line and '>' in line:
                            header_includes.append(line.strip())
            
            header_block = ""
            if header_includes:
                includes_str = '\n'.join(header_includes)
                header_block = f"""
#ifndef SWIG
{includes_str}
#endif
"""
            
            # 使用 %begin 将 struct 定义插入到生成的 C++ 代码最开头
            # 同时使用 %inline 让 SWIG 生成 Java 包装类
            struct_definition = f"""
#ifndef {include_guard}
#define {include_guard}
struct {ret_class_name} {{
    DasResult error_code;
    {cpp_value_type} value;

    // 默认构造函数
    {ret_class_name}() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value({cpp_default_value}) {{}}

    // 删除复制构造函数和复制赋值运算符
    {ret_class_name}(const {ret_class_name}&) = delete;
    {ret_class_name}& operator=(const {ret_class_name}&) = delete;

    // 移动构造函数
    {ret_class_name}({ret_class_name}&& other) noexcept
        : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value({cpp_default_value}) {{
        std::swap(error_code, other.error_code);
        std::swap(value, other.value);
    }}

    // 移动赋值运算符（仅C++使用，SWIG忽略）
#ifndef SWIG
    {ret_class_name}& operator=({ret_class_name}&& other) noexcept {{
        if (this != &other) {{
            // 释放当前资源
            if (value) {{
                value->Release();
            }}
            error_code = DAS_E_UNDEFINED_RETURN_VALUE;
            // 移动新资源
            std::swap(error_code, other.error_code);
            std::swap(value, other.value);
        }}
        return *this;
    }}
#endif // SWIG

    // 析构函数
    ~{ret_class_name}() {{
        if (value) {{
            value->Release();
        }}
    }}

    DasResult GetErrorCode() const {{ return error_code; }}
    void SetErrorCode(DasResult code) {{ error_code = code; }}

    {cpp_value_type} GetValue() const {{
        if (value) {{
            value->AddRef();
        }}
        return value;
    }}
    void SetValue({cpp_value_type} v) {{
        if (value) {{
            value->Release();
        }}
        value = v;
        if (value) {{
            value->AddRef();
        }}
    }}

    bool IsOk() const {{ return DAS::IsOk(error_code); }}
}};
#endif // {include_guard}
"""
            
            return f"""
// ============================================================================
// {ret_class_name} - 返回包装类（接口类型）
// 用于封装带有 [out] 参数的方法返回值
// ============================================================================

// 将 struct 定义插入到生成的 C++ 代码最开头（确保在 %extend 函数之前）
%begin %{{
{header_block}
{struct_definition}
%}}

#ifdef SWIGJAVA
// Java 命名规范：将 PascalCase 方法 rename 为小驼峰
%rename("getErrorCode") {ret_class_name}::GetErrorCode;
%rename("setErrorCode") {ret_class_name}::SetErrorCode;
%rename("getValue") {ret_class_name}::GetValue;
%rename("setValue") {ret_class_name}::SetValue;
%rename("isOk") {ret_class_name}::IsOk;
// 隐藏 public 字段的自动 getter/setter，避免重复方法
%ignore {ret_class_name}::error_code;
%ignore {ret_class_name}::value;
#endif // SWIGJAVA

// 让 SWIG 解析 struct 并生成 Java 包装类
%inline %{{
{struct_definition}
%}}

// 为 {ret_class_name} 添加 Java 便捷方法
%typemap(javacode) {ret_class_name} %{{
    /**
     * 获取值，如果操作失败则抛出异常
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public {java_type} getValueOrThrow() throws DasException {{
        if (!isOk()) {{
            throw new DasException(getErrorCode());
        }}
        return getValue();
    }}
%}}
  """

            if self.debug:
                print(f"[DEBUG] Returning ret_class_code for {ret_class_name}, length={len(ret_class_code)} chars")

            return ret_class_code
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
            
            header_includes = []
            if "DasGuid" in out_type:
                header_includes.append('#include <DasBasicTypes.h>')
            else:
                idl_file_name = self.get_enum_idl_file(out_type)
                if idl_file_name:
                    header_includes.append(f'#include <das/_autogen/idl/abi/{idl_file_name}.h>')
                elif interface_name and self._context and hasattr(self._context, '_global_header_blocks'):
                    if interface_name in self._context._global_header_blocks:
                        header_block_data = self._context._global_header_blocks[interface_name]
                        header_block_content = header_block_data.get('code', '') if isinstance(header_block_data, dict) else header_block_data
                        for line in header_block_content.split('\n'):
                            if '#include' in line and '<' in line and '>' in line:
                                header_includes.append(line.strip())
            
            header_block = ""
            if header_includes:
                includes_str = '\n'.join(header_includes)
                header_block = f"""
#ifndef SWIG
{includes_str}
#endif
"""
            
            # 使用 %begin 将 struct 定义插入到生成的 C++ 代码最开头
            # 同时使用 %inline 让 SWIG 生成 Java 包装类
            struct_definition = f"""
#ifndef {include_guard}
#define {include_guard}
struct {ret_class_name} {{
    DasResult error_code;
    {cpp_value_type} value;

    {ret_class_name}() : error_code(DAS_E_UNDEFINED_RETURN_VALUE){f", value({cpp_default_value})" if cpp_default_value else ""} {{}}

    DasResult GetErrorCode() const {{ return error_code; }}
    void SetErrorCode(DasResult code) {{ error_code = code; }}

    {cpp_value_type} GetValue() const {{ return value; }}
    void SetValue({cpp_value_type} v) {{ value = v; }}

    bool IsOk() const {{ return DAS::IsOk(error_code); }}
}};
#endif // {include_guard}
"""
            
            return f"""
// ============================================================================
// {ret_class_name} - 返回包装类（值类型）
// 用于封装带有 [out] 参数的方法返回值
// ============================================================================

// 将 struct 定义插入到生成的 C++ 代码最开头（确保在 %extend 函数之前）
%begin %{{
{header_block}
{struct_definition}
%}}

#ifdef SWIGJAVA
// Java 命名规范：将 PascalCase 方法 rename 为小驼峰
%rename("getErrorCode") {ret_class_name}::GetErrorCode;
%rename("setErrorCode") {ret_class_name}::SetErrorCode;
%rename("getValue") {ret_class_name}::GetValue;
%rename("setValue") {ret_class_name}::SetValue;
%rename("isOk") {ret_class_name}::IsOk;
// 隐藏 public 字段的自动 getter/setter，避免重复方法
%ignore {ret_class_name}::error_code;
%ignore {ret_class_name}::value;
#endif // SWIGJAVA

// 让 SWIG 解析 struct 并生成 Java 包装类
%inline %{{
{struct_definition}
%}}

// 为 {ret_class_name} 添加 Java 便捷方法
%typemap(javacode) {ret_class_name} %{{
    /**
     * 获取值，如果操作失败则抛出异常
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public {java_type} getValueOrThrow() throws DasException {{
        if (!isOk()) {{
            throw new DasException(getErrorCode());
        }}
        return getValue();
    }}
%}}
  """
            if self.debug:
                print(f"[DEBUG] Returning ret_class_code for {ret_class_name}, length={len(ret_class_code)} chars")

    def _generate_multi_ret_class(self, out_params: list[ParameterDef], interface_name: str | None = None) -> str:
        """生成多返回值包装类 (如 DasRetDasResultDasReadOnlyString)
        
        该类包含完整的生命周期管理：
        - 删除复制构造函数和复制赋值运算符
        - 提供移动构造函数和移动赋值运算符
        - 接口指针类型成员的引用计数管理（AddRef/Release）
        - 析构函数自动释放资源
        
        Args:
            out_params: 多个输出参数列表
            interface_name: 可选的接口名称，用于确定头文件包含
            
        Returns:
            生成的 C++ 类定义代码（使用 %begin/%inline 块）
        """
        if not out_params:
            return ""
        
        class_name_parts = []
        for param in out_params:
            type_name = param.type_info.base_type.split('::')[-1]
            clean_name = type_name.lstrip('I').rstrip('*')
            class_name_parts.append(clean_name)
        class_name = "DasRet" + "".join(class_name_parts)
        
        if class_name in self._generated_ret_classes:
            if self.debug:
                print(f"[DEBUG] Multi-ret class {class_name} already generated, skipping")
            return ""
        self._generated_ret_classes.add(class_name)
        
        CORE_TYPES_IN_IDASBASE = {'IDasBase', 'IDasReadOnlyString', 'IDasSwigBase'}
        
        header_includes = []
        # 添加 utility 头文件用于 std::swap
        header_includes.append('#include <utility>')
        
        for i, param in enumerate(out_params):
            out_type = param.type_info.base_type
            simple_type_name = out_type.split('::')[-1]
            
            if "DasGuid" in out_type:
                header_includes.append('#include <DasBasicTypes.h>')
            elif simple_type_name in CORE_TYPES_IN_IDASBASE:
                header_includes.append('#include <das/IDasBase.h>')
            elif self._is_interface_type(out_type):
                idl_file_name = self.get_interface_idl_file(out_type)
                if idl_file_name:
                    header_includes.append(f'#include <das/_autogen/idl/abi/{idl_file_name}.h>')
                else:
                    header_includes.append('#include <das/IDasBase.h>')
            else:
                idl_file_name = self.get_enum_idl_file(out_type)
                if idl_file_name:
                    header_includes.append(f'#include <das/_autogen/idl/abi/{idl_file_name}.h>')
        
        header_includes = list(dict.fromkeys(header_includes))
        header_block = ""
        if header_includes:
            includes_str = '\n'.join(header_includes)
            header_block = f"""
#ifndef SWIG
{includes_str}
#endif
"""
        
        value_fields = []
        value_initializers = []
        getter_methods = []
        setter_methods = []
        swap_code_lines = []
        destructor_lines = []
        java_types = []
        interface_indices = []  # 记录哪些 value 成员是接口指针类型
        
        for i, param in enumerate(out_params):
            out_type = param.type_info.base_type
            cpp_type = self._get_cpp_type(out_type)
            java_type = self._get_java_type(out_type)
            is_interface = self._is_interface_type(out_type)
            pointer_level = getattr(param.type_info, 'pointer_level', 1 if param.type_info.is_pointer else 0)
            
            if pointer_level >= 2:
                namespace = self.get_type_namespace(out_type)
                if namespace:
                    cpp_type = f"{namespace}::{out_type.split('::')[-1]}*"
                else:
                    cpp_type = f"{out_type.split('::')[-1]}*"
                if is_interface:
                    java_type = out_type.split('::')[-1]
                    interface_indices.append(i)
                value_initializers.append(f"value{i+1}(nullptr)")
            elif pointer_level == 1:
                simple_type = out_type.split('::')[-1]
                namespace = self.get_type_namespace(out_type)
                if namespace:
                    cpp_type = f"{namespace}::{simple_type}"
                else:
                    cpp_type = simple_type
                value_initializers.append(f"value{i+1}()")
            else:
                value_initializers.append(f"value{i+1}()")
            
            value_fields.append(f"    {cpp_type} value{i+1};")
            
            # 生成 Get 方法 - 接口指针类型需要 AddRef
            if i in interface_indices:
                getter_methods.append(f"""    {cpp_type} GetValue{i+1}() const {{
        if (value{i+1}) {{
            value{i+1}->AddRef();
        }}
        return value{i+1};
    }}""")
            else:
                getter_methods.append(f"    {cpp_type} GetValue{i+1}() const {{ return value{i+1}; }}")
            
            # 生成 Set 方法 - 接口指针类型需要 Release/AddRef
            if i in interface_indices:
                setter_methods.append(f"""    void SetValue{i+1}({cpp_type} new_value) {{
        if (value{i+1}) {{
            value{i+1}->Release();
        }}
        value{i+1} = new_value;
        if (value{i+1}) {{
            value{i+1}->AddRef();
        }}
    }}""")
            else:
                setter_methods.append(f"    void SetValue{i+1}({cpp_type} new_value) {{ value{i+1} = new_value; }}")
            
            # 生成 swap 代码
            swap_code_lines.append(f"        std::swap(value{i+1}, other.value{i+1});")
            
            # 生成析构函数代码 - 接口指针类型需要 Release
            if i in interface_indices:
                destructor_lines.append(f"        if (value{i+1}) {{ value{i+1}->Release(); }}")
            
            java_types.append(java_type)
        
        include_guard = self._to_include_guard(class_name)
        initializer_list = ", ".join(value_initializers)
        
        # 构建析构函数
        destructor = ""
        if destructor_lines:
            destructor_body = "\n".join(destructor_lines)
            destructor = f"""
    ~{class_name}() {{
{destructor_body}
    }}"""
        
        # 构建移动构造函数和移动赋值运算符
        move_constructor = f"""
    // 移动构造函数
    {class_name}({class_name}&& other) noexcept
        : error_code(DAS_E_UNDEFINED_RETURN_VALUE), {initializer_list} {{
        std::swap(error_code, other.error_code);
{chr(10).join(swap_code_lines)}
    }}"""
        
        move_assignment = f"""
#ifndef SWIG
    // 移动赋值运算符（仅C++使用，SWIG忽略）
    {class_name}& operator=({class_name}&& other) noexcept {{
        if (this != &other) {{
            // 释放当前资源
{chr(10).join(destructor_lines) if destructor_lines else '            // 无接口指针需要释放'}
            error_code = DAS_E_UNDEFINED_RETURN_VALUE;
            // 移动新资源
            std::swap(error_code, other.error_code);
{chr(10).join(swap_code_lines)}
        }}
        return *this;
    }}
#endif // SWIG"""
        
        struct_definition = f"""
#ifndef {include_guard}
#define {include_guard}
struct {class_name} {{
    DasResult error_code;
{chr(10).join(value_fields)}

    // 默认构造函数
    {class_name}() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), {initializer_list} {{}}
    
    // 删除复制构造函数和复制赋值运算符
    {class_name}(const {class_name}&) = delete;
    {class_name}& operator=(const {class_name}&) = delete;
    
    // 移动构造函数和移动赋值运算符
{move_constructor}
{move_assignment}
    
    // 析构函数
{destructor}

    DasResult GetErrorCode() const {{ return error_code; }}
    void SetErrorCode(DasResult code) {{ error_code = code; }}
{chr(10).join(getter_methods)}

{chr(10).join(setter_methods)}

    bool IsOk() const {{ return DAS::IsOk(error_code); }}
}};
#endif // {include_guard}
"""
        
        # 生成 %rename 和 %ignore 指令（多返回值的 GetValue{i}/SetValue{i} 和 value{i} 字段）
        rename_getter_lines = []
        rename_setter_lines = []
        ignore_field_lines = []
        for i in range(len(out_params)):
            rename_getter_lines.append(f'%rename("getValue{i+1}") {class_name}::GetValue{i+1};')
            rename_setter_lines.append(f'%rename("setValue{i+1}") {class_name}::SetValue{i+1};')
            ignore_field_lines.append(f'%ignore {class_name}::value{i+1};')
        rename_getters = "\n".join(rename_getter_lines)
        rename_setters = "\n".join(rename_setter_lines)
        ignore_value_fields = "\n".join(ignore_field_lines)
        
        java_getters = []
        for i, java_type in enumerate(java_types):
            java_getters.append(f"""
    /**
     * 获取值{i+1}，如果操作失败则抛出异常
     * @return 结果值{i+1}
     * @throws DasException 当操作失败时
     */
    public {java_type} getValue{i+1}OrThrow() throws DasException {{
        if (!isOk()) {{
            throw new DasException(getErrorCode());
        }}
        return getValue{i+1}();
    }}""")
        
        ret_class_code = f"""
// ============================================================================
// {class_name} - 多返回值包装类
// 用于封装带有多个 [out] 参数的方法返回值
// 包含完整的生命周期管理（移动语义、引用计数）
// ============================================================================

%begin %{{
{header_block}
{struct_definition}
%}}

#ifdef SWIGJAVA
// Java 命名规范：将 PascalCase 方法 rename 为小驼峰
%rename("getErrorCode") {class_name}::GetErrorCode;
%rename("setErrorCode") {class_name}::SetErrorCode;
%rename("isOk") {class_name}::IsOk;
{rename_getters}
{rename_setters}
// 隐藏 public 字段的自动 getter/setter，避免重复方法
%ignore {class_name}::error_code;
{ignore_value_fields}
#endif // SWIGJAVA

%inline %{{
{struct_definition}
%}}

%typemap(javacode) {class_name} %{{{{"".join(java_getters)}}
%}}
"""        
        if self._context:
            ret_class_key = f"ret_class_{class_name}"
            if ret_class_key not in self._context._global_ret_classes:
                self._context._global_ret_classes[ret_class_key] = ret_class_code
        
        return ret_class_code

    def _generate_multi_out_wrapper(self, interface: InterfaceDef, method: MethodDef, out_params: list[ParameterDef]) -> str:
        """生成多返回值方法的 %extend 包装代码
        
        Args:
            interface: 接口定义
            method: 方法定义
            out_params: 所有 [out] 参数列表
            
        Returns:
            %extend 包装代码字符串
        """
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        method_name = method.name
        
        ret_class_name_parts = []
        for param in out_params:
            type_name = param.type_info.base_type.split('::')[-1]
            clean_name = type_name.lstrip('I').rstrip('*')
            ret_class_name_parts.append(clean_name)
        ret_class_name = "DasRet" + "".join(ret_class_name_parts)
        
        in_params = [p for p in method.parameters if p.direction != ParamDirection.OUT]
        
        cpp_params: list[str] = []
        call_args: list[str] = []
        for p in in_params:
            cpp_type = self._get_cpp_type(p.type_info.base_type)
            if self._is_interface_type(p.type_info.base_type):
                namespace = self.get_type_namespace(p.type_info.base_type)
                if namespace:
                    cpp_type = f"{namespace}::{p.type_info.base_type}*"
                else:
                    cpp_type = f"{p.type_info.base_type}*"
            elif p.type_info.is_pointer:
                namespace = self.get_type_namespace(p.type_info.base_type)
                if namespace:
                    cpp_type = f"{namespace}::{p.type_info.base_type}*"
                else:
                    cpp_type = f"{p.type_info.base_type}*"
            cpp_params.append(f"{cpp_type} {p.name}")
            call_args.append(p.name)
        
        for i, param in enumerate(out_params):
            call_args.append(f"&result.value{i+1}")
        
        extend_code = f"""
%extend {qualified_interface} {{
    {ret_class_name} {method_name}({', '.join(cpp_params)}) {{
        {ret_class_name} result;
        result.error_code = $self->{method_name}({', '.join(call_args)});
        return result;
    }}
}}
"""
        
        # 存储到 _global_typemaps，由 DasTypeMapsExtend.i 统一包含
        # 避免直接写入接口的 .i 文件导致重复定义（Warning 302）
        if self._context:
            typemap_key = f"{qualified_interface}::{method_name}_multi_out"
            if typemap_key not in self._context._global_typemaps:
                self._context._global_typemaps[typemap_key] = extend_code
            
            # 生成 %ignore 代码，隐藏原始方法
            # 生成 %ignore 代码，使用与 _generate_ignore_directive 相同的逻辑
            # 同时生成带前缀和不带前缀的版本
            param_signatures_with_prefix = []
            param_signatures_without_prefix = []
            current_namespace = interface.namespace
            for param in method.parameters:
                param_type = param.type_info.base_type
                param_type_with_prefix = param_type
                if param_type == 'IDasReadOnlyString':
                    param_type_with_prefix = '::IDasReadOnlyString'
                elif self._is_interface_type(param_type):
                    namespace = self.get_type_namespace(param_type)
                    if namespace:
                        if namespace == current_namespace:
                            param_type_with_prefix = param_type
                        else:
                            param_type_with_prefix = f'::{namespace}::{param_type}'
                
                is_out_param = param.direction == ParamDirection.OUT
                if param.type_info.is_pointer or (is_out_param and self._is_interface_type(param_type)):
                    if is_out_param:
                        if self._is_interface_type(param_type) or param_type == 'IDasReadOnlyString':
                            pointer_level = 2
                        else:
                            pointer_level = param.type_info.pointer_level if param.type_info.is_pointer else 1
                    else:
                        pointer_level = param.type_info.pointer_level
                    stars = '*' * pointer_level
                    const_prefix = "const " if param.type_info.is_const else ""
                    param_signatures_with_prefix.append(f"{const_prefix}{param_type_with_prefix}{stars}")
                    param_signatures_without_prefix.append(f"{const_prefix}{param_type}{stars}")
                elif param.type_info.is_reference:
                    if param.type_info.is_const:
                        param_signatures_with_prefix.append(f"const {param_type_with_prefix}&")
                        param_signatures_without_prefix.append(f"const {param_type}&")
                    else:
                        param_signatures_with_prefix.append(f"{param_type_with_prefix}&")
                        param_signatures_without_prefix.append(f"{param_type}&")
                else:
                    param_signatures_with_prefix.append(param_type_with_prefix)
                    param_signatures_without_prefix.append(param_type)
            
            param_list_with_prefix = ", ".join(param_signatures_with_prefix)
            param_list_without_prefix = ", ".join(param_signatures_without_prefix)
            
            ignore_code = f"""
%ignore {qualified_interface}::{method_name}({param_list_with_prefix});
"""
            if param_list_with_prefix != param_list_without_prefix:
                ignore_code += f"""
%ignore {qualified_interface}::{method_name}({param_list_without_prefix});
"""
            
            ignore_key = f"{qualified_interface}::{method_name}_multi_out"
            if not hasattr(self._context, '_global_typemaps_ignore'):
                self._context._global_typemaps_ignore = {}
            if ignore_key not in self._context._global_typemaps_ignore:
                self._context._global_typemaps_ignore[ignore_key] = ignore_code
        
        return extend_code

    @staticmethod
    def _has_idas_readonly_string_param(method: MethodDef) -> bool:
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

    def _generate_ignore_directive(self, interface: InterfaceDef, method: MethodDef, out_param: ParameterDef) -> str:
        """生成带参数签名的 %ignore 指令代码

        Args:
            interface: 接口定义
            method: 方法定义
            out_param: [out] 参数定义

        Returns:
            SWIG %ignore 指令代码（带完整参数签名）
        """
        # 对于全局命名空间的接口，不使用 :: 前缀
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        
        # 构建完整参数签名：包括所有参数
        # 对于全局命名空间的类型，使用 :: 前缀（与头文件生成保持一致）
        # 对于接口类型，检查是否与当前接口在同一命名空间，如果是则使用相对名称
        param_signatures_with_prefix = []
        param_signatures_without_prefix = []
        current_namespace = interface.namespace  # 当前接口的命名空间
        for param in method.parameters:
            param_type = param.type_info.base_type
            param_type_with_prefix = param_type
            # 对于全局命名空间的类型（IDasReadOnlyString），添加 :: 前缀
            if param_type == 'IDasReadOnlyString':
                param_type_with_prefix = '::IDasReadOnlyString'
            # 对于其他接口类型，检查命名空间
            elif self._is_interface_type(param_type):
                namespace = self.get_type_namespace(param_type)
                if namespace:
                    # 如果参数类型与当前接口在同一命名空间，使用相对名称（不带命名空间前缀）
                    # 否则使用完整命名空间
                    if namespace == current_namespace:
                        # 同一命名空间，头文件中使用相对名称
                        param_type_with_prefix = param_type
                    else:
                        param_type_with_prefix = f'::{namespace}::{param_type}'
            
            # 构建完整的类型签名，包括 const、指针和引用修饰符
            # 对于 [out] 参数，ABI 生成器的处理逻辑是：
            # - 对于接口类型：总是使用 ** (固定为2级指针，忽略原始 pointer_level)
            # - 对于字符串类型：总是使用 ** (固定为2级指针)
            # - 对于其他类型：使用原始的 pointer_level
            # 注意：对于属性 getter，参数的 direction 可能不是 OUT，但它实际上是输出参数
            # 通过 param is out_param 来识别这种情况
            is_out_param = param.direction == ParamDirection.OUT or param is out_param
            if param.type_info.is_pointer or (is_out_param and self._is_interface_type(param_type)):
                if is_out_param:
                    # [out] 参数：接口类型和字符串类型固定为 **，其他类型增加一级指针
                    if self._is_interface_type(param_type) or param_type == 'IDasReadOnlyString':
                        pointer_level = 2  # 固定为2级指针
                    else:
                        # 对于非接口类型的 [out] 参数（如属性 getter 的 double* p_out），
                        # ABI 生成器会将其变成指针类型（增加一级指针）
                        # 例如：属性 double score -> getter 方法 Getscore(double* p_out)
                        pointer_level = param.type_info.pointer_level if param.type_info.is_pointer else 1
                else:
                    pointer_level = param.type_info.pointer_level
                stars = '*' * pointer_level
                # 处理 const 修饰符
                const_prefix = "const " if param.type_info.is_const else ""
                param_signatures_with_prefix.append(f"{const_prefix}{param_type_with_prefix}{stars}")
                param_signatures_without_prefix.append(f"{const_prefix}{param_type}{stars}")
            elif param.type_info.is_reference:
                # 处理引用类型（如 const DasGuid&）
                if param.type_info.is_const:
                    param_signatures_with_prefix.append(f"const {param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"const {param_type}&")
                else:
                    param_signatures_with_prefix.append(f"{param_type_with_prefix}&")
                    param_signatures_without_prefix.append(f"{param_type}&")
            else:
                param_signatures_with_prefix.append(param_type_with_prefix)
                param_signatures_without_prefix.append(param_type)
        
        param_list_with_prefix = ", ".join(param_signatures_with_prefix)
        param_list_without_prefix = ", ".join(param_signatures_without_prefix)
        
        result = f"""
// 隐藏原始的 {method.name} 方法（带完整参数签名，带命名空间前缀）
%ignore {qualified_interface}::{method.name}({param_list_with_prefix});
"""
        # 如果两个签名不同，添加不带前缀的版本
        if param_list_with_prefix != param_list_without_prefix:
            result += f"""
// 隐藏原始的 {method.name} 方法（不带命名空间前缀）
%ignore {qualified_interface}::{method.name}({param_list_without_prefix});
"""
        
        # 如果方法有 IDasReadOnlyString* 输入参数，还需要隐藏 %extend 生成的 IDasReadOnlyString* 参数版本
        # （只保留 DasReadOnlyString 参数版本）
        if JavaSwigGenerator._has_idas_readonly_string_param(method):
            # 构建 %extend 生成的方法签名（不包括 [out] 参数）
            # %extend 中接口类型使用命名空间前缀但不带全局 :: （如 Das::ExportInterface::IDasVariantVector）
            extend_param_signatures = []
            for param in method.parameters:
                if param.direction == ParamDirection.OUT:
                    continue  # 跳过 [out] 参数
                param_type = param.type_info.base_type
                # 对于接口类型，添加命名空间前缀（不带全局 ::）
                if self._is_interface_type(param_type) and param_type != 'IDasReadOnlyString':
                    namespace = self.get_type_namespace(param_type)
                    if namespace:
                        param_type = f'{namespace}::{param_type}'
                if param.type_info.is_pointer:
                    stars = '*' * param.type_info.pointer_level
                    extend_param_signatures.append(f"{param_type}{stars}")
                else:
                    extend_param_signatures.append(param_type)
            
            extend_param_list = ", ".join(extend_param_signatures)
            
            result += f"""
// 隐藏 %extend 生成的 {method.name} 方法（IDasReadOnlyString* 参数版本，保留 DasReadOnlyString 参数版本）
%ignore {qualified_interface}::{method.name}({extend_param_list});
"""
        
        return result

    def _generate_extend_wrapper(self, interface: InterfaceDef, method: MethodDef, out_param: ParameterDef) -> str:
        """生成 %extend 包装代码

        通过 %extend 添加返回 DasRetXxx 的包装方法。
        注意：%ignore 在 _generate_ignore_directive 中单独生成

        Args:
            interface: 接口定义
            method: 方法定义
            out_param: [out] 参数定义

        Returns:
            SWIG .i 文件中的 %extend 代码
        """
        qualified_interface = f"{interface.namespace}::{interface.name}" if interface.namespace else interface.name
        out_type = out_param.type_info.base_type
        ret_class_name = self._get_ret_class_name(out_type)
        is_interface = self._is_interface_type(out_type)
        is_idas_readonly_string_out = self._is_idas_readonly_string_out_param(out_type)
        is_void_pointer_pointer = self._is_void_pointer_pointer(out_type)

        # 确定 Java 侧方法名和 C++ 侧方法名
        # 对于属性方法，Java 侧使用小驼峰命名
        cpp_method_name = method.name  # 用于 $self-> 调用
        if method.attributes and method.attributes.get('_is_property'):
            prop_name = method.attributes['_prop_name']
            if method.name.startswith('Get'):
                java_method_name = self._to_java_method_name('get', prop_name)
            elif method.name.startswith('Set'):
                java_method_name = self._to_java_method_name('set', prop_name)
            else:
                java_method_name = method.name
        else:
            java_method_name = method.name

        # 收集非 [out] 参数
        # 注意：对于属性 getter，参数的 direction 可能不是 OUT，但它实际上是输出参数
        # 通过 p is out_param 来识别这种情况
        in_params: list[ParameterDef] = []
        for p in method.parameters:
            if p.direction != ParamDirection.OUT and p is not out_param:
                in_params.append(p)

        # 检查是否需要生成 DasReadOnlyString 参数版本
        has_idas_readonly_string = JavaSwigGenerator._has_idas_readonly_string_param(method)

        # 生成 C++ 参数列表
        cpp_params: list[str] = []
        call_args: list[str] = []
        for p in in_params:
            cpp_type = self._get_cpp_type(p.type_info.base_type)
            if self._is_interface_type(p.type_info.base_type):
                # 获取接口的完整命名空间限定名，让 SWIG 能正确映射到 Java 类
                namespace = self.get_type_namespace(p.type_info.base_type)
                if namespace:
                    cpp_type = f"{namespace}::{p.type_info.base_type}*"
                else:
                    cpp_type = f"{p.type_info.base_type}*"
            elif p.type_info.is_pointer:
                namespace = self.get_type_namespace(p.type_info.base_type)
                if namespace:
                    cpp_type = f"{namespace}::{p.type_info.base_type}*"
                else:
                    cpp_type = f"{p.type_info.base_type}*"
            cpp_params.append(f"{cpp_type} {p.name}")
            call_args.append(p.name)

        cpp_params_str = ", ".join(cpp_params)

        lines: list[str] = []

        # 对于 void** 类型的 [out] 参数，映射到 DasRetBase（返回 IDasBase）
        if is_void_pointer_pointer:
            call_args_with_temp = call_args.copy()
            call_args_with_temp.append("reinterpret_cast<void**>(&result.value)")
            call_args_str = ", ".join(call_args_with_temp)

            lines.append(f"""
// 添加返回 DasRetBase 的包装方法（void** -> IDasBase*）
%extend {qualified_interface} {{
    DasRetBase {java_method_name}({cpp_params_str}) {{
        DasRetBase result;
        result.error_code = $self->{cpp_method_name}({call_args_str});
        return result;
    }}
}}
""")
        # 对于 IDasReadOnlyString** 类型的 [out] 参数，需要特殊处理
        elif is_idas_readonly_string_out:
            call_args_with_temp = call_args.copy()
            call_args_with_temp.append("&p_out_string")
            call_args_str = ", ".join(call_args_with_temp)

            lines.append(f"""
// 添加返回 {ret_class_name} 的包装方法
%extend {qualified_interface} {{
    {ret_class_name} {java_method_name}({cpp_params_str}) {{
        {ret_class_name} result;
        IDasReadOnlyString* p_out_string = nullptr;
        result.error_code = $self->{cpp_method_name}({call_args_str});
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
// 添加返回 {ret_class_name} 的包装方法
%extend {qualified_interface} {{
    {ret_class_name} {java_method_name}({cpp_params_str}) {{
        {ret_class_name} result;
        result.error_code = $self->{cpp_method_name}({call_args_str});
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
                        namespace = self.get_type_namespace(p.type_info.base_type)
                        if namespace:
                            cpp_type = f"{namespace}::{p.type_info.base_type}*"
                        else:
                            cpp_type = f"{p.type_info.base_type}*"
                    elif p.type_info.is_pointer:
                        namespace = self.get_type_namespace(p.type_info.base_type)
                        if namespace:
                            cpp_type = f"{namespace}::{p.type_info.base_type}*"
                        else:
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
    {ret_class_name} {java_method_name}({das_cpp_params_str}) {{
        {ret_class_name} result;
        IDasReadOnlyString* p_out_string = nullptr;
        result.error_code = $self->{cpp_method_name}({das_call_args_str});
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
    {ret_class_name} {java_method_name}({das_cpp_params_str}) {{
        {ret_class_name} result;
        result.error_code = $self->{cpp_method_name}({das_call_args_str});
        return result;
    }}
}}
""")

        generated_code = "\n".join(lines)

        if self._context:
            typemap_key = f"{qualified_interface}::{method.name}"
            # 存储到 _global_typemaps 供 DasTypeMapsExtend.i 使用
            if typemap_key not in self._context._global_typemaps:
                self._context._global_typemaps[typemap_key] = generated_code
            
            # 生成并收集 DasRetXxx 类定义到 _global_ret_classes
            ret_class_code = self._generate_ret_class(out_param.type_info.base_type, interface.name)
            if ret_class_code:
                ret_class_key = f"ret_class_{ret_class_name}"
                if ret_class_key not in self._context._global_ret_classes:
                    self._context._global_ret_classes[ret_class_key] = ret_class_code

        return generated_code

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
