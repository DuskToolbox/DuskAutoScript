"""
IpcBaseTypeMapper uint8_t/uint16_t/int8_t/int16_t 支持回归测试

背景：
    IDL 中 DasDate 结构体使用 uint16_t / uint8_t 字段（带 _t 后缀）。
    IpcBaseTypeMapper.TYPE_MAP 原本只包含 uint8/uint16/int8/int16（无 _t 后缀），
    而 ProxyTypeMapper 又未覆盖 TYPE_MAP，导致 proxy 端遇到 DasDate 这类结构体时
    在 _generate_struct_deserialize 中拿不到 type_info，输出
    "// TODO: Unsupported field type uint16_t" 之类的占位注释，
    GetNextExecutionTime 的 out 参数因此始终为零值。

本测试确保 IpcBaseTypeMapper 自身即可正确解析这些 _t 类型，
从而使 ProxyTypeMapper / StubTypeMapper 行为一致。
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from das_idl_parser import IdlDocument
from ipc_type_mapper import IpcBaseTypeMapper, ProxyTypeMapper


class TestIpcBaseTypeMapperUintTSupport(unittest.TestCase):
    """验证 IpcBaseTypeMapper 能识别 _t 后缀的 8/16 位整型。"""

    EXPECTED = {
        'int8_t': ('int8_t', 'WriteInt8', 'ReadInt8'),
        'int16_t': ('int16_t', 'WriteInt16', 'ReadInt16'),
        'uint8_t': ('uint8_t', 'WriteUInt8', 'ReadUInt8'),
        'uint16_t': ('uint16_t', 'WriteUInt16', 'ReadUInt16'),
    }

    def _make_mapper(self):
        return IpcBaseTypeMapper(IdlDocument())

    def test_get_type_info_returns_correct_tuple_for_each_t_type(self):
        mapper = self._make_mapper()
        for idl_type, expected in self.EXPECTED.items():
            with self.subTest(idl_type=idl_type):
                info = mapper.get_type_info(idl_type)
                self.assertIsNotNone(
                    info,
                    f"{idl_type} 应被 IpcBaseTypeMapper 识别为基本类型，"
                    f"避免 proxy 反序列化结构体字段时输出 TODO 注释",
                )
                cpp_type, write_method, read_method, is_struct = info
                self.assertEqual(
                    (cpp_type, write_method, read_method),
                    expected,
                    f"{idl_type} 映射条目不正确",
                )
                self.assertFalse(
                    is_struct,
                    f"{idl_type} 应被识别为基本类型而非 struct",
                )

    def test_type_map_contains_all_t_variants(self):
        """直接断言 TYPE_MAP 字典，避免 get_type_info 的其它分支掩盖缺失。"""
        type_map = IpcBaseTypeMapper.TYPE_MAP
        for idl_type, expected in self.EXPECTED.items():
            with self.subTest(idl_type=idl_type):
                self.assertIn(idl_type, type_map, f"TYPE_MAP 缺少 {idl_type} 条目")
                self.assertEqual(type_map[idl_type], expected)

    def test_is_basic_type_true_for_t_variants(self):
        mapper = self._make_mapper()
        for idl_type in self.EXPECTED:
            with self.subTest(idl_type=idl_type):
                self.assertTrue(
                    mapper.is_basic_type(idl_type),
                    f"is_basic_type({idl_type!r}) 应返回 True",
                )


class TestProxyTypeMapperInheritsTSupport(unittest.TestCase):
    """ProxyTypeMapper 未覆盖 TYPE_MAP，必须继承到 base 的 _t 条目。"""

    def test_proxy_mapper_inherits_t_variants(self):
        mapper = ProxyTypeMapper(IdlDocument())
        for idl_type, expected in TestIpcBaseTypeMapperUintTSupport.EXPECTED.items():
            with self.subTest(idl_type=idl_type):
                info = mapper.get_type_info(idl_type)
                self.assertIsNotNone(
                    info,
                    f"ProxyTypeMapper 应继承 base TYPE_MAP 中的 {idl_type}",
                )
                cpp_type, write_method, read_method, is_struct = info
                self.assertEqual(
                    (cpp_type, write_method, read_method),
                    expected,
                )
                self.assertFalse(is_struct)


if __name__ == '__main__':
    unittest.main()
