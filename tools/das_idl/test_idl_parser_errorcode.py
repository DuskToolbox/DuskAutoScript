"""
errorcode 和 module IDL 块类型的解析器单元测试
验证 das_idl_parser 中新增的 errorcode / module 解析功能
"""
import sys
import unittest
from pathlib import Path

# 添加 tools/das_idl 到路径
sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import (
    ErrorCodeDef,
    ErrorCodeValue,
    IdlDocument,
    ModuleDef,
    ModuleFunctionDef,
    ParamDirection,
    TypeKind,
    parse_idl,
)


class TestErrorcodeParsing(unittest.TestCase):
    """errorcode 块解析测试"""

    def test_basic_errorcode(self):
        """解析包含显式值的基本 errorcode 块"""
        doc = parse_idl(
            "errorcode DasResult { DAS_S_OK = 0, DAS_E_FAIL = 1, DAS_E_INVALID_ARG = 2, }"
        )
        self.assertEqual(len(doc.error_codes), 1)
        ec = doc.error_codes[0]
        self.assertIsInstance(ec, ErrorCodeDef)
        self.assertEqual(ec.name, "DasResult")
        self.assertEqual(ec.namespace, "")
        self.assertEqual(len(ec.values), 3)
        self.assertEqual(ec.values[0], ErrorCodeValue(name="DAS_S_OK", value=0, namespace=""))
        self.assertEqual(ec.values[1], ErrorCodeValue(name="DAS_E_FAIL", value=1, namespace=""))
        self.assertEqual(
            ec.values[2], ErrorCodeValue(name="DAS_E_INVALID_ARG", value=2, namespace="")
        )

    def test_errorcode_hex_values(self):
        """解析十六进制值如 0x80000000"""
        doc = parse_idl(
            "errorcode HResult { S_OK = 0, E_FAIL = 0x80004005, E_ACCESSDENIED = 0x80070005, }"
        )
        ec = doc.error_codes[0]
        self.assertEqual(ec.values[0].value, 0)
        self.assertEqual(ec.values[1].value, 0x80004005)
        self.assertEqual(ec.values[2].value, 0x80070005)

    def test_errorcode_negative_values(self):
        """解析负数值如 -1, -1073750001"""
        doc = parse_idl(
            "errorcode NegResult { ERR_NONE = 0, ERR_GENERAL = -1, ERR_SPECIFIC = -1073750001, }"
        )
        ec = doc.error_codes[0]
        self.assertEqual(ec.values[0].value, 0)
        self.assertEqual(ec.values[1].value, -1)
        self.assertEqual(ec.values[2].value, -1073750001)

    def test_errorcode_auto_increment(self):
        """不带显式 = 的值自动递增"""
        doc = parse_idl("errorcode AutoInc { A = 10, B, C = 20, D, }")
        ec = doc.error_codes[0]
        # A=10, B auto=11, C=20, D auto=21
        self.assertEqual(ec.values[0].name, "A")
        self.assertEqual(ec.values[0].value, 10)
        self.assertEqual(ec.values[1].name, "B")
        self.assertEqual(ec.values[1].value, 11)
        self.assertEqual(ec.values[2].name, "C")
        self.assertEqual(ec.values[2].value, 20)
        self.assertEqual(ec.values[3].name, "D")
        self.assertEqual(ec.values[3].value, 21)

    def test_errorcode_semicolon_separator(self):
        """使用分号作为分隔符"""
        doc = parse_idl("errorcode SemiResult { X = 1; Y = 2; Z = 3; }")
        ec = doc.error_codes[0]
        self.assertEqual(len(ec.values), 3)
        self.assertEqual(ec.values[0].value, 1)
        self.assertEqual(ec.values[1].value, 2)
        self.assertEqual(ec.values[2].value, 3)

    def test_errorcode_empty_block_no_error(self):
        """空 errorcode 块不报错（当前解析器行为：允许空块）"""
        doc = parse_idl("errorcode EmptyResult {}")
        self.assertEqual(len(doc.error_codes), 1)
        self.assertEqual(doc.error_codes[0].name, "EmptyResult")
        self.assertEqual(len(doc.error_codes[0].values), 0)

    def test_errorcode_default_auto_starts_at_zero(self):
        """无显式值的 errorcode 从 0 开始自动递增"""
        doc = parse_idl("errorcode DefAuto { A, B, C, }")
        ec = doc.error_codes[0]
        self.assertEqual(ec.values[0].value, 0)
        self.assertEqual(ec.values[1].value, 1)
        self.assertEqual(ec.values[2].value, 2)


class TestModuleParsing(unittest.TestCase):
    """module 块解析测试"""

    def test_basic_module(self):
        """解析包含一个函数的基本 module 块"""
        doc = parse_idl("module MyApi { void Hello(); }")
        self.assertEqual(len(doc.modules), 1)
        mod = doc.modules[0]
        self.assertIsInstance(mod, ModuleDef)
        self.assertEqual(mod.name, "MyApi")
        # module_name 默认等于 name
        self.assertEqual(mod.module_name, "MyApi")
        self.assertEqual(mod.namespace, "")
        self.assertEqual(len(mod.functions), 1)
        func = mod.functions[0]
        self.assertIsInstance(func, ModuleFunctionDef)
        self.assertEqual(func.name, "Hello")
        self.assertEqual(func.return_type.base_type, "void")

    def test_module_with_attributes(self):
        """函数带 [export] 和 [c_abi] 属性"""
        doc = parse_idl(
            "module Api { [export] void LogMsg(const char* msg); "
            "[export, c_abi] DasResult Query(int id); }"
        )
        mod = doc.modules[0]
        self.assertEqual(len(mod.functions), 2)

        f1 = mod.functions[0]
        self.assertEqual(f1.name, "LogMsg")
        self.assertTrue(f1.attributes.get("export"))
        self.assertNotIn("c_abi", f1.attributes)

        f2 = mod.functions[1]
        self.assertEqual(f2.name, "Query")
        self.assertTrue(f2.attributes.get("export"))
        self.assertTrue(f2.attributes.get("c_abi"))

    def test_module_with_module_name(self):
        """module_name 属性覆盖默认名称"""
        doc = parse_idl(
            'module DasCoreApi [module_name = "DasCore"] { void Init(); }'
        )
        mod = doc.modules[0]
        self.assertEqual(mod.name, "DasCoreApi")
        self.assertEqual(mod.module_name, "DasCore")

    def test_module_with_out_parameters(self):
        """module 函数包含 [out] 参数"""
        doc = parse_idl(
            "module Api { DasResult GetObj([out] int** pp_obj); }"
        )
        func = doc.modules[0].functions[0]
        self.assertEqual(len(func.parameters), 1)
        param = func.parameters[0]
        self.assertEqual(param.name, "pp_obj")
        self.assertEqual(param.direction, ParamDirection.OUT)
        self.assertEqual(param.type_info.base_type, "int")
        self.assertEqual(param.type_info.pointer_level, 2)

    def test_module_multiple_functions(self):
        """module 包含多个函数"""
        doc = parse_idl(
            "module Multi { void Init(); void Shutdown(); int GetCount(); }"
        )
        mod = doc.modules[0]
        self.assertEqual(len(mod.functions), 3)
        self.assertEqual(mod.functions[0].name, "Init")
        self.assertEqual(mod.functions[1].name, "Shutdown")
        self.assertEqual(mod.functions[2].name, "GetCount")

    def test_module_empty_block(self):
        """空 module 块（无函数）"""
        doc = parse_idl("module EmptyApi {}")
        self.assertEqual(len(doc.modules), 1)
        self.assertEqual(len(doc.modules[0].functions), 0)


class TestMixedIdl(unittest.TestCase):
    """混合 IDL 类型测试"""

    def test_errorcode_with_interface(self):
        """同一文件中 errorcode + interface"""
        idl = """
        errorcode Result { OK = 0, FAIL = 1, }
        [uuid("12345678-1234-1234-1234-123456789012")] interface IFoo : IDasBase {
            DasResult DoWork();
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 1)
        self.assertEqual(len(doc.interfaces), 1)
        self.assertEqual(doc.error_codes[0].name, "Result")
        self.assertEqual(doc.interfaces[0].name, "IFoo")

    def test_errorcode_module_interface(self):
        """同一文件中 errorcode + module + interface"""
        idl = """
        errorcode DasResult { DAS_S_OK = 0, DAS_E_FAIL = 1, }
        module DasApi { [export] void LogError(const char* msg); }
        [uuid("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")] interface IBar : IDasBase {
            int GetValue();
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 1)
        self.assertEqual(len(doc.modules), 1)
        self.assertEqual(len(doc.interfaces), 1)
        self.assertEqual(doc.error_codes[0].name, "DasResult")
        self.assertEqual(doc.modules[0].name, "DasApi")
        self.assertEqual(doc.interfaces[0].name, "IBar")

    def test_multiple_errorcodes(self):
        """多个 errorcode 定义"""
        idl = """
        errorcode Result1 { A = 0, B = 1, }
        errorcode Result2 { X = 10, Y = 20, }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 2)
        self.assertEqual(doc.error_codes[0].name, "Result1")
        self.assertEqual(doc.error_codes[1].name, "Result2")
        self.assertEqual(doc.error_codes[1].values[0].value, 10)

    def test_multiple_modules(self):
        """多个 module 定义"""
        idl = """
        module Mod1 { void F1(); }
        module Mod2 { void F2(); }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.modules), 2)
        self.assertEqual(doc.modules[0].name, "Mod1")
        self.assertEqual(doc.modules[1].name, "Mod2")


class TestForbiddenVoidPointerPointer(unittest.TestCase):
    """void** is forbidden in IDL signatures"""

    def test_rejects_interface_void_pointer_pointer_out_param(self):
        """接口方法参数中出现 void** 时直接报错"""
        idl = """
        [uuid("12345678-1234-1234-1234-123456789012")]
        interface IFoo : IDasBase {
            DasResult Query([out] void** pp_out);
        }
        """

        with self.assertRaisesRegex(SyntaxError, r"void\*\*"):
            parse_idl(idl)

    def test_rejects_module_void_pointer_pointer_out_param(self):
        """module 函数参数中出现 void** 时直接报错"""
        idl = """
        module Api {
            DasResult Query([out] void** pp_out);
        }
        """

        with self.assertRaisesRegex(SyntaxError, r"void\*\*"):
            parse_idl(idl)

    def test_rejects_multi_out_void_pointer_pointer(self):
        """多返回值路径中的 void** 必须在解析阶段报错"""
        idl = """
        [uuid("12345678-1234-1234-1234-123456789012")]
        interface IFoo : IDasBase {
            DasResult GetRawData([out] void** pp_data, [out] uint64_t* p_size);
        }
        """

        with self.assertRaisesRegex(SyntaxError, r"void\*\*"):
            parse_idl(idl)

    def test_allows_idasbase_out_object_pointer(self):
        """任意 IDasBase 或其子类型指针出参使用 IDasBase**"""
        idl = """
        [uuid("12345678-1234-1234-1234-123456789012")]
        interface IFoo : IDasBase {
            DasResult Query([out] IDasBase** pp_out);
        }
        """

        doc = parse_idl(idl)
        param = doc.interfaces[0].methods[0].parameters[0]
        self.assertEqual(param.type_info.base_type, "IDasBase")
        self.assertEqual(param.type_info.pointer_level, 2)
        self.assertEqual(param.type_info.type_kind, TypeKind.INTERFACE)

    def test_allows_plain_void_return(self):
        """普通 void 返回值仍然合法"""
        doc = parse_idl("module Api { void Shutdown(); }")
        func = doc.modules[0].functions[0]
        self.assertEqual(func.return_type.base_type, "void")
        self.assertEqual(func.return_type.pointer_level, 0)


class TestNamespaceScoped(unittest.TestCase):
    """命名空间作用域测试"""

    def test_errorcode_in_namespace(self):
        """namespace 内的 errorcode 块"""
        idl = """
        namespace DAS {
            errorcode DasResult { DAS_S_OK = 0, DAS_E_FAIL = -1, }
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 1)
        ec = doc.error_codes[0]
        self.assertEqual(ec.name, "DasResult")
        self.assertEqual(ec.namespace, "DAS")
        # 值也继承 namespace
        self.assertEqual(ec.values[0].namespace, "DAS")
        self.assertEqual(ec.values[1].namespace, "DAS")

    def test_module_in_namespace(self):
        """namespace 内的 module 块"""
        idl = """
        namespace DAS {
            module DasCoreApi { [export] void Init(); }
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.modules), 1)
        mod = doc.modules[0]
        self.assertEqual(mod.name, "DasCoreApi")
        self.assertEqual(mod.namespace, "DAS")
        self.assertEqual(mod.functions[0].namespace, "DAS")

    def test_nested_namespace_errorcode_and_module(self):
        """嵌套命名空间中的 errorcode 和 module"""
        idl = """
        namespace DAS::Core {
            errorcode CoreResult { OK = 0, ERROR = 1, }
            module CoreApi { void DoStuff(); }
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 1)
        self.assertEqual(len(doc.modules), 1)
        self.assertEqual(doc.error_codes[0].namespace, "DAS::Core")
        self.assertEqual(doc.modules[0].namespace, "DAS::Core")


class TestResolveTypesWithNewTypes(unittest.TestCase):
    """resolve_types 对新类型的兼容性测试"""

    def test_resolve_types_no_crash_with_errorcode(self):
        """resolve_types 处理 errorcode 不崩溃"""
        idl = """
        errorcode DasResult { DAS_S_OK = 0, DAS_E_FAIL = -1, }
        """
        doc = parse_idl(idl)
        # parse_idl 内部已调用 resolve_types，只要不抛异常就算通过
        self.assertIsNotNone(doc)
        self.assertEqual(len(doc.error_codes), 1)

    def test_resolve_types_no_crash_with_module(self):
        """resolve_types 处理 module 不崩溃"""
        idl = """
        module Api {
            [export] DasResult GetResult();
            [export] void SetName(const char* name);
        }
        """
        doc = parse_idl(idl)
        self.assertIsNotNone(doc)
        self.assertEqual(len(doc.modules), 1)
        # 验证函数参数/返回值的 TypeInfo 已被 annotate
        func = doc.modules[0].functions[0]
        self.assertEqual(func.return_type.base_type, "DasResult")
        # DasResult 在 BUILTIN_TYPES 中，所以 type_kind 为 BASIC
        self.assertEqual(func.return_type.type_kind, TypeKind.BASIC)

    def test_resolve_types_annotates_module_function_params(self):
        """resolve_types 正确标注 module 函数参数的 TypeInfo"""
        idl = """
        enum Color { Red = 0, Green = 1, Blue = 2, }
        module Paint {
            void SetColor(Color c);
        }
        """
        doc = parse_idl(idl)
        func = doc.modules[0].functions[0]
        param = func.parameters[0]
        self.assertEqual(param.type_info.base_type, "Color")
        self.assertEqual(param.type_info.type_kind, TypeKind.ENUM)

    def test_resolve_types_mixed_all_types(self):
        """resolve_types 处理所有类型混合不崩溃"""
        idl = """
        errorcode DasResult { DAS_S_OK = 0, }
        enum Status { Active = 0, Inactive = 1, }
        struct Point { double x; double y; }
        module Api { Status GetStatus(); }
        [uuid("00000000-0000-0000-0000-000000000001")] interface IFoo : IDasBase {
            DasResult DoWork();
        }
        """
        doc = parse_idl(idl)
        self.assertEqual(len(doc.error_codes), 1)
        self.assertEqual(len(doc.enums), 1)
        self.assertEqual(len(doc.structs), 1)
        self.assertEqual(len(doc.modules), 1)
        self.assertEqual(len(doc.interfaces), 1)


if __name__ == "__main__":
    unittest.main()
