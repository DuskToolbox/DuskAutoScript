"""SWIG 异常抛出代码生成器

为 Java、Python、C# 等语言提供统一的异常抛出代码生成能力。

设计原则：
1. 统一模板：所有语言共享相同的异常信息结构
2. 语言特定：每种语言有自己的代码生成格式
3. 预留扩展：Python 和 C# 为预留接口，后续实现
4. 向后兼容：与现有 Java 生成器行为一致

异常信息结构：
- error_code: 错误码
- source_file: 源文件（如 "IDasTaskInfoVector.java"）
- source_line: 行号（通常为 0）
- source_function: 函数名（如 "EnumByIndexEz"）
- type_info_object: TypeInfo 对象（仅用于 createWithTypeInfo）
"""

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Optional, Tuple, Dict, Any
from abc import ABC, abstractmethod


class Language(Enum):
    """支持的语言"""
    JAVA = auto()
    PYTHON = auto()
    CSHARP = auto()


@dataclass(frozen=True)
class ExceptionContext:
    """异常上下文信息
    
    Attributes:
        interface_name: 接口名（如 "IDasTaskInfoVector"）
        method_name: 方法名（如 "EnumByIndex"）
        ez_method_name: Ez 便捷方法名（如 "EnumByIndexEz"）
        file_extension: 文件扩展名（如 ".java", ".py", ".cs"）
        inherits_type_info: 是否继承 IDasTypeInfo
        error_code_expr: 获取错误码的表达式（如 "result.getErrorCode()"）
        type_info_expr: TypeInfo 对象表达式（如 "this"）
    """
    interface_name: str
    method_name: str
    ez_method_name: str
    file_extension: str
    inherits_type_info: bool
    error_code_expr: str = "result.getErrorCode()"
    type_info_expr: str = "this"
    source_line: str = "0"
    
    @property
    def source_file(self) -> str:
        """生成源文件名"""
        return f"{self.interface_name}{self.file_extension}"
    
    @property
    def source_function(self) -> str:
        """生成源函数名"""
        return self.ez_method_name


@dataclass(frozen=True)
class ExceptionEmitterConfig:
    """异常发射器配置
    
    Attributes:
        language: 目标语言
        exception_type: 异常类名（如 "DasException"）
        create_method: 创建异常的方法名（如 "create"）
        create_with_type_info: 带 TypeInfo 的创建方法名（如 "createWithTypeInfo"）
        error_check_pattern: 错误检查表达式模板
        throw_pattern: 异常抛出语句模板
        indent: 缩进字符串
    """
    language: Language
    exception_type: str
    create_method: str
    create_with_type_info: str
    error_check_pattern: str
    throw_pattern: str
    indent: str = "    "
    
    def format_throw(self, context: ExceptionContext) -> str:
        """格式化异常抛出语句"""
        if context.inherits_type_info:
            exception_call = (
                f'{self.exception_type}.{self.create_with_type_info}('  
                f'{context.error_code_expr}, '
                f'"{context.source_file}", '
                f'{context.source_line}, '
                f'"{context.source_function}", '
                f'{context.type_info_expr})'
            )
        else:
            exception_call = (
                f'{self.exception_type}.{self.create_method}('
                f'{context.error_code_expr}, '
                f'"{context.source_file}", '
                f'{context.source_line}, '
                f'"{context.source_function}")'
            )
        return self.throw_pattern.format(exception=exception_call)
    
    def format_error_check(self, context: ExceptionContext, check_expr: Optional[str] = None) -> str:
        """格式化错误检查代码"""
        expr = check_expr or context.error_code_expr
        return self.error_check_pattern.format(error_code=expr)


# =============================================================================
# 预设配置
# =============================================================================

JAVA_CONFIG = ExceptionEmitterConfig(
    language=Language.JAVA,
    exception_type="DasException",
    create_method="create",
    create_with_type_info="createWithTypeInfo",
    error_check_pattern="DuskAutoScript.IsFailed({error_code})",
    throw_pattern="throw {exception};",
    indent="    "
)

PYTHON_CONFIG = ExceptionEmitterConfig(
    language=Language.PYTHON,
    exception_type="RuntimeError",  # 或 "DasException" 如果可用
    create_method="",  # 预留：Python 可能直接使用构造函数
    create_with_type_info="",  # 预留
    error_check_pattern="DasResult.IsFailed({error_code})",  # 预留
    throw_pattern="raise {exception}",
    indent="    "
)

CSHARP_CONFIG = ExceptionEmitterConfig(
    language=Language.CSHARP,
    exception_type="DasException",
    create_method="",  # 预留：可能直接使用构造函数
    create_with_type_info="",  # 预留
    error_check_pattern="DasResult.IsFailed({error_code})",  # 预留
    throw_pattern="throw {exception};",
    indent="    "
)


# =============================================================================
# 语言特定生成器基类
# =============================================================================

class BaseExceptionEmitter(ABC):
    """异常代码生成器基类"""
    
    def __init__(self, config: ExceptionEmitterConfig):
        self.config = config
    
    @abstractmethod
    def emit_exception_check(self, context: ExceptionContext) -> str:
        """生成异常检查代码块
        
        Args:
            context: 异常上下文信息
            
        Returns:
            完整的异常检查 if 语句块
        """
        pass
    
    @abstractmethod
    def emit_ez_method(
        self,
        context: ExceptionContext,
        return_type: str,
        params: List[Tuple[str, str]],  # [(type, name), ...]
        ret_class_name: str,
        call_args: List[str]
    ) -> str:
        """生成单返回值 Ez 便捷方法
        
        Args:
            context: 异常上下文信息
            return_type: Java/C#/Python 返回类型
            params: 参数列表 [(type, name), ...]
            ret_class_name: 返回包装类名（如 "DasRetInt"）
            call_args: 调用参数列表
            
        Returns:
            完整的 Ez 便捷方法代码
        """
        pass
    
    @abstractmethod
    def emit_ez_method_multi_out(
        self,
        context: ExceptionContext,
        out_params: List[Tuple[str, str]],  # [(type, name), ...]
        in_params: List[Tuple[str, str]],
        call_args: List[str]
    ) -> str:
        """生成多返回值 Ez 便捷方法
        
        Args:
            context: 异常上下文信息
            out_params: out 参数列表 [(type, name), ...]
            in_params: 输入参数列表 [(type, name), ...]
            call_args: 调用参数列表
            
        Returns:
            完整的多返回值 Ez 便捷方法代码
        """
        pass
    
    def emit_exception_throw(self, context: ExceptionContext) -> str:
        """生成异常抛出语句
        
        Args:
            context: 异常上下文信息
            
        Returns:
            抛出异常的代码行
        """
        return self.config.format_throw(context)
    
    def _make_docstring(self, context: ExceptionContext, description: Optional[str] = None) -> str:
        """生成文档字符串（语言特定）"""
        return ""


# =============================================================================
# Java 异常生成器
# =============================================================================

class JavaExceptionEmitter(BaseExceptionEmitter):
    """Java 异常代码生成器
    
    生成与现有 swig_java_generator.py 行为一致的 Java 代码。
    """
    
    def __init__(self):
        super().__init__(JAVA_CONFIG)
    
    def emit_exception_check(self, context: ExceptionContext) -> str:
        """生成 Java 异常检查代码块
        
        Example:
            if (DuskAutoScript.IsFailed(result.getErrorCode())) {
                throw DasException.create(error_code, "IDasTaskInfoVector.java", 0, "EnumByIndexEz");
            }
        """
        indent = self.config.indent
        check_expr = self.config.format_error_check(context)
        throw_stmt = self.emit_exception_throw(context)
        
        return f"""if ({check_expr}) {{
{indent}{throw_stmt}
}}"""
    
    def emit_ez_method(
        self,
        context: ExceptionContext,
        return_type: str,
        params: List[Tuple[str, str]],
        ret_class_name: str,
        call_args: List[str]
    ) -> str:
        """生成 Java 单返回值 Ez 便捷方法
        
        Example:
            public final int GetValueEz() throws DasException {
                DasRetInt result = GetValue();
                if (DuskAutoScript.IsFailed(result.getErrorCode())) {
                    throw DasException.create(...);
                }
                return result.getValue();
            }
        """
        indent = self.config.indent
        
        # 生成参数列表
        params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in params])
        call_args_str = ", ".join(call_args)
        
        # 生成异常检查代码
        exception_check = self.emit_exception_check(context)
        
        return f"""
    /**
     * {context.method_name} 的便捷方法（Ez 版本）
     * <p>
     * 接受 Java String 参数，失败时抛出 DasException。
     * </p>
     * @return 结果值
     * @throws DasException 当操作失败时
     */
    public final {return_type} {context.ez_method_name}({params_str}) throws DasException {{
        {ret_class_name} result = {context.method_name}({call_args_str});
        {exception_check.replace(chr(10), chr(10) + indent)}
        return result.getValue();
    }}"""
    
    def emit_ez_method_multi_out(
        self,
        context: ExceptionContext,
        out_params: List[Tuple[str, str]],
        in_params: List[Tuple[str, str]],
        call_args: List[str]
    ) -> str:
        """生成 Java 多返回值 Ez 便捷方法（预留实现）
        
        TODO: 多返回值方法的返回类型需要自定义结构，待设计
        """
        # 预留：多返回值方法需要自定义返回结构
        # 当前返回注释说明预留状态
        indent = self.config.indent
        
        params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in in_params])
        call_args_str = ", ".join(call_args)
        
        # 多返回值需要自定义返回类型，当前预留
        return_type = f"{context.interface_name}{context.method_name}Result"
        
        return f"""
    /**
     * {context.method_name} 的多返回值便捷方法（Ez 版本）
     * <p>
     * 返回多个值，失败时抛出 DasException。
     * </p>
     * @return 包含多个结果值的对象
     * @throws DasException 当操作失败时
     * @deprecated 多返回值方法正在设计中，当前为预留实现
     */
    @Deprecated
    public final {return_type} {context.ez_method_name}({params_str}) throws DasException {{
        // TODO: 多返回值方法需要自定义返回结构
        // out_params: {out_params}
        throw new UnsupportedOperationException("多返回值 Ez 方法尚未实现");
    }}"""


# =============================================================================
# Python 异常生成器（预留接口）
# =============================================================================

class PythonExceptionEmitter(BaseExceptionEmitter):
    """Python 异常代码生成器（预留接口）
    
    当前为预留实现，后续根据 Python 生成器需求完善。
    """
    
    def __init__(self):
        super().__init__(PYTHON_CONFIG)
    
    def emit_exception_check(self, context: ExceptionContext) -> str:
        """预留：Python 异常检查代码块"""
        # 预留实现：Python 可能使用不同的错误检查模式
        return f"""# TODO: Python 异常检查
if DasResult.IsFailed({context.error_code_expr}):
    raise RuntimeError(f"[{{{context.error_code_expr}}}] {context.source_file}:{context.source_line} {context.source_function}")"""
    
    def emit_ez_method(
        self,
        context: ExceptionContext,
        return_type: str,
        params: List[Tuple[str, str]],
        ret_class_name: str,
        call_args: List[str]
    ) -> str:
        """预留：Python 单返回值 Ez 便捷方法"""
        # 预留实现
        params_str = ", ".join([pname for _, pname in params])
        call_args_str = ", ".join(call_args)
        
        return f"""
    def {context.ez_method_name}(self{', ' + params_str if params else ''}):
        \"\"\"
        {context.method_name} 的便捷方法（Ez 版本）
        
        Returns:
            结果值
        Raises:
            RuntimeError: 当操作失败时
        \"\"\"
        result = self.{context.method_name}({call_args_str})
        if DasResult.IsFailed(result.error_code):
            raise RuntimeError(f"[{{result.error_code}}] {context.source_file}:{context.source_line} {context.source_function}")
        return result.value
"""
    
    def emit_ez_method_multi_out(
        self,
        context: ExceptionContext,
        out_params: List[Tuple[str, str]],
        in_params: List[Tuple[str, str]],
        call_args: List[str]
    ) -> str:
        """预留：Python 多返回值 Ez 便捷方法"""
        params_str = ", ".join([pname for _, pname in in_params])
        
        return f"""
    def {context.ez_method_name}(self{', ' + params_str if in_params else ''}):
        \"\"\"
        {context.method_name} 的多返回值便捷方法（Ez 版本）
        
        Returns:
            tuple: 包含多个结果值的元组
        Raises:
            RuntimeError: 当操作失败时
        \"\"\"
        # TODO: 多返回值方法需要自定义返回结构
        raise NotImplementedError("多返回值 Ez 方法尚未实现")
"""


# =============================================================================
# C# 异常生成器（预留接口）
# =============================================================================

class CSharpExceptionEmitter(BaseExceptionEmitter):
    """C# 异常代码生成器（预留接口）
    
    当前为预留实现，后续根据 C# 生成器需求完善。
    """
    
    def __init__(self):
        super().__init__(CSHARP_CONFIG)
    
    def emit_exception_check(self, context: ExceptionContext) -> str:
        """预留：C# 异常检查代码块"""
        indent = self.config.indent
        return f"""if (DasResult.IsFailed({context.error_code_expr}))
{{
{indent}throw new DasException({context.error_code_expr}, "{context.source_file}", {context.source_line}, "{context.source_function}");
}}"""
    
    def emit_ez_method(
        self,
        context: ExceptionContext,
        return_type: str,
        params: List[Tuple[str, str]],
        ret_class_name: str,
        call_args: List[str]
    ) -> str:
        """预留：C# 单返回值 Ez 便捷方法"""
        indent = self.config.indent
        params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in params])
        call_args_str = ", ".join(call_args)
        
        return f"""
    /// <summary>
    /// {context.method_name} 的便捷方法（Ez 版本）
    /// </summary>
    /// <returns>结果值</returns>
    /// <exception cref="DasException">当操作失败时</exception>
    public {return_type} {context.ez_method_name}({params_str})
    {{
        var result = {context.method_name}({call_args_str});
        if (DasResult.IsFailed(result.ErrorCode))
        {{
{indent}throw new DasException(result.ErrorCode, "{context.source_file}", {context.source_line}, "{context.source_function}");
{indent}}}
        return result.Value;
    }}
"""
    
    def emit_ez_method_multi_out(
        self,
        context: ExceptionContext,
        out_params: List[Tuple[str, str]],
        in_params: List[Tuple[str, str]],
        call_args: List[str]
    ) -> str:
        """预留：C# 多返回值 Ez 便捷方法"""
        indent = self.config.indent
        params_str = ", ".join([f"{ptype} {pname}" for ptype, pname in in_params])
        
        return f"""
    /// <summary>
    /// {context.method_name} 的多返回值便捷方法（Ez 版本）
    /// </summary>
    /// <returns>包含多个结果值的对象</returns>
    /// <exception cref="DasException">当操作失败时</exception>
    [Obsolete("多返回值 Ez 方法尚未实现")]
    public object {context.ez_method_name}({params_str})
    {{
        // TODO: 多返回值方法需要自定义返回结构
        throw new NotImplementedException("多返回值 Ez 方法尚未实现");
    }}
"""


# =============================================================================
# 工厂函数
# =============================================================================

def create_exception_emitter(language: Language) -> BaseExceptionEmitter:
    """创建语言特定的异常代码生成器
    
    Args:
        language: 目标语言
        
    Returns:
        对应的异常代码生成器实例
        
    Example:
        >>> emitter = create_exception_emitter(Language.JAVA)
        >>> context = ExceptionContext(
        ...     interface_name="IDasTaskInfoVector",
        ...     method_name="EnumByIndex",
        ...     ez_method_name="EnumByIndexEz",
        ...     file_extension=".java",
        ...     inherits_type_info=False
        ... )
        >>> code = emitter.emit_exception_check(context)
    """
    emitters = {
        Language.JAVA: JavaExceptionEmitter,
        Language.PYTHON: PythonExceptionEmitter,
        Language.CSHARP: CSharpExceptionEmitter,
    }
    
    if language not in emitters:
        raise ValueError(f"不支持的语言: {language}")
    
    return emitters[language]()


# =============================================================================
# 便捷函数
# =============================================================================

def emit_java_ez_method(
    interface_name: str,
    method_name: str,
    return_type: str,
    params: List[Tuple[str, str]],
    ret_class_name: str,
    inherits_type_info: bool = False
) -> str:
    """便捷函数：生成 Java Ez 便捷方法
    
    Args:
        interface_name: 接口名
        method_name: 方法名
        return_type: Java 返回类型
        params: 参数列表 [(type, name), ...]
        ret_class_name: 返回包装类名
        inherits_type_info: 是否继承 IDasTypeInfo
        
    Returns:
        完整的 Java Ez 便捷方法代码
    """
    context = ExceptionContext(
        interface_name=interface_name,
        method_name=method_name,
        ez_method_name=f"{method_name}Ez",
        file_extension=".java",
        inherits_type_info=inherits_type_info
    )
    
    call_args = [pname for _, pname in params]
    emitter = create_exception_emitter(Language.JAVA)
    
    return emitter.emit_ez_method(
        context=context,
        return_type=return_type,
        params=params,
        ret_class_name=ret_class_name,
        call_args=call_args
    )


def emit_python_ez_method(
    interface_name: str,
    method_name: str,
    params: List[Tuple[str, str]],
    inherits_type_info: bool = False
) -> str:
    """便捷函数：生成 Python Ez 便捷方法（预留接口）
    
    Args:
        interface_name: 接口名
        method_name: 方法名
        params: 参数列表 [(type, name), ...]
        inherits_type_info: 是否继承 IDasTypeInfo
        
    Returns:
        完整的 Python Ez 便捷方法代码
    """
    context = ExceptionContext(
        interface_name=interface_name,
        method_name=method_name,
        ez_method_name=f"{method_name}_ez",
        file_extension=".py",
        inherits_type_info=inherits_type_info
    )
    
    call_args = [pname for _, pname in params]
    emitter = create_exception_emitter(Language.PYTHON)
    
    return emitter.emit_ez_method(
        context=context,
        return_type="Any",  # Python 动态类型
        params=params,
        ret_class_name="DasRet",  # Python 简化
        call_args=call_args
    )


def emit_csharp_ez_method(
    interface_name: str,
    method_name: str,
    return_type: str,
    params: List[Tuple[str, str]],
    ret_class_name: str,
    inherits_type_info: bool = False
) -> str:
    """便捷函数：生成 C# Ez 便捷方法（预留接口）
    
    Args:
        interface_name: 接口名
        method_name: 方法名
        return_type: C# 返回类型
        params: 参数列表 [(type, name), ...]
        ret_class_name: 返回包装类名
        inherits_type_info: 是否继承 IDasTypeInfo
        
    Returns:
        完整的 C# Ez 便捷方法代码
    """
    context = ExceptionContext(
        interface_name=interface_name,
        method_name=method_name,
        ez_method_name=f"{method_name}Ez",
        file_extension=".cs",
        inherits_type_info=inherits_type_info
    )
    
    call_args = [pname for _, pname in params]
    emitter = create_exception_emitter(Language.CSHARP)
    
    return emitter.emit_ez_method(
        context=context,
        return_type=return_type,
        params=params,
        ret_class_name=ret_class_name,
        call_args=call_args
    )


# 导出公共 API
__all__ = [
    # 枚举和配置
    'Language',
    'ExceptionContext',
    'ExceptionEmitterConfig',
    'JAVA_CONFIG',
    'PYTHON_CONFIG',
    'CSHARP_CONFIG',
    # 生成器类
    'BaseExceptionEmitter',
    'JavaExceptionEmitter',
    'PythonExceptionEmitter',
    'CSharpExceptionEmitter',
    # 工厂函数
    'create_exception_emitter',
    # 便捷函数
    'emit_java_ez_method',
    'emit_python_ez_method',
    'emit_csharp_ez_method',
]
