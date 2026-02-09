"""
SWIG API 分析模型

为 Java/Python/C# 生成器提供统一的接口分析模型。
将 IDL 解析结果转换为语言生成器友好的数据结构。

设计原则：
1. 只包含 Java 生成器当前已用到的信息
2. 避免"上帝对象"，保持最小可用字段
3. 支持递归继承链解析（带循环检测和最大深度限制）
4. 模型可哈希（frozen=True），可用作字典键
"""

from dataclasses import dataclass, field
from typing import List, Optional, Set, Dict, Tuple, Any
from das_idl_parser import (
    InterfaceDef,
    MethodDef,
    ParameterDef,
    ParamDirection,
    TypeInfo,
)


@dataclass(frozen=True)
class OutParamInfo:
    """
    Out 参数方法信息
    
    用于描述带 [out] 参数的方法，包括：
    - 方法名
    - out 参数类型（第一个 out 参数的类型，用于向后兼容）
    - 所有 out 参数类型列表（用于多 out 方法生成准确返回类型）
    - 输入参数列表
    - 是否多 out 参数
    """
    method_name: str
    out_param_type: str
    in_params: Tuple[Tuple[str, str], ...]  # ((type, name), ...) - 元组用于哈希
    is_multi_out: bool = False
    all_out_params: Tuple[str, ...] = ()
    
    def __post_init__(self):
        # 确保 in_params 是元组（用于不可变性）
        if not isinstance(self.in_params, tuple):
            object.__setattr__(
                self, 
                'in_params', 
                tuple((str(t), str(n)) for t, n in self.in_params)
            )


@dataclass(frozen=True)
class SwigInterfaceModel:
    """
    SWIG 接口分析模型
    
    包含语言生成器需要的所有接口元数据：
    - 基本信息：名称、命名空间、父接口
    - 继承链信息：是否继承 IDasTypeInfo
    - 方法分类：out 参数、binary_buffer、字符串参数等
    """
    # 基本信息
    name: str
    namespace: str
    base_interface: str
    
    # 继承链分析结果
    inherits_idas_type_info: bool
    
    # 方法分类
    out_methods: Tuple[OutParamInfo, ...]  # 单 out 参数方法
    multi_out_methods: Tuple[OutParamInfo, ...]  # 多 out 参数方法
    binary_buffer_methods: Tuple[str, ...]  # 方法名列表
    string_param_methods: Tuple[str, ...]  # 含 IDasReadOnlyString* 参数的方法
    
    def __post_init__(self):
        # 确保所有列表字段都是元组（用于不可变性）
        for field_name in ['out_methods', 'multi_out_methods', 'binary_buffer_methods', 'string_param_methods']:
            value = getattr(self, field_name)
            if not isinstance(value, tuple):
                object.__setattr__(self, field_name, tuple(value))
    
    @property
    def qualified_name(self) -> str:
        """获取完整限定名（namespace::name）"""
        if self.namespace:
            return f"{self.namespace}::{self.name}"
        return self.name
    
    def get_all_out_methods(self) -> List[OutParamInfo]:
        """获取所有 out 参数方法（单 out + 多 out）"""
        return list(self.out_methods) + list(self.multi_out_methods)


# ============================================================================
# 继承链解析
# ============================================================================

MAX_INHERIT_DEPTH = 20  # 最大继承深度，防止循环继承导致的无限递归


def check_inherits_idas_type_info(
    interface_name: str,
    interface_map: Dict[str, InterfaceDef],
    visited: Optional[Set[str]] = None,
    depth: int = 0
) -> bool:
    """
    递归检查接口是否继承自 IDasTypeInfo
    
    Args:
        interface_name: 要检查的接口名
        interface_map: 接口名到 InterfaceDef 的映射字典
        visited: 已访问的接口集合（用于循环检测）
        depth: 当前递归深度
        
    Returns:
        如果继承自 IDasTypeInfo（直接或通过继承链），返回 True
        
    Raises:
        RecursionError: 如果检测到循环继承或超过最大深度
    """
    if depth > MAX_INHERIT_DEPTH:
        raise RecursionError(
            f"继承链深度超过最大限制 {MAX_INHERIT_DEPTH}，"
            f"可能存在循环继承。当前接口: {interface_name}"
        )
    
    if visited is None:
        visited = set()
    
    # 循环检测
    if interface_name in visited:
        raise RecursionError(
            f"检测到循环继承: {interface_name} 已在继承链中。"
            f"继承链: {' -> '.join(visited)}"
        )
    
    # 基本情况：直接是 IDasTypeInfo
    if interface_name == 'IDasTypeInfo':
        return True
    
    # 基本情况：IDasBase 不是 IDasTypeInfo
    if interface_name == 'IDasBase':
        return False
    
    # 查找接口定义
    iface_def = interface_map.get(interface_name)
    if iface_def is None:
        # 接口未定义，假设不继承 IDasTypeInfo
        return False
    
    # 标记为已访问
    visited.add(interface_name)
    
    try:
        # 递归检查父接口
        base_interface = iface_def.base_interface
        if base_interface and base_interface != interface_name:
            return check_inherits_idas_type_info(
                base_interface,
                interface_map,
                visited.copy(),  # 传递副本，避免不同分支互相影响
                depth + 1
            )
        return False
    finally:
        # 不需要手动移除 visited，因为我们传递了副本
        pass


def build_interface_map(interfaces: List[InterfaceDef]) -> Dict[str, InterfaceDef]:
    """
    构建接口名到 InterfaceDef 的映射字典
    
    Args:
        interfaces: InterfaceDef 列表
        
    Returns:
        接口名到 InterfaceDef 的映射字典（使用 qualified_name 作为键）
    """
    result: Dict[str, InterfaceDef] = {}
    for iface in interfaces:
        # 使用简单名称作为键
        result[iface.name] = iface
        # 同时添加完整限定名
        if iface.namespace:
            qualified = f"{iface.namespace}::{iface.name}"
            result[qualified] = iface
    return result


# ============================================================================
# 方法分类器
# ============================================================================

def is_binary_buffer_method(method: MethodDef) -> bool:
    """
    判断方法是否有 [binary_buffer] 标记
    
    Args:
        method: 方法定义
        
    Returns:
        如果有 [binary_buffer] 标记返回 True
    """
    return method.attributes.get('binary_buffer', False) if method.attributes else False


def is_interface_type(type_name: str) -> bool:
    """
    判断是否是接口类型（以 I 开头，后跟大写字母）
    
    Args:
        type_name: 类型名称
        
    Returns:
        如果是接口类型返回 True
    """
    simple_name = type_name.split('::')[-1]
    return simple_name.startswith('I') and len(simple_name) > 1 and simple_name[1:2].isupper()


def has_idas_readonly_string_param(method: MethodDef) -> bool:
    """
    检查方法是否有 IDasReadOnlyString* 类型的输入参数
    
    Args:
        method: 方法定义
        
    Returns:
        如果有 IDasReadOnlyString* 输入参数返回 True
    """
    for param in method.parameters:
        if param.direction == ParamDirection.IN:
            if param.type_info.base_type == 'IDasReadOnlyString':
                return True
    return False


def is_property_getter_method(method: MethodDef, interface: InterfaceDef) -> bool:
    """
    判断方法是否是属性 getter（通过 interface.properties 列表判断）
    
    Args:
        method: 方法定义
        interface: 接口定义
        
    Returns:
        如果是属性 getter 返回 True
    """
    for prop in interface.properties:
        if not prop.has_getter:
            continue
        if method.name == f"Get{prop.name}":
            return True
    return False


def classify_out_methods(
    interface: InterfaceDef,
    include_properties: bool = True
) -> Tuple[List[OutParamInfo], List[OutParamInfo]]:
    """
    分类接口中的 out 参数方法
    
    将 out 参数方法分为：
    1. 单 out 参数方法（out_methods）
    2. 多 out 参数方法（multi_out_methods）
    
    Args:
        interface: 接口定义
        include_properties: 是否包含属性生成的虚拟 getter 方法
        
    Returns:
        (单 out 方法列表, 多 out 方法列表)
    """
    single_out: List[OutParamInfo] = []
    multi_out: List[OutParamInfo] = []
    processed_methods: set = set()
    
    # 处理显式定义的方法
    for method in interface.methods:
        # 跳过 binary_buffer 方法
        if is_binary_buffer_method(method):
            continue
        
        # 收集 out 参数
        out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
        
        if len(out_params) == 1:
            # 单 out 参数
            out_param = out_params[0]
            in_params = [
                (p.type_info.base_type, p.name)
                for p in method.parameters
                if p.direction != ParamDirection.OUT
            ]
            
            single_out.append(OutParamInfo(
                method_name=method.name,
                out_param_type=out_param.type_info.base_type,
                in_params=tuple(in_params),
                is_multi_out=False
            ))
            processed_methods.add(method.name)
            
        elif len(out_params) >= 2:
            # 多 out 参数
            # 使用第一个 out 参数的类型作为主要返回类型
            out_param = out_params[0]
            in_params = [
                (p.type_info.base_type, p.name)
                for p in method.parameters
                if p.direction != ParamDirection.OUT
            ]
            # 收集所有 out 参数类型
            all_out_types = tuple(p.type_info.base_type for p in out_params)
            
            multi_out.append(OutParamInfo(
                method_name=method.name,
                out_param_type=out_param.type_info.base_type,
                in_params=tuple(in_params),
                is_multi_out=True,
                all_out_params=all_out_types
            ))
            processed_methods.add(method.name)
    
    # 处理属性 getter（如果需要）
    if include_properties:
        for prop in interface.properties:
            if not prop.has_getter:
                continue
            
            method_name = f"Get{prop.name}"
            if method_name in processed_methods:
                continue  # 已处理过
            
            # 确定属性 getter 的参数类型
            base_type = prop.type_info.base_type
            is_interface = is_interface_type(base_type)
            is_string = base_type == 'IDasReadOnlyString'
            
            if is_interface or is_string:
                out_type = base_type if is_interface else 'IDasReadOnlyString'
            else:
                out_type = base_type
            
            single_out.append(OutParamInfo(
                method_name=method_name,
                out_param_type=out_type,
                in_params=tuple(),  # 属性 getter 没有输入参数
                is_multi_out=False
            ))
    
    return single_out, multi_out


def classify_binary_buffer_methods(interface: InterfaceDef) -> List[str]:
    """
    获取所有 binary_buffer 标记的方法名
    
    Args:
        interface: 接口定义
        
    Returns:
        binary_buffer 方法名列表
    """
    result: List[str] = []
    for method in interface.methods:
        if is_binary_buffer_method(method):
            result.append(method.name)
    return result


def classify_string_param_methods(interface: InterfaceDef) -> List[str]:
    """
    获取所有带 IDasReadOnlyString* 输入参数的方法名
    
    包括：
    1. 显式方法中不带 out 参数但有 IDasReadOnlyString* 输入参数的方法
    2. 属性 setter 中类型为 IDasReadOnlyString 的方法
    
    Args:
        interface: 接口定义
        
    Returns:
        方法名列表
    """
    result: List[str] = []
    processed: set = set()
    
    # 处理显式方法
    for method in interface.methods:
        # 跳过 binary_buffer 方法
        if is_binary_buffer_method(method):
            continue
        
        # 跳过有 out 参数的方法
        out_params = [p for p in method.parameters if p.direction == ParamDirection.OUT]
        if out_params:
            continue
        
        # 检查是否有 IDasReadOnlyString* 输入参数
        if has_idas_readonly_string_param(method):
            result.append(method.name)
            processed.add(method.name)
    
    # 处理属性 setter
    for prop in interface.properties:
        if not prop.has_setter:
            continue
        if prop.type_info.base_type != 'IDasReadOnlyString':
            continue
        
        method_name = f"Set{prop.name}"
        if method_name in processed:
            continue
        
        result.append(method_name)
    
    return result


# ============================================================================
# 模型构建器
# ============================================================================

def build_swig_interface_model(
    interface: InterfaceDef,
    interface_map: Optional[Dict[str, InterfaceDef]] = None
) -> SwigInterfaceModel:
    """
    从 InterfaceDef 构建 SwigInterfaceModel
    
    Args:
        interface: 接口定义
        interface_map: 接口映射字典（用于继承链解析）
        
    Returns:
        SwigInterfaceModel 实例
    """
    # 解析继承链
    if interface_map:
        try:
            inherits_type_info = check_inherits_idas_type_info(
                interface.name,
                interface_map
            )
        except RecursionError:
            # 继承链解析失败，保守起见假设不继承
            inherits_type_info = False
    else:
        # 没有接口映射，只能做简单判断
        inherits_type_info = interface.base_interface == 'IDasTypeInfo'
    
    # 分类方法
    out_methods, multi_out_methods = classify_out_methods(interface)
    binary_buffer_methods = classify_binary_buffer_methods(interface)
    string_param_methods = classify_string_param_methods(interface)
    
    return SwigInterfaceModel(
        name=interface.name,
        namespace=interface.namespace,
        base_interface=interface.base_interface,
        inherits_idas_type_info=inherits_type_info,
        out_methods=tuple(out_methods),
        multi_out_methods=tuple(multi_out_methods),
        binary_buffer_methods=tuple(binary_buffer_methods),
        string_param_methods=tuple(string_param_methods)
    )


def build_all_models(
    interfaces: List[InterfaceDef]
) -> Dict[str, SwigInterfaceModel]:
    """
    为所有接口构建 SwigInterfaceModel
    
    Args:
        interfaces: 接口定义列表
        
    Returns:
        接口名到 SwigInterfaceModel 的映射字典
    """
    interface_map = build_interface_map(interfaces)
    result: Dict[str, SwigInterfaceModel] = {}
    
    for iface in interfaces:
        model = build_swig_interface_model(iface, interface_map)
        result[iface.name] = model
        # 同时添加完整限定名
        if iface.namespace:
            qualified = f"{iface.namespace}::{iface.name}"
            result[qualified] = model
    
    return result


# ============================================================================
# 测试代码
# ============================================================================

if __name__ == '__main__':
    # 创建测试 IDL
    test_idl = '''
    namespace DAS {
        // 测试接口 - 继承 IDasBase
        [uuid("12345678-1234-1234-1234-123456789abc")]
        interface IDasTest : IDasBase {
            // out 参数方法
            DasResult GetValue([out] int32* p_out);
            DasResult GetInterface([out] IDasTestResult** pp_out);
            
            // 多 out 参数方法
            DasResult GetMultiple([out] int32* p_a, [out] int32* p_b);
            
            // binary_buffer 方法
            [binary_buffer] DasResult GetBuffer([out] unsigned char** pp_buffer, [out] size_t* p_size);
            
            // 字符串参数方法
            DasResult SetName(IDasReadOnlyString* p_name);
            
            // 普通方法
            DasResult DoSomething(int32 value);
        }
        
        // 测试接口 - 继承 IDasTypeInfo
        [uuid("87654321-4321-4321-4321-cba987654321")]
        interface IDasTypeInfoTest : IDasTypeInfo {
            [get] int32 Id
            [set] IDasReadOnlyString Name
        }
    }
    '''
    
    from das_idl_parser import parse_idl
    
    print("=" * 60)
    print("SWIG API Model 测试")
    print("=" * 60)
    
    # 解析 IDL
    doc = parse_idl(test_idl)
    print(f"\n解析到 {len(doc.interfaces)} 个接口")
    
    # 构建接口映射
    interface_map = build_interface_map(doc.interfaces)
    print(f"构建接口映射: {list(interface_map.keys())}")
    
    # 为每个接口构建模型
    for iface in doc.interfaces:
        print(f"\n{'='*60}")
        print(f"接口: {iface.name}")
        print(f"父接口: {iface.base_interface}")
        print(f"命名空间: {iface.namespace or '(无)'}")
        
        # 构建模型
        model = build_swig_interface_model(iface, interface_map)
        
        print(f"\n模型信息:")
        print(f"  完整名: {model.qualified_name}")
        print(f"  继承 IDasTypeInfo: {model.inherits_idas_type_info}")
        print(f"  单 out 方法数: {len(model.out_methods)}")
        for m in model.out_methods:
            print(f"    - {m.method_name}: returns {m.out_param_type}")
        
        print(f"  多 out 方法数: {len(model.multi_out_methods)}")
        for m in model.multi_out_methods:
            print(f"    - {m.method_name}: returns {m.out_param_type}")
        
        print(f"  binary_buffer 方法数: {len(model.binary_buffer_methods)}")
        for name in model.binary_buffer_methods:
            print(f"    - {name}")
        
        print(f"  字符串参数方法数: {len(model.string_param_methods)}")
        for name in model.string_param_methods:
            print(f"    - {name}")
        
        # 测试可哈希性
        print(f"\n可哈希性测试:")
        try:
            model_set = {model}
            print(f"  可作为集合元素: OK")
        except TypeError as e:
            print(f"  可作为集合元素: FAIL ({e})")
        
        try:
            model_dict = {model: "test"}
            print(f"  可作为字典键: OK")
        except TypeError as e:
            print(f"  可作为字典键: FAIL ({e})")
    
    # 测试继承链检查
    print(f"\n{'='*60}")
    print("继承链检查测试")
    print(f"{'='*60}")
    
    # 创建模拟的 IDasTypeInfo 接口
    idas_type_info = InterfaceDef(
        name="IDasTypeInfo",
        uuid="00000000-0000-0000-0000-000000000000",
        base_interface="IDasBase"
    )
    
    # 创建循环继承测试
    circular_a = InterfaceDef(
        name="ICircularA",
        uuid="11111111-1111-1111-1111-111111111111",
        base_interface="ICircularB"
    )
    circular_b = InterfaceDef(
        name="ICircularB",
        uuid="22222222-2222-2222-2222-222222222222",
        base_interface="ICircularA"
    )
    
    test_interfaces = doc.interfaces + [idas_type_info, circular_a, circular_b]
    test_map = build_interface_map(test_interfaces)
    
    # 测试正常继承
    print("\n正常继承检查:")
    result = check_inherits_idas_type_info("IDasTest", test_map)
    print(f"  IDasTest 继承 IDasTypeInfo: {result} (期望: False)")
    
    result = check_inherits_idas_type_info("IDasTypeInfoTest", test_map)
    print(f"  IDasTypeInfoTest 继承 IDasTypeInfo: {result} (期望: True)")
    
    # 测试循环继承检测
    print("\n循环继承检测:")
    try:
        result = check_inherits_idas_type_info("ICircularA", test_map)
        print(f"  ICircularA 检查: 未检测到循环 (结果: {result})")
    except RecursionError as e:
        print(f"  ICircularA 检查: OK 检测到循环继承")
        print(f"    错误信息: {e}")
    
    print("\n" + "=" * 60)
    print("测试完成")
    print("=" * 60)
