"""
SWIG 语言生成器基类

定义了语言特定生成器的接口，用于将不同语言的特定逻辑从主生成器中分离
"""

from abc import ABC, abstractmethod
from typing import Callable, Optional, TYPE_CHECKING
from das_idl_parser import InterfaceDef, MethodDef, ParameterDef

if TYPE_CHECKING:
    from swig_api_model import SwigInterfaceModel


class SwigLangGeneratorContext:
    """SWIG 语言生成器上下文
    
    提供语言生成器需要的外部信息，如类型命名空间查找等
    """
    
    def __init__(self, get_interface_namespace_func: Optional[Callable[[str], Optional[str]]] = None, get_interface_idl_file_func: Optional[Callable[[str], Optional[str]]] = None, get_enum_idl_file_func: Optional[Callable[[str], Optional[str]]] = None, global_ret_classes: Optional[dict] = None, global_typemaps: Optional[dict] = None, global_header_blocks: Optional[dict] = None, global_typemaps_ignore: Optional[dict] = None):
        self._get_interface_namespace = get_interface_namespace_func
        self._get_interface_idl_file = get_interface_idl_file_func
        self._get_enum_idl_file = get_enum_idl_file_func
        self._global_ret_classes = global_ret_classes if global_ret_classes is not None else {}
        self._global_typemaps = global_typemaps if global_typemaps is not None else {}
        self._global_header_blocks = global_header_blocks if global_header_blocks is not None else {}
        self._global_typemaps_ignore = global_typemaps_ignore if global_typemaps_ignore is not None else {}
    
    def get_type_namespace(self, type_name: str) -> Optional[str]:
        """获取类型的命名空间
        
        Args:
            type_name: 类型名称（可以是简单名或完全限定名）
            
        Returns:
            命名空间字符串，如果未找到则返回 None
        """
        if self._get_interface_namespace:
            return self._get_interface_namespace(type_name)
        return None
    
    def get_interface_idl_file(self, type_name: str) -> Optional[str]:
        if self._get_interface_idl_file:
            return self._get_interface_idl_file(type_name)
        return None
    
    def get_enum_idl_file(self, type_name: str) -> Optional[str]:
        if self._get_enum_idl_file:
            return self._get_enum_idl_file(type_name)
        return None


class SwigLangGenerator(ABC):
    """SWIG 语言生成器基类"""
    
    def __init__(self):
        self._context: Optional[SwigLangGeneratorContext] = None
        self.debug: bool = False

    def set_context(self, context: SwigLangGeneratorContext) -> None:
        """设置生成器上下文"""
        self._context = context
    
    def get_type_namespace(self, type_name: str) -> Optional[str]:
        if self._context:
            return self._context.get_type_namespace(type_name)
        return None
    
    def get_interface_idl_file(self, type_name: str) -> Optional[str]:
        if self._context:
            return self._context.get_interface_idl_file(type_name)
        return None
    
    def get_enum_idl_file(self, type_name: str) -> Optional[str]:
        if self._context:
            return self._context.get_enum_idl_file(type_name)
        return None
    
    @abstractmethod
    def get_language_name(self) -> str:
        """获取语言名称（如 'java', 'csharp', 'python'）"""
        pass

    @abstractmethod
    def get_swig_define(self) -> str:
        """获取 SWIG 宏定义（如 'SWIGJAVA', 'SWIGCSHARP', 'SWIGPYTHON'）"""
        pass

    def handles_out_param_completely(self) -> bool:
        """声明此语言生成器是否完全处理 [out] 参数
        
        如果返回 True，表示此语言生成器通过 generate_pre_include_directives 
        完全处理了 [out] 参数（例如使用 %ignore + %extend 替换原始方法），
        主生成器将跳过为该语言生成通用的 typemap。
        
        如果返回 False（默认），主生成器会生成通用的 typemap，
        并调用 generate_out_param_wrapper 生成语言特定的补充代码。
        
        Returns:
            True 如果完全处理 [out] 参数，False 否则
        """
        return False

    @abstractmethod
    def generate_out_param_wrapper(self, interface: InterfaceDef, method: MethodDef, param: ParameterDef) -> str:
        """生成 [out] 参数的语言特定包装代码

        返回 SWIG .i 文件中的代码片段
        注意：此方法生成的代码会放在 %include 之后
        
        如果 handles_out_param_completely() 返回 True，此方法可能不会被调用。
        """
        pass

    def generate_pre_include_directives(self, interface: InterfaceDef) -> str:
        """生成必须在 %include 之前的 SWIG 指令
        
        例如 Java 的 %rename、%javamethodmodifiers 等指令必须在 %include 之前
        才能正确应用到目标方法上。
        
        如果 handles_out_param_completely() 返回 True，此方法应该生成完整的
        [out] 参数处理代码（包括 %ignore、%extend、DasRetXxx 类定义等）。
        
        Args:
            interface: 接口定义
            
        Returns:
            SWIG .i 文件中的代码片段，将插入到 %include 指令之前
        """
        return ""  # 默认实现返回空字符串
    
    def generate_post_include_directives(self, interface: InterfaceDef) -> str:
        """生成在 %include 之后的 SWIG 指令
        
        某些 typemap（如 javacode）需要在 %include 之后生成，
        以避免在类的前置声明时被过早应用。
        
        Args:
            interface: 接口定义
            
        Returns:
            SWIG .i 文件中的代码片段，将插入到 %include 指令之后
        """
        return ""  # 默认实现返回空字符串

    @abstractmethod
    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
        """生成二进制缓冲区辅助方法的代码

        Args:
            interface: 接口定义
            method_name: 获取数据的方法名（如 GetData），如果不提供则为空字符串
            size_method_name: 获取大小的方法名（如 GetSize）

        Returns:
            SWIG .i 文件中的代码片段
        """
        pass

    def on_interface_model(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> None:
        """处理接口分析模型
        
        当主生成器构建好 SwigInterfaceModel 后，会调用此方法让语言生成器
        有机会消费模型数据。默认实现为空，子类可以覆盖此方法。
        
        Args:
            model: 接口分析模型（包含分类好的方法信息等）
            interface_def: 原始的接口定义
        """
        pass

    def emit_post_include(self, model: "SwigInterfaceModel", interface_def: InterfaceDef) -> str:
        """生成后置包含指令（Ez方法）
        
        在生成 %include 指令之后调用，用于生成语言特定的后置代码。
        与 generate_post_include_directives 的区别是此方法接收 SwigInterfaceModel，
        包含了预分析好的接口元数据。
        
        Args:
            model: 接口分析模型
            interface_def: 原始的接口定义
            
        Returns:
            SWIG .i 文件中的代码片段，将插入到 %include 指令之后
        """
        return ""
