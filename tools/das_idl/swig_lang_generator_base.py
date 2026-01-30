"""
SWIG 语言生成器基类

定义了语言特定生成器的接口，用于将不同语言的特定逻辑从主生成器中分离
"""

from abc import ABC, abstractmethod
from typing import List
from das_idl_parser import InterfaceDef, MethodDef, ParameterDef


class SwigLangGenerator(ABC):
    """SWIG 语言生成器基类"""

    @abstractmethod
    def get_language_name(self) -> str:
        """获取语言名称（如 'java', 'csharp', 'python'）"""
        pass

    @abstractmethod
    def get_swig_define(self) -> str:
        """获取 SWIG 宏定义（如 'SWIGJAVA', 'SWIGCSHARP', 'SWIGPYTHON'）"""
        pass

    @abstractmethod
    def generate_out_param_wrapper(self, interface: InterfaceDef, method: MethodDef, param: ParameterDef) -> str:
        """生成 [out] 参数的语言特定包装代码

        返回 SWIG .i 文件中的代码片段
        """
        pass

    @abstractmethod
    def generate_binary_buffer_helpers(self, interface: InterfaceDef, method_name: str, size_method_name: str) -> str:
        """生成二进制缓冲区辅助方法的代码

        Args:
            interface: 接口定义
            method_name: 获取数据的方法名（如 GetData）
            size_method_name: 获取大小的方法名（如 GetSize）

        Returns:
            SWIG .i 文件中的代码片段
        """
        pass
