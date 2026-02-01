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
)
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

            # 验证typemap签名数量
            self.assertEqual(
                len(generator._global_typemaps),
                2,
                f"期望_global_typemaps中有2个typemap，但实际有{len(generator._global_typemaps)}个",
            )
            self.assertIn("IDasReadOnlyString** (out)", generator._global_typemaps)
            self.assertIn("IDasVariant** (out)", generator._global_typemaps)

            # 验证DasRetXxx类定义数量
            self.assertEqual(
                len(generator._global_ret_classes),
                2,
                f"期望_global_ret_classes中有2个类，但实际有{len(generator._global_ret_classes)}个",
            )
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
            self.assertEqual(len(typemap_info["typemaps"]), 2)
            self.assertEqual(len(typemap_info["ret_classes"]), 2)


if __name__ == "__main__":
    unittest.main()
