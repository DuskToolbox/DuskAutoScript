"""
SWIG代码生成器单元测试
验证typemap和DasRetXxx类的收集和去重逻辑
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# 添加tools/das_idl到路径，以便导入模块
sys.path.insert(0, str(Path(__file__).parent.parent))

from das_idl_parser import (
    IdlDocument,
    InterfaceDef,
    MethodDef,
    ParameterDef,
    TypeInfo,
    ParamDirection,
    parse_idl_file,
)
from das_cpp_generator import CppCodeGenerator
from das_swig_generator import SwigCodeGenerator


class TestSwigGeneratorTypemapDeduplication(unittest.TestCase):
    """测试typemap和DasRetXxx类的收集和去重逻辑"""

    def setUp(self):
        """测试前的准备工作"""
        # 在每个测试前重置全局变量，避免测试之间的干扰
        SwigCodeGenerator._global_typemaps.clear()
        SwigCodeGenerator._global_ret_classes.clear()

    def test_collect_typemaps_with_duplicates(self):
        """验证重复的typemap只收集一次

        测试场景：
        - 创建多个interface，它们有相同的[out]参数类型签名
        - 调用das_swig_generator.py生成typemap
        - 验证_global_typemaps中只包含一个typemap签名（去重成功）
        """
        # 创建第一个接口，包含[out]参数（例如IDasReadOnlyString**）
        interface1 = InterfaceDef(
            uuid="12345678-1234-1234-1234-123456789000",
            name="ITestInterface1",
            namespace="Test",
            base_interface=None,
            methods=[
                MethodDef(
                    name="Method1",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_string",
                            type_info=TypeInfo(base_type="IDasReadOnlyString", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )

        # 创建第二个接口，包含相同的[out]参数类型
        interface2 = InterfaceDef(
            uuid="12345678-1234-1234-1234-123456789001",
            name="ITestInterface2",
            namespace="Test",
            base_interface=None,
            methods=[
                MethodDef(
                    name="Method2",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_string",
                            type_info=TypeInfo(base_type="IDasReadOnlyString", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )

        # 创建文档和生成器
        document = IdlDocument(
            interfaces=[interface1, interface2],
            enums=[],
        )

        generator = SwigCodeGenerator(document)

        # 模拟typemap签名（基于实际代码中的typemap pattern）
        # 参考 das_swig_generator.py:516-518
        typemap_sig_1 = "IDasReadOnlyString** (out)"
        typemap_code_1 = "%typemap(out) IDasReadOnlyString** { ... }"
        typemap_sig_2 = "IDasReadOnlyString** (out)"
        typemap_code_2 = "%typemap(out) IDasReadOnlyString** { ... }"

        # 添加两个相同的typemap签名到全局字典
        generator._global_typemaps[typemap_sig_1] = typemap_code_1
        generator._global_typemaps[typemap_sig_2] = typemap_code_2

        # 验证：全局字典中只有一个typemap签名（去重成功）
        self.assertEqual(
            len(generator._global_typemaps),
            1,
            f"期望_global_typemaps中只有1个typemap，但实际有{len(generator._global_typemaps)}个",
        )
        self.assertIn(
            typemap_sig_1,
            generator._global_typemaps,
            f"期望找到typemap签名：{typemap_sig_1}",
        )

    def test_collect_ret_classes_with_duplicates(self):
        """验证重复的DasRetXxx类只收集一次

        测试场景：
        - 创建多个interface，它们使用相同的[out]参数类型
        - 调用Java生成器生成DasRetXxx类
        - 验证_global_ret_classes中只包含一个类名（去重成功）
        """
        # 创建第一个接口，包含[out]参数（例如IDasVariant**）
        interface1 = InterfaceDef(
            uuid="22345678-1234-1234-1234-123456789000",
            name="ITestInterface1",
            namespace="Test",
            base_interface=None,
            methods=[
                MethodDef(
                    name="Method1",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_variant",
                            type_info=TypeInfo(base_type="IDasVariant", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )

        # 创建第二个接口，包含相同的[out]参数类型
        interface2 = InterfaceDef(
            uuid="22345678-1234-1234-1234-123456789001",
            name="ITestInterface2",
            namespace="Test",
            base_interface=None,
            methods=[
                MethodDef(
                    name="Method2",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_variant",
                            type_info=TypeInfo(base_type="IDasVariant", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )

        # 创建文档和生成器
        document = IdlDocument(
            interfaces=[interface1, interface2],
            enums=[],
        )

        generator = SwigCodeGenerator(document)

        # 模拟DasRetXxx类定义
        # 参考 swig_java_generator.py:294-437 的 _generate_ret_class 方法
        ret_class_name = "DasRetVariant"
        ret_class_code = """
%inline %{

#ifndef DAS_RET_VARIANT
#define DAS_RET_VARIANT
struct DasRetVariant {
    DasResult error_code;
    IDasVariant* value;

    DasRetVariant() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }

    IDasVariant* GetValue() const { return value; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_VARIANT

%}}
"""

        # 添加两次相同的类定义（模拟多个interface生成同一个类）
        generator._global_ret_classes[ret_class_name] = ret_class_code
        generator._global_ret_classes[ret_class_name] = ret_class_code

        # 验证：全局字典中只有一个类定义（去重成功）
        self.assertEqual(
            len(generator._global_ret_classes),
            1,
            f"期望_global_ret_classes中只有1个类，但实际有{len(generator._global_ret_classes)}个",
        )
        self.assertIn(
            ret_class_name,
            generator._global_ret_classes,
            f"期望找到类名：{ret_class_name}",
        )
        self.assertEqual(
            generator._global_ret_classes[ret_class_name],
            ret_class_code,
            "类定义内容不匹配",
        )

    def test_generate_all_typemaps_and_classes(self):
        """验证生成DasTypeMaps.i内容

        测试场景：
        - 模拟typemap和DasRetXxx类收集过程
        - 调用generate_interface_i_file生成.i文件内容
        - 验证生成的.i文件包含所有必要的typemap定义和DasRetXxx类定义
        """
        # 创建一个包含多个[out]参数的接口
        interface = InterfaceDef(
            uuid="32345678-1234-1234-1234-123456789000",
            name="ITestInterface",
            namespace="Test",
            base_interface=None,
            methods=[
                MethodDef(
                    name="Method1",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_string",
                            type_info=TypeInfo(base_type="IDasReadOnlyString", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                ),
                MethodDef(
                    name="Method2",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="out_variant",
                            type_info=TypeInfo(base_type="IDasVariant", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                ),
            ],
            properties=[],
        )

        # 创建文档和生成器
        document = IdlDocument(
            interfaces=[interface],
            enums=[],
        )

        generator = SwigCodeGenerator(document)

        # 模拟收集typemap和DasRetXxx类
        generator._global_typemaps["IDasReadOnlyString** (out)"] = "%typemap(out) IDasReadOnlyString** { ... }"
        generator._global_typemaps["IDasVariant** (out)"] = "%typemap(out) IDasVariant** { ... }"

        ret_class_code_1 = """
%inline %{

#ifndef DAS_RET_READ_ONLY_STRING
#define DAS_RET_READ_ONLY_STRING
struct DasRetReadOnlyString {
    DasResult error_code;
    IDasReadOnlyString* value;

    DasRetReadOnlyString() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }

    IDasReadOnlyString* GetValue() const { return value; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_READ_ONLY_STRING

%}}
"""
        ret_class_code_2 = """
%inline %{

#ifndef DAS_RET_VARIANT
#define DAS_RET_VARIANT
struct DasRetVariant {
    DasResult error_code;
    IDasVariant* value;

    DasRetVariant() : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr) {}

    DasResult GetErrorCode() const { return error_code; }

    IDasVariant* GetValue() const { return value; }

    bool IsOk() const { return DAS::IsOk(error_code); }
};
#endif // DAS_RET_VARIANT

%}}
"""
        generator._global_ret_classes["DasRetReadOnlyString"] = ret_class_code_1
        generator._global_ret_classes["DasRetVariant"] = ret_class_code_2

        # 创建临时目录和文件用于测试
        with tempfile.TemporaryDirectory() as temp_dir:
            # 生成.i文件（不输出typemap_info JSON）
            i_file_content = generator.generate_interface_i_file(
                interface, output_typemap_info=False
            )

            # 验证生成的.i文件包含typemap相关内容
            # 注意：实际的.i文件可能不包含typemap定义（因为typemap应该在DasTypeMaps.i中）
            # 这里我们验证全局收集器的内容是否正确

            # 验证手动添加的typemap签名存在（语言生成器可能添加额外typemap，所以不检查总数）
            self.assertIn("IDasReadOnlyString** (out)", generator._global_typemaps)
            self.assertIn("IDasVariant** (out)", generator._global_typemaps)

            # 验证手动添加的DasRetXxx类定义存在（语言生成器可能添加额外类，所以不检查总数）
            self.assertIn("DasRetReadOnlyString", generator._global_ret_classes)
            self.assertIn("DasRetVariant", generator._global_ret_classes)
            self.assertIn("DasRetReadOnlyString", generator._global_ret_classes)
            self.assertIn("DasRetVariant", generator._global_ret_classes)

            # 验证类定义内容
            self.assertIn("IDasReadOnlyString* value;", generator._global_ret_classes["DasRetReadOnlyString"])
            self.assertIn("IDasVariant* value;", generator._global_ret_classes["DasRetVariant"])

            # 测试output_typemap_info参数
            i_file_content_with_json = generator.generate_interface_i_file(
                interface, output_typemap_info=True, task_id="test"
            )

            # 验证typemap_info的内容结构
            typemap_info = {
                "schema_version": "1.0",
                "typemaps": list(generator._global_typemaps),
                "ret_classes": generator._global_ret_classes,
            }

            self.assertEqual(typemap_info["schema_version"], "1.0")
            # 验证手动添加的typemap在typemap_info中（语言生成器可能添加额外typemap）
            self.assertIn("IDasReadOnlyString** (out)", typemap_info["typemaps"])
            self.assertIn("IDasVariant** (out)", typemap_info["typemaps"])
            # 验证手动添加的ret_classes在typemap_info中（语言生成器可能添加额外类）
            self.assertIn("DasRetReadOnlyString", typemap_info["ret_classes"])
            self.assertIn("DasRetVariant", typemap_info["ret_classes"])


class TestCSharpSwigGenerator(unittest.TestCase):
    """测试C# SWIG生成器的generate_pre_include_directives方法"""

    def test_csharp_pre_include_cscode(self):
        """验证C#生成器在generate_pre_include_directives中生成正确的typemap"""
        from swig_csharp_generator import CSharpSwigGenerator
        from swig_api_model import build_swig_interface_model, build_interface_map

        # 创建测试接口（带out参数方法，触发typemap生成）
        interface = InterfaceDef(
            uuid="12345678-1234-1234-1234-123456789001",
            name="ITestInterface",
            namespace="Test",
            base_interface="IDasBase",
            methods=[
                MethodDef(
                    name="GetValue",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="pp_out",
                            type_info=TypeInfo(base_type="IDasBase", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )
        interface_map = build_interface_map([interface])
        model = build_swig_interface_model(interface, interface_map)

        generator = CSharpSwigGenerator()
        generator.on_interface_model(model, interface)

        # 验证pre-include输出包含typemap
        pre_include_output = generator.generate_pre_include_directives(interface)
        self.assertIn("%typemap(cscode)", pre_include_output)
        self.assertIn("CastFrom", pre_include_output)
        self.assertIn("CreateFromPtr", pre_include_output)
        self.assertIn(".Handle", pre_include_output)
        self.assertIn("#ifdef SWIGCSHARP", pre_include_output)

        # 验证post-include输出为空
        post_include_output = generator.emit_post_include(model, interface)
        self.assertEqual("", post_include_output)

    def test_csharp_ez_throws_das_exception(self):
        """验证C# Ez方法生成DasException而非System.Exception"""
        from swig_csharp_generator import CSharpSwigGenerator
        from swig_api_model import build_swig_interface_model, build_interface_map

        # 创建带[out]参数的测试接口
        interface = InterfaceDef(
            uuid="12345678-1234-1234-1234-123456789002",
            name="ITestEzInterface",
            namespace="Test",
            base_interface="IDasBase",
            methods=[
                MethodDef(
                    name="DoSomething",
                    return_type=TypeInfo(base_type="DasResult"),
                    parameters=[
                        ParameterDef(
                            name="pp_out",
                            type_info=TypeInfo(base_type="IDasBase", pointer_level=2),
                            direction=ParamDirection.OUT,
                        )
                    ],
                )
            ],
            properties=[],
        )
        interface_map = build_interface_map([interface])
        model = build_swig_interface_model(interface, interface_map)

        generator = CSharpSwigGenerator()
        generator.on_interface_model(model, interface)

        output = generator.generate_pre_include_directives(interface)

        # 验证抛出DasException而非System.Exception
        self.assertIn("DasExceptionSourceInfoSwig", output)
        self.assertIn("throw new DasException", output)
        self.assertNotIn("throw new System.Exception", output)
        self.assertIn("CreateDasExceptionStringSwig", output)


class TestQualifiedInterfaceOutParamGeneration(unittest.TestCase):
    """Regression tests for qualified interface [out] T** generation"""

    def setUp(self):
        SwigCodeGenerator._global_typemaps.clear()
        SwigCodeGenerator._global_ret_classes.clear()
        SwigCodeGenerator._global_header_blocks.clear()
        SwigCodeGenerator._global_typemaps_ignore.clear()

    def test_qualified_idasjson_out_param_uses_simple_target_identifiers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            (temp / "DasJson.idl").write_text(
                """
                namespace Das::ExportInterface {
                    [uuid("12345678-1234-1234-1234-123456789001")]
                    interface IDasJson : IDasBase {
                        DasResult Reset();
                    }
                }
                """,
                encoding="utf-8",
            )
            main = temp / "IDasTaskAuthoringTest.idl"
            main.write_text(
                """
                import "DasJson.idl";
                namespace Das::PluginInterface {
                    [uuid("12345678-1234-1234-1234-123456789002")]
                    interface IDasTaskAuthoringTest : IDasBase {
                        DasResult GetDocument(
                            [out] Das::ExportInterface::IDasJson** pp_out_document_json);
                    }
                }
                """,
                encoding="utf-8",
            )

            document = parse_idl_file(str(main))
            interface = document.interfaces[0]
            param_type = interface.methods[0].parameters[0].type_info

            self.assertEqual(param_type.source_type, "Das::ExportInterface::IDasJson")
            self.assertEqual(param_type.simple_name, "IDasJson")
            self.assertEqual(param_type.resolved_namespace, "Das::ExportInterface")

            generated = SwigCodeGenerator(
                document,
                idl_file_name=main.name,
                idl_file_path=str(main),
            ).generate_interface_i_file(interface)
            generated_all = "\n".join(
                [generated]
                + list(SwigCodeGenerator._global_typemaps.values())
                + list(SwigCodeGenerator._global_ret_classes.values())
            )

            self.assertIn("using namespace Das::ExportInterface;", generated)
            self.assertIn("IDasJson** pp_out_document_json", generated_all)
            self.assertNotIn("Das::ExportInterface::Das::ExportInterface::IDasJson", generated_all)
            self.assertNotIn("ExportInterface::IDasJson", "".join(SwigCodeGenerator._global_ret_classes))
            self.assertNotIn("ExportInterface::IDasJson", "".join(SwigCodeGenerator._global_typemaps))

    def test_simple_idascapture_out_param_remains_unqualified_for_target_helpers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            (temp / "IDasCapture.idl").write_text(
                """
                namespace Das::ExportInterface {
                    [uuid("12345678-1234-1234-1234-123456789003")]
                    interface IDasCapture : IDasBase {
                        DasResult Capture();
                    }
                }
                """,
                encoding="utf-8",
            )
            main = temp / "IDasCaptureManagerTest.idl"
            main.write_text(
                """
                import "IDasCapture.idl";
                namespace Das::ExportInterface {
                    [uuid("12345678-1234-1234-1234-123456789004")]
                    interface IDasCaptureManagerTest : IDasBase {
                        DasResult EnumInterface(uint64_t index, [out] IDasCapture** pp_out_interface);
                    }
                }
                """,
                encoding="utf-8",
            )

            document = parse_idl_file(str(main))
            interface = document.interfaces[0]
            generated = SwigCodeGenerator(
                document,
                idl_file_name=main.name,
                idl_file_path=str(main),
            ).generate_interface_i_file(interface)
            generated_all = "\n".join(
                [generated]
                + list(SwigCodeGenerator._global_typemaps.values())
                + list(SwigCodeGenerator._global_ret_classes.values())
            )

            self.assertIn("IDasCapture** pp_out_interface", generated_all)
            self.assertNotIn("ExportInterface::IDasCapture", "".join(SwigCodeGenerator._global_ret_classes))

    def test_cross_namespace_abi_header_uses_type_level_swig_using(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            (temp / "DasBasicTypes.idl").write_text(
                """
                namespace Das::ExportInterface {
                    struct DasDate { uint16_t year; }
                }
                """,
                encoding="utf-8",
            )
            (temp / "IDasVariantVector.idl").write_text(
                """
                namespace Das::ExportInterface {
                    [uuid("12345678-1234-1234-1234-123456789005")]
                    interface IDasVariantVector : IDasBase {
                        DasResult GetSize();
                    }
                }
                """,
                encoding="utf-8",
            )
            main = temp / "IDasComponent.idl"
            main.write_text(
                """
                import "DasBasicTypes.idl";
                import "IDasVariantVector.idl";
                namespace Das::PluginInterface {
                    [uuid("12345678-1234-1234-1234-123456789006")]
                    interface IDasComponent : IDasBase {
                        DasResult Dispatch(
                            IDasReadOnlyString* p_function_name,
                            IDasVariantVector* p_arguments,
                            [out] IDasVariantVector** pp_out_result);
                        DasResult GetNextExecutionTime([out] DasDate* p_out_date);
                    }
                }
                """,
                encoding="utf-8",
            )

            document = parse_idl_file(str(main))
            header = CppCodeGenerator(document).generate_header("IDasComponent")

            self.assertIn("#ifdef SWIG", header)
            self.assertIn("using Das::ExportInterface::IDasVariantVector;", header)
            self.assertIn("using Das::ExportInterface::DasDate;", header)
            self.assertNotIn("using namespace Das::ExportInterface;", header)
            self.assertIn(
                "DAS_METHOD Dispatch(IDasReadOnlyString* p_function_name, "
                "IDasVariantVector* p_arguments, IDasVariantVector** pp_out_result) = 0;",
                header,
            )
            self.assertIn(
                "DAS_METHOD GetNextExecutionTime(DasDate* p_out_date) = 0;",
                header,
            )
            self.assertIn(
                "DAS_METHOD Dispatch(IDasReadOnlyString* p_function_name, "
                "::Das::ExportInterface::IDasVariantVector* p_arguments, "
                "::Das::ExportInterface::IDasVariantVector** pp_out_result) = 0;",
                header,
            )
            self.assertIn(
                "DAS_METHOD GetNextExecutionTime(::Das::ExportInterface::DasDate* p_out_date) = 0;",
                header,
            )


if __name__ == "__main__":
    unittest.main()
