import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import parse_idl
from das_lua_export import _merge_documents
from swig_lua_generator import LuaSwigGenerator


class TestLuaExportNaming(unittest.TestCase):
    def test_gen_batch_config_writes_lua_open_module_name(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            config_path = temp / "batch.json"

            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).parent / "gen_batch_config.py"),
                    "--output",
                    str(config_path),
                    "--idl-dir",
                    str(temp),
                    "--idl-files",
                    "IDasExample.idl",
                    "--raw-output-dir",
                    str(temp / "abi"),
                    "--wrapper-output-dir",
                    str(temp / "wrapper"),
                    "--header-output-dir",
                    str(temp / "header"),
                    "--languages",
                    "Lua",
                    "--lua-output-dir",
                    str(temp / "lua"),
                    "--lua-name",
                    "GeneratedFileBase",
                    "--lua-open-module-name",
                    "CMakeOpenName",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads(config_path.read_text(encoding="utf-8"))
            reduce_config = config["reduce"]
            self.assertEqual(reduce_config["lua_name"], "GeneratedFileBase")
            self.assertEqual(
                reduce_config["lua_open_module_name"],
                "CMakeOpenName",
            )

    def test_luaopen_function_uses_explicit_open_module_name(self):
        doc = parse_idl("module { void Init(); }")

        generated = LuaSwigGenerator()._generate_luaopen_function(
            doc,
            [],
            "DAS_C_API",
            "CMakeOpenName",
        )

        self.assertIn("DAS_C_API int luaopen_CMakeOpenName(lua_State* L)", generated)
        self.assertNotIn("luaopen_Idl", generated)

    def test_merge_documents_preserves_anonymous_module_functions(self):
        first = parse_idl("module { void First(); }")
        second = parse_idl("module { void Second(); }")

        merged = _merge_documents([first, second])

        module_functions = [
            func.name
            for module in merged.modules
            for func in module.functions
        ]
        self.assertEqual(module_functions, ["First", "Second"])

    def test_lua_generator_skips_c_abi_module_functions_from_public_surfaces(self):
        doc = parse_idl(
            """
            errorcode DasResult {
                DAS_S_OK = 0,
            }

            module {
                [export, c_abi] void CAbiOnly(const char* p_string);
                [export] void PublicNoOut(uint32_t value);
                [export, swig_ret] DasResult PublicRet([out] uint32_t* p_out_value);
            }
            """
        )

        generator = LuaSwigGenerator()
        binding = generator._generate_module_binding(doc)
        stub = generator._generate_lua_stub([], doc)

        self.assertNotIn("CAbiOnly", binding)
        self.assertNotIn("CAbiOnly", stub)
        self.assertIn(
            'lua.set_function("PublicNoOut", &PublicNoOut);',
            binding,
        )
        self.assertIn("function PublicNoOut(value) end", stub)
        self.assertIn('lua.set_function("PublicRet",', binding)
        self.assertIn("auto ret = PublicRet();", binding)
        self.assertIn("ret.GetErrorCode()", binding)
        self.assertIn("ret.GetValue()", binding)
        self.assertIn("function PublicRet() end", stub)


if __name__ == "__main__":
    unittest.main()
