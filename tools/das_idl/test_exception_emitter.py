#!/usr/bin/env python3
"""
测试 swig_exception_emitter 模块

验证：
1. ExceptionContext 正确生成源文件和函数名
2. ExceptionEmitterConfig 正确格式化异常抛出语句
3. JavaExceptionEmitter 生成与现有代码一致的 Java 异常代码
4. PythonExceptionEmitter 和 CSharpExceptionEmitter 预留接口可用
"""

import sys
import os

# 将上级目录加入路径以导入被测模块
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from swig_exception_emitter import (
    Language,
    ExceptionContext,
    ExceptionEmitterConfig,
    JAVA_CONFIG,
    PYTHON_CONFIG,
    CSHARP_CONFIG,
    JavaExceptionEmitter,
    PythonExceptionEmitter,
    CSharpExceptionEmitter,
    create_exception_emitter,
    emit_java_ez_method,
    emit_python_ez_method,
    emit_csharp_ez_method,
)


def test_exception_context():
    """测试 ExceptionContext"""
    print("\n" + "="*60)
    print("测试 ExceptionContext")
    print("="*60)
    
    # 普通接口上下文
    ctx_normal = ExceptionContext(
        interface_name="IDasTaskInfoVector",
        method_name="EnumByIndex",
        ez_method_name="EnumByIndexEz",
        file_extension=".java",
        inherits_type_info=False
    )
    
    assert ctx_normal.source_file == "IDasTaskInfoVector.java"
    assert ctx_normal.source_function == "EnumByIndexEz"
    print(f"[OK] 普通接口上下文:")
    print(f"  source_file: {ctx_normal.source_file}")
    print(f"  source_function: {ctx_normal.source_function}")
    print(f"  inherits_type_info: {ctx_normal.inherits_type_info}")
    
    # 继承 IDasTypeInfo 的接口上下文
    ctx_typeinfo = ExceptionContext(
        interface_name="IDasJsonSetting",
        method_name="GetValue",
        ez_method_name="getValueEz",
        file_extension=".java",
        inherits_type_info=True
    )
    
    assert ctx_typeinfo.source_file == "IDasJsonSetting.java"
    assert ctx_typeinfo.source_function == "getValueEz"
    print(f"[OK] TypeInfo 接口上下文:")
    print(f"  source_file: {ctx_typeinfo.source_file}")
    print(f"  source_function: {ctx_typeinfo.source_function}")
    print(f"  inherits_type_info: {ctx_typeinfo.inherits_type_info}")
    
    print("\n[PASS] ExceptionContext 测试通过")


def test_exception_config_format():
    """测试 ExceptionEmitterConfig 格式化功能"""
    print("\n" + "="*60)
    print("测试 ExceptionEmitterConfig 格式化")
    print("="*60)
    
    # 普通接口 - create 方法
    ctx_normal = ExceptionContext(
        interface_name="IDasTaskInfoVector",
        method_name="EnumByIndex",
        ez_method_name="EnumByIndexEz",
        file_extension=".java",
        inherits_type_info=False,
        error_code_expr="result.getErrorCode()"
    )
    
    throw_stmt = JAVA_CONFIG.format_throw(ctx_normal)
    expected = 'throw DasException.create(result.getErrorCode(), "IDasTaskInfoVector.java", 0, "EnumByIndexEz");'
    assert throw_stmt == expected, f"期望值: {expected}\n实际值: {throw_stmt}"
    print(f"[OK] 普通接口异常抛出:")
    print(f"  {throw_stmt}")
    
    # TypeInfo 接口 - createWithTypeInfo 方法
    ctx_typeinfo = ExceptionContext(
        interface_name="IDasJsonSetting",
        method_name="GetValue",
        ez_method_name="getValueEz",
        file_extension=".java",
        inherits_type_info=True,
        error_code_expr="result.getErrorCode()",
        type_info_expr="this"
    )
    
    throw_stmt_typeinfo = JAVA_CONFIG.format_throw(ctx_typeinfo)
    expected_typeinfo = 'throw DasException.createWithTypeInfo(result.getErrorCode(), "IDasJsonSetting.java", 0, "getValueEz", this);'
    assert throw_stmt_typeinfo == expected_typeinfo, f"期望值: {expected_typeinfo}\n实际值: {throw_stmt_typeinfo}"
    print(f"[OK] TypeInfo 接口异常抛出:")
    print(f"  {throw_stmt_typeinfo}")
    
    # 错误检查
    check_expr = JAVA_CONFIG.format_error_check(ctx_normal)
    assert check_expr == "DuskAutoScript.IsFailed(result.getErrorCode())"
    print(f"[OK] 错误检查表达式:")
    print(f"  {check_expr}")
    
    print("\n[PASS] ExceptionEmitterConfig 格式化测试通过")


def test_java_exception_emitter():
    """测试 JavaExceptionEmitter"""
    print("\n" + "="*60)
    print("测试 JavaExceptionEmitter")
    print("="*60)
    
    emitter = JavaExceptionEmitter()
    
    # 测试异常检查代码
    ctx = ExceptionContext(
        interface_name="IDasTaskInfoVector",
        method_name="EnumByIndex",
        ez_method_name="EnumByIndexEz",
        file_extension=".java",
        inherits_type_info=False
    )
    
    check_code = emitter.emit_exception_check(ctx)
    print("[OK] 异常检查代码:")
    print(check_code)
    
    assert "DuskAutoScript.IsFailed(result.getErrorCode())" in check_code
    assert "throw DasException.create" in check_code
    
    # 测试单返回值 Ez 方法
    return_type = "IDasTaskInfo"
    params = [("int", "index")]
    ret_class_name = "DasRetTaskInfo"
    call_args = ["index"]
    
    ez_method = emitter.emit_ez_method(
        context=ctx,
        return_type=return_type,
        params=params,
        ret_class_name=ret_class_name,
        call_args=call_args
    )
    print("\n[OK] 单返回值 Ez 方法:")
    print(ez_method)
    
    assert "public final IDasTaskInfo EnumByIndexEz(int index) throws DasException" in ez_method
    assert "DasRetTaskInfo result = EnumByIndex(index);" in ez_method
    assert "throw DasException.create" in ez_method
    
    # 测试 TypeInfo 接口的 Ez 方法
    ctx_typeinfo = ExceptionContext(
        interface_name="IDasJsonSetting",
        method_name="GetValue",
        ez_method_name="getValueEz",
        file_extension=".java",
        inherits_type_info=True
    )
    
    ez_method_typeinfo = emitter.emit_ez_method(
        context=ctx_typeinfo,
        return_type="int",
        params=[],
        ret_class_name="DasRetInt",
        call_args=[]
    )
    print("\n[OK] TypeInfo 接口 Ez 方法:")
    print(ez_method_typeinfo)
    
    assert "throw DasException.createWithTypeInfo" in ez_method_typeinfo
    
    # 测试多返回值方法（预留）
    multi_out = emitter.emit_ez_method_multi_out(
        context=ctx,
        out_params=[("int", "width"), ("int", "height")],
        in_params=[],
        call_args=[]
    )
    print("\n[OK] 多返回值 Ez 方法（预留）:")
    print(multi_out)
    
    assert "@Deprecated" in multi_out
    assert "多返回值方法正在设计中" in multi_out
    
    print("\n[PASS] JavaExceptionEmitter 测试通过")


def test_python_exception_emitter():
    """测试 PythonExceptionEmitter（预留接口）"""
    print("\n" + "="*60)
    print("测试 PythonExceptionEmitter（预留接口）")
    print("="*60)
    
    emitter = PythonExceptionEmitter()
    
    ctx = ExceptionContext(
        interface_name="IDasTaskInfoVector",
        method_name="EnumByIndex",
        ez_method_name="enum_by_index_ez",
        file_extension=".py",
        inherits_type_info=False
    )
    
    # 预留的异常检查代码
    check_code = emitter.emit_exception_check(ctx)
    print("[OK] Python 异常检查代码（预留）:")
    print(check_code)
    
    assert "TODO: Python 异常检查" in check_code
    
    # 预留的 Ez 方法
    ez_method = emitter.emit_ez_method(
        context=ctx,
        return_type="Any",
        params=[("int", "index")],
        ret_class_name="DasRet",
        call_args=["index"]
    )
    print("\n[OK] Python Ez 方法（预留）:")
    print(ez_method)
    
    assert "def enum_by_index_ez(self, index):" in ez_method
    assert "RuntimeError" in ez_method
    
    print("\n[PASS] PythonExceptionEmitter 测试通过（预留接口）")


def test_csharp_exception_emitter():
    """测试 CSharpExceptionEmitter（预留接口）"""
    print("\n" + "="*60)
    print("测试 CSharpExceptionEmitter（预留接口）")
    print("="*60)
    
    emitter = CSharpExceptionEmitter()
    
    ctx = ExceptionContext(
        interface_name="IDasTaskInfoVector",
        method_name="EnumByIndex",
        ez_method_name="EnumByIndexEz",
        file_extension=".cs",
        inherits_type_info=False
    )
    
    # 预留的异常检查代码
    check_code = emitter.emit_exception_check(ctx)
    print("[OK] C# 异常检查代码（预留）:")
    print(check_code)
    
    assert "DasResult.IsFailed" in check_code
    
    # 预留的 Ez 方法
    ez_method = emitter.emit_ez_method(
        context=ctx,
        return_type="IDasTaskInfo",
        params=[("int", "index")],
        ret_class_name="DasRetTaskInfo",
        call_args=["index"]
    )
    print("\n[OK] C# Ez 方法（预留）:")
    print(ez_method)
    
    assert "public IDasTaskInfo EnumByIndexEz(int index)" in ez_method
    assert "DasException" in ez_method
    
    print("\n[PASS] CSharpExceptionEmitter 测试通过（预留接口）")


def test_factory_function():
    """测试工厂函数"""
    print("\n" + "="*60)
    print("测试工厂函数 create_exception_emitter")
    print("="*60)
    
    java_emitter = create_exception_emitter(Language.JAVA)
    assert isinstance(java_emitter, JavaExceptionEmitter)
    print("[OK] Java 生成器创建成功")
    
    python_emitter = create_exception_emitter(Language.PYTHON)
    assert isinstance(python_emitter, PythonExceptionEmitter)
    print("[OK] Python 生成器创建成功")
    
    csharp_emitter = create_exception_emitter(Language.CSHARP)
    assert isinstance(csharp_emitter, CSharpExceptionEmitter)
    print("[OK] C# 生成器创建成功")
    
    # 测试不支持的语言
    try:
        create_exception_emitter("unsupported")  # type: ignore
        assert False, "应该抛出 ValueError"
    except (ValueError, KeyError):
        print("[OK] 不支持的语言正确抛出异常")
    
    print("\n[PASS] 工厂函数测试通过")


def test_convenience_functions():
    """测试便捷函数"""
    print("\n" + "="*60)
    print("测试便捷函数")
    print("="*60)
    
    # Java 便捷函数
    java_code = emit_java_ez_method(
        interface_name="IDasJsonSetting",
        method_name="GetName",
        return_type="String",
        params=[],
        ret_class_name="DasRetReadOnlyString",
        inherits_type_info=True
    )
    print("[OK] emit_java_ez_method:")
    print(java_code)
    
    assert "public final String GetNameEz() throws DasException" in java_code
    assert "createWithTypeInfo" in java_code
    
    # Python 便捷函数
    python_code = emit_python_ez_method(
        interface_name="IDasJsonSetting",
        method_name="GetName",
        params=[],
        inherits_type_info=True
    )
    print("\n[OK] emit_python_ez_method:")
    print(python_code)
    
    assert "def GetName_ez(self):" in python_code
    
    # C# 便捷函数
    csharp_code = emit_csharp_ez_method(
        interface_name="IDasJsonSetting",
        method_name="GetName",
        return_type="string",
        params=[],
        ret_class_name="DasRetReadOnlyString",
        inherits_type_info=True
    )
    print("\n[OK] emit_csharp_ez_method:")
    print(csharp_code)
    
    assert "public string GetNameEz()" in csharp_code
    
    print("\n[PASS] 便捷函数测试通过")


def test_real_world_scenarios():
    """测试真实场景"""
    print("\n" + "="*60)
    print("测试真实场景")
    print("="*60)
    
    emitter = create_exception_emitter(Language.JAVA)
    
    # 场景 1: 带多个参数的接口方法
    ctx1 = ExceptionContext(
        interface_name="IDasOcr",
        method_name="Recognize",
        ez_method_name="RecognizeEz",
        file_extension=".java",
        inherits_type_info=False
    )
    
    method1 = emitter.emit_ez_method(
        context=ctx1,
        return_type="IDasOcrResult",
        params=[("IDasImage", "image"), ("int", "flags")],
        ret_class_name="DasRetOcrResult",
        call_args=["image", "flags"]
    )
    print("[OK] 场景 1: 带多个参数的接口方法")
    print(method1)
    
    # 场景 2: 属性 getter（小驼峰命名）
    ctx2 = ExceptionContext(
        interface_name="IDasTask",
        method_name="GetStatus",
        ez_method_name="getStatusEz",
        file_extension=".java",
        inherits_type_info=True
    )
    
    method2 = emitter.emit_ez_method(
        context=ctx2,
        return_type="DasTaskStatus",
        params=[],
        ret_class_name="DasRetTaskStatus",
        call_args=[]
    )
    print("\n[OK] 场景 2: 属性 getter（小驼峰命名）")
    print(method2)
    
    assert "getStatusEz" in method2
    assert "createWithTypeInfo" in method2
    
    # 场景 3: 带 IDasReadOnlyString 参数的方法
    ctx3 = ExceptionContext(
        interface_name="IDasJsonSetting",
        method_name="GetString",
        ez_method_name="GetStringEz",
        file_extension=".java",
        inherits_type_info=False
    )
    
    method3 = emitter.emit_ez_method(
        context=ctx3,
        return_type="String",
        params=[("String", "key")],
        ret_class_name="DasRetReadOnlyString",
        call_args=["DasReadOnlyString.fromString(key)"]
    )
    print("\n[OK] 场景 3: 字符串参数方法")
    print(method3)
    
    print("\n[PASS] 真实场景测试通过")


def run_all_tests():
    """运行所有测试"""
    print("\n" + "="*60)
    print("SWIG Exception Emitter 测试套件")
    print("="*60)
    
    try:
        test_exception_context()
        test_exception_config_format()
        test_java_exception_emitter()
        test_python_exception_emitter()
        test_csharp_exception_emitter()
        test_factory_function()
        test_convenience_functions()
        test_real_world_scenarios()
        
        print("\n" + "="*60)
        print("[PASS] 所有测试通过！")
        print("="*60)
        return True
    except AssertionError as e:
        print(f"\n[FAIL] 测试失败: {e}")
        import traceback
        traceback.print_exc()
        return False
    except Exception as e:
        print(f"\n[FAIL] 测试异常: {e}")
        import traceback
        traceback.print_exc()
        return False


if __name__ == "__main__":
    success = run_all_tests()
    sys.exit(0 if success else 1)
