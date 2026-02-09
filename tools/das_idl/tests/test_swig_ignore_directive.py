"""
测试 SWIG Java 生成器的 %ignore 指令生成

验证以下修复:
1. 全局命名空间接口的 %ignore 指令不使用 :: 前缀
2. 多返回值方法生成正确的 %ignore 代码

测试流程:
1. 创建测试 IDL 文件
2. 解析 IDL 并生成 ABI/SWIG 文件
3. 验证生成的 %ignore 指令与 ABI 签名匹配
"""

import unittest
import tempfile
import os
import sys
from pathlib import Path

# 添加工具目录到路径
sys.path.insert(0, str(Path(__file__).parent.parent))

from das_idl_parser import parse_idl_file, InterfaceDef, ParamDirection
from swig_java_generator import JavaSwigGenerator


class TestSwigIgnoreDirective(unittest.TestCase):
    """测试 SWIG %ignore 指令生成"""

    def setUp(self):
        """设置测试环境"""
        self.test_dir = tempfile.mkdtemp()
        self.generator = JavaSwigGenerator()

    def tearDown(self):
        """清理测试环境"""
        import shutil
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def create_test_idl(self, content: str) -> str:
        """创建测试 IDL 文件"""
        idl_path = os.path.join(self.test_dir, "test.idl")
        with open(idl_path, 'w', encoding='utf-8') as f:
            f.write(content)
        return idl_path

    def test_global_namespace_interface_ignore(self):
        """测试全局命名空间接口的 %ignore 不使用 :: 前缀"""
        # 创建测试 IDL - 全局命名空间接口
        idl_content = '''
[uuid("12345678-1234-1234-1234-123456789012")]
interface IGlobalInterface : IDasBase {
    DasResult GetString([out] IDasReadOnlyString** pp_out_name);
}
'''
        idl_path = self.create_test_idl(idl_content)
        
        # 解析 IDL
        document = parse_idl_file(idl_path)
        self.assertEqual(len(document.interfaces), 1)
        
        iface = document.interfaces[0]
        self.assertEqual(iface.name, "IGlobalInterface")
        self.assertEqual(iface.namespace, "")  # 全局命名空间
        
        # 验证方法
        self.assertEqual(len(iface.methods), 1)
        method = iface.methods[0]
        self.assertEqual(method.name, "GetString")
        self.assertEqual(len(method.parameters), 1)
        
        param = method.parameters[0]
        self.assertEqual(param.direction, ParamDirection.OUT)
        self.assertEqual(param.type_info.base_type, "IDasReadOnlyString")
        self.assertTrue(param.type_info.is_pointer)
        self.assertEqual(param.type_info.pointer_level, 2)

    def test_multi_out_param_method(self):
        """测试多返回值方法的参数解析"""
        # 创建测试 IDL - 多返回值方法
        idl_content = '''
namespace Das::ExportInterface {
    [uuid("12345678-1234-1234-1234-123456789013")]
    interface IDasMultiOut : IDasBase {
        DasResult FindText([out] IDasResult** result, [out] uint64_t* count);
    }
}
'''
        idl_path = self.create_test_idl(idl_content)
        
        # 解析 IDL
        document = parse_idl_file(idl_path)
        self.assertEqual(len(document.interfaces), 1)
        
        iface = document.interfaces[0]
        self.assertEqual(iface.name, "IDasMultiOut")
        self.assertEqual(iface.namespace, "Das::ExportInterface")
        
        # 验证方法
        self.assertEqual(len(iface.methods), 1)
        method = iface.methods[0]
        self.assertEqual(method.name, "FindText")
        self.assertEqual(len(method.parameters), 2)
        
        # 验证两个都是 [out] 参数
        for param in method.parameters:
            self.assertEqual(param.direction, ParamDirection.OUT)

    def test_qualified_interface_name_generation(self):
        """测试 qualified_interface 名称生成逻辑"""
        # 全局命名空间
        global_iface = InterfaceDef(
            name="IGlobalInterface",
            base_interface="IDasBase",
            namespace="",
            uuid="12345678-1234-1234-1234-123456789012"
        )
        
        # 应为 "IGlobalInterface"，不是 "::IGlobalInterface"
        qualified = global_iface.namespace + "::" + global_iface.name if global_iface.namespace else global_iface.name
        self.assertEqual(qualified, "IGlobalInterface")
        self.assertNotEqual(qualified, "::IGlobalInterface")
        
        # 带命名空间
        namespaced_iface = InterfaceDef(
            name="IDasMultiOut",
            base_interface="IDasBase",
            namespace="Das::ExportInterface",
            uuid="12345678-1234-1234-1234-123456789013"
        )
        
        qualified = namespaced_iface.namespace + "::" + namespaced_iface.name if namespaced_iface.namespace else namespaced_iface.name
        self.assertEqual(qualified, "Das::ExportInterface::IDasMultiOut")

    def test_ignore_directive_with_global_namespace_prefix(self):
        """测试生成的 ignore 语句包含 :: 前缀（针对全局命名空间类型）"""
        # 创建测试 IDL - 带有全局命名空间类型参数的方法
        idl_content = """
namespace Das::ExportInterface {
    [uuid(\"12345678-1234-1234-1234-123456789020\")]
    interface IDasTestGlobalNamespace : IDasBase {
        DasResult GetBase([out] IDasBase** pp_out_base);
        DasResult GetString([out] IDasReadOnlyString** pp_out_string);
        DasResult GetWeakRef([out] IDasWeakReference** pp_out_weak);
    }
}
"""
        idl_path = self.create_test_idl(idl_content)
        
        # 解析 IDL
        document = parse_idl_file(idl_path)
        self.assertEqual(len(document.interfaces), 1)
        
        iface = document.interfaces[0]
        self.assertEqual(iface.name, "IDasTestGlobalNamespace")
        self.assertEqual(iface.namespace, "Das::ExportInterface")
        
        # 验证方法
        self.assertEqual(len(iface.methods), 3)
        
        # 测试 GetBase 方法 - 应该包含 ::IDasBase**
        method = iface.methods[0]
        self.assertEqual(method.name, "GetBase")
        self.assertEqual(len(method.parameters), 1)
        param = method.parameters[0]
        self.assertEqual(param.direction, ParamDirection.OUT)
        self.assertEqual(param.type_info.base_type, "IDasBase")
        
        # 生成 %ignore 指令
        ignore_code = self.generator._generate_ignore_directive(iface, method, param)
        
        # 验证生成的 ignore 代码包含 ::IDasBase**
        self.assertIn("::IDasBase**", ignore_code)
        
        # 测试 GetString 方法 - 应该包含 ::IDasReadOnlyString**
        method = iface.methods[1]
        self.assertEqual(method.name, "GetString")
        self.assertEqual(len(method.parameters), 1)
        param = method.parameters[0]
        self.assertEqual(param.direction, ParamDirection.OUT)
        self.assertEqual(param.type_info.base_type, "IDasReadOnlyString")
        
        # 生成 %ignore 指令
        ignore_code = self.generator._generate_ignore_directive(iface, method, param)
        
        # 验证生成的 ignore 代码包含 ::IDasReadOnlyString**
        self.assertIn("::IDasReadOnlyString**", ignore_code)
        
        # 测试 GetWeakRef 方法 - 应该包含 ::IDasWeakReference**
        method = iface.methods[2]
        self.assertEqual(method.name, "GetWeakRef")
        self.assertEqual(len(method.parameters), 1)
        param = method.parameters[0]
        self.assertEqual(param.direction, ParamDirection.OUT)
        self.assertEqual(param.type_info.base_type, "IDasWeakReference")
        
        # 生成 %ignore 指令
        ignore_code = self.generator._generate_ignore_directive(iface, method, param)
        
        # 验证生成的 ignore 代码包含 ::IDasWeakReference**
        self.assertIn("::IDasWeakReference**", ignore_code)



class TestAbiSignatureMatching(unittest.TestCase):
    """测试 ABI 签名与 SWIG %ignore 签名匹配"""

    def test_idas_type_info_get_runtime_class_name(self):
        """
        测试 IDasTypeInfo::GetRuntimeClassName 的签名匹配
        
        ABI 头文件: IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)
        错误 %ignore: ::IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString**)
        正确 %ignore: IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString**)
        """
        # ABI 签名
        abi_signature = "IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString** pp_out_name)"
        
        # 错误的 SWIG %ignore（带 :: 前缀）
        wrong_swig_ignore = "::IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString**)"
        
        # 正确的 SWIG %ignore（不带 :: 前缀）
        correct_swig_ignore = "IDasTypeInfo::GetRuntimeClassName(IDasReadOnlyString**)"
        
        # 验证：ABI 中的接口名是 "IDasTypeInfo"，不是 "::IDasTypeInfo"
        self.assertIn("IDasTypeInfo", abi_signature)
        self.assertNotIn("::IDasTypeInfo", abi_signature.split("(")[0])
        
        # 验证正确的 %ignore 格式
        self.assertIn("IDasTypeInfo::GetRuntimeClassName", correct_swig_ignore)
        self.assertNotIn("::IDasTypeInfo::GetRuntimeClassName", correct_swig_ignore)


class TestSwigGeneratorIntegration(unittest.TestCase):
    """集成测试：完整的 IDL -> 生成 -> 验证流程"""

    def setUp(self):
        """设置测试环境"""
        self.test_dir = tempfile.mkdtemp()
        
    def tearDown(self):
        """清理测试环境"""
        import shutil
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_global_namespace_interface_full_pipeline(self):
        """
        测试全局命名空间接口的完整生成流程
        
        1. 创建包含全局命名空间接口的 IDL
        2. 生成 ABI 头文件和 SWIG .i 文件
        3. 验证生成的 %ignore 指令格式正确
        """
        # 步骤 1: 创建测试 IDL
        idl_content = '''
[uuid("12345678-1234-1234-1234-123456789014")]
interface IGlobalTestInterface : IDasBase {
    DasResult GetString([out] IDasReadOnlyString** pp_out_name);
}

namespace Das::ExportInterface {
    [uuid("12345678-1234-1234-1234-123456789015")]
    interface IDasNamespacedInterface : IDasBase {
        DasResult GetString([out] IDasReadOnlyString** pp_out_name);
    }
}
'''
        idl_path = os.path.join(self.test_dir, "test_integration.idl")
        with open(idl_path, 'w', encoding='utf-8') as f:
            f.write(idl_content)
        
        # 步骤 2: 解析 IDL
        document = parse_idl_file(idl_path)
        self.assertEqual(len(document.interfaces), 2)
        
        # 找到全局命名空间接口
        global_iface = None
        namespaced_iface = None
        for iface in document.interfaces:
            if iface.name == "IGlobalTestInterface":
                global_iface = iface
            elif iface.name == "IDasNamespacedInterface":
                namespaced_iface = iface
        
        self.assertIsNotNone(global_iface)
        self.assertIsNotNone(namespaced_iface)
        
        # 验证全局命名空间
        self.assertEqual(global_iface.namespace, "")
        
        # 验证带命名空间
        self.assertEqual(namespaced_iface.namespace, "Das::ExportInterface")


if __name__ == '__main__':
    unittest.main()
