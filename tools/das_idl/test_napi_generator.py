import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import parse_idl
from napi_generator import (
    NapiGenerator,
    classify_module_function,
    generate_napi_artifacts,
)


def _sample_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_ARGUMENT = -1073750038,
        }

        enum DasMode {
            DasModeFast = 1,
            DasModeSafe = 2,
        }

        [uuid("12345678-1234-1234-1234-123456789012")]
        interface IDasExample : IDasBase {
            DasResult Run();
        }

        module {
            [export, c_abi] void DasLogInfoU8(const char* p_string);
            [export, c_abi] DasResult DasSetIpcTimeout(uint32_t timeout_ms);
            [export, c_abi] DasResult DasUnregisterMainProcessServiceByName(const char* name);
            [export, c_abi] DasResult DasUseGuid(const DasGuid& guid);
            [export, c_abi] DasResult DasGetIpcTimeout([out] uint32_t* p_out_timeout_ms);
            [export, c_abi] DasResult DasQueryMainProcessInterface(const DasGuid& iid, [out] IDasBase** pp_out_object);
        }
        """
    )


def _function(doc, name):
    for module in doc.modules:
        for func in module.functions:
            if func.name == name:
                return func
    raise AssertionError(f"missing module function {name}")


class TestNapiGenerator(unittest.TestCase):
    def test_napi_artifacts_share_export_names(self):
        artifacts = generate_napi_artifacts(
            _sample_doc(),
            package_name="das-core",
            addon_name="das_core_napi",
        )

        self.assertIn('#include <napi.h>', artifacts.cpp)
        self.assertIn('#include "das/IDasBase.h"', artifacts.cpp)
        self.assertIn('#include "das/DasString.hpp"', artifacts.cpp)
        self.assertIn('#include "das/DasApi.h"', artifacts.cpp)
        self.assertIn("NODE_API_MODULE(das_core_napi, Init)", artifacts.cpp)
        self.assertIn("require(path.join(__dirname, 'das_core_napi.node'))", artifacts.js)
        self.assertIn("Failed to load DAS native addon das_core_napi.node", artifacts.js)

        for name in (
            "DasLogInfoU8",
            "DasSetIpcTimeout",
            "DasUnregisterMainProcessServiceByName",
            "DasGetIpcTimeout",
            "DasQueryMainProcessInterface",
        ):
            with self.subTest(name=name):
                self.assertIn(name, artifacts.cpp)
                self.assertIn(name, artifacts.dts)
                self.assertIn(name, artifacts.js)

        self.assertIn("export const DasMode", artifacts.dts)
        self.assertIn("DasModeFast: 1", artifacts.dts)
        self.assertIn("export const DasResult", artifacts.dts)
        self.assertIn("DAS_E_INVALID_ARGUMENT: -1073750038", artifacts.dts)

    def test_napi_primitive_typescript_mapping(self):
        doc = parse_idl(
            """
            module {
                [export, c_abi] DasResult PrimitiveMap(
                    int64_t signed64,
                    uint64_t unsigned64,
                    size_t size,
                    int32_t signed32,
                    uint32_t unsigned32,
                    float ratio,
                    double score,
                    bool enabled,
                    DasBool das_enabled);
            }
            """
        )

        artifacts = generate_napi_artifacts(
            doc,
            package_name="das-core",
            addon_name="das_core_napi",
        )

        self.assertIn("signed64: bigint", artifacts.dts)
        self.assertIn("unsigned64: bigint", artifacts.dts)
        self.assertIn("size: bigint", artifacts.dts)
        self.assertIn("signed32: number", artifacts.dts)
        self.assertIn("unsigned32: number", artifacts.dts)
        self.assertIn("ratio: number", artifacts.dts)
        self.assertIn("score: number", artifacts.dts)
        self.assertIn("enabled: boolean", artifacts.dts)
        self.assertIn("das_enabled: boolean", artifacts.dts)

    def test_napi_das_guid_mapping_and_helpers(self):
        artifacts = generate_napi_artifacts(
            _sample_doc(),
            package_name="das-core",
            addon_name="das_core_napi",
        )

        self.assertIn(
            "export type DasGuid = string & { readonly __dasGuidBrand: unique symbol };",
            artifacts.dts,
        )
        self.assertIn("export function guid(value: string): DasGuid;", artifacts.dts)
        self.assertIn("DasMakeDasGuid(value.c_str(), &guid)", artifacts.cpp)
        self.assertIn("DasGuidToString(&guid", artifacts.cpp)
        self.assertIn("guid(value)", artifacts.js)

    def test_napi_out_and_inout_parameters_are_unsupported(self):
        doc = parse_idl(
            """
            module {
                [export, c_abi] DasResult OutScalar([out] uint32_t* p_out_value);
                [export, c_abi] DasResult InOutObject([inout] IDasBase** pp_value);
            }
            """
        )

        for name in ("OutScalar", "InOutObject"):
            support = classify_module_function(_function(doc, name))
            with self.subTest(name=name):
                self.assertFalse(support.supported)
                self.assertIn("out/inout parameters are deferred to Phase 74", support.reason)

    def test_napi_future_capabilities_are_consistently_unsupported(self):
        doc = parse_idl(
            """
            struct DasPoint {
                int32_t x;
                int32_t y;
            }

            [uuid("12345678-1234-1234-1234-123456789012")]
            interface IDasFuture : IDasBase {
                DasResult Run();
            }

            module {
                [export, c_abi] DasResult UseObject(IDasBase* p_object);
                [export, c_abi] DasResult UseText(IDasReadOnlyString* p_text);
                [export, c_abi] DasResult UseStruct(const DasPoint& point);
                [export, c_abi] DasResult UseBinary(IDasBinaryBuffer* p_buffer);
                [export, c_abi] DasResult StartNodeHostBootstrap(const char* script);
            }
            """
        )

        supported_text = classify_module_function(_function(doc, "UseText"))
        self.assertTrue(supported_text.supported, supported_text.reason)

        expected_reasons = {
            "UseObject": "interface pointer inputs are deferred to Phase 74",
            "UseStruct": "struct values are deferred to Phase 74",
            "UseBinary": "binary buffer inputs are deferred to Phase 74",
            "StartNodeHostBootstrap": "Node host/bootstrap is deferred to Phase 75",
        }
        for name, reason in expected_reasons.items():
            support = classify_module_function(_function(doc, name))
            with self.subTest(name=name):
                self.assertFalse(support.supported)
                self.assertIn(reason, support.reason)

        artifacts = generate_napi_artifacts(
            doc,
            package_name="das-core",
            addon_name="das_core_napi",
        )
        for name, reason in expected_reasons.items():
            with self.subTest(name=name):
                self.assertIn(name, artifacts.cpp)
                self.assertIn(name, artifacts.dts)
                self.assertIn(name, artifacts.js)
                self.assertIn(reason, artifacts.cpp)
                self.assertIn(reason, artifacts.dts)
                self.assertIn(reason, artifacts.js)
        self.assertIn("class INapiDasFuture", artifacts.dts)
        self.assertIn("director support is deferred to Phase 74", artifacts.dts)
        self.assertIn("INapiDasFuture", artifacts.js)

    def test_napi_generated_text_avoids_forbidden_patterns(self):
        artifacts = generate_napi_artifacts(
            _sample_doc(),
            package_name="das-core",
            addon_name="das_core_napi",
        )
        combined = "\n".join([artifacts.cpp, artifacts.dts, artifacts.js])

        forbidden = (
            "module_name",
            "mod.name",
            ".module_name",
            "SwigLangGenerator",
            "#include <node.h>",
            "v8::",
            "NAN_",
            "NAPI_EXPERIMENTAL",
            "node_api_symbol_for",
            "node_api_create_buffer_from_arraybuffer",
            "node_api_syntax_error",
        )
        for pattern in forbidden:
            with self.subTest(pattern=pattern):
                self.assertNotIn(pattern, combined)

    def test_napi_generator_public_class_matches_function_helper(self):
        doc = _sample_doc()

        from_class = NapiGenerator(
            package_name="das-core",
            addon_name="das_core_napi",
        ).generate(doc)
        from_helper = generate_napi_artifacts(
            doc,
            package_name="das-core",
            addon_name="das_core_napi",
        )

        self.assertEqual(from_class, from_helper)


class TestNapiExportCli(unittest.TestCase):
    def test_napi_cli_requires_all_names(self):
        script = Path(__file__).parent / "das_napi_export.py"
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "--idl-dir",
                    str(temp),
                    "--output",
                    str(temp / "out"),
                    "--idl-files",
                    "Core.idl",
                ],
                capture_output=True,
                text=True,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--package-name", result.stderr)
        self.assertIn("--addon-name", result.stderr)

    def test_napi_cli_merges_idl_and_uses_addon_export_stem(self):
        script = Path(__file__).parent / "das_napi_export.py"
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            idl_dir = temp / "idl"
            output_dir = temp / "out"
            idl_dir.mkdir()
            (idl_dir / "Result.idl").write_text(
                "errorcode DasResult { DAS_S_OK = 0, }",
                encoding="utf-8",
            )
            (idl_dir / "Core.idl").write_text(
                """
                module {
                    [export, c_abi] void DasLogInfoU8(const char* p_string);
                }
                """,
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "--idl-dir",
                    str(idl_dir),
                    "--output",
                    str(output_dir),
                    "--package-name",
                    "das-core",
                    "--addon-name",
                    "das_core_napi",
                    "--idl-files",
                    "Result.idl",
                    "Core.idl",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            cpp = output_dir / "das_core_napi_export.cpp"
            dts = output_dir / "das_core_napi_export.d.ts"
            js = output_dir / "das_core_napi_export.js"
            self.assertTrue(cpp.exists())
            self.assertTrue(dts.exists())
            self.assertTrue(js.exists())
            self.assertFalse((output_dir / "das_core_napi_napi_export.cpp").exists())
            self.assertIn(
                '#include "das/_autogen/idl/abi/Core.h"',
                cpp.read_text(encoding="utf-8"),
            )
            self.assertIn(
                '#include "das/_autogen/idl/header/Core.generated.h"',
                cpp.read_text(encoding="utf-8"),
            )
            self.assertIn("DAS_S_OK", dts.read_text(encoding="utf-8"))
            self.assertIn("DasLogInfoU8", js.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
