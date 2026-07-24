import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from csharp_generator import generate_csharp_artifacts as _generate_csharp_artifacts
from das_idl_parser import parse_idl


TEST_DAS_NATIVE_MODULE = "DasCoreNativeForTest"
TEST_CSHARP_NATIVE_SUPPORT_MODULE = "DasCoreCSharpSupportForTest"


def generate_csharp_artifacts(*args, **kwargs):
    kwargs.setdefault("das_native_module_name", TEST_DAS_NATIVE_MODULE)
    kwargs.setdefault(
        "csharp_native_support_module_name",
        TEST_CSHARP_NATIVE_SUPPORT_MODULE,
    )
    return _generate_csharp_artifacts(*args, **kwargs)


def _contract_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_POINTER = -1073750020,
            DAS_E_INVALID_ARGUMENT = -1073750038,
        }

        namespace Das {
            [uuid("11111111-2222-3333-4444-555555555555")]
            interface IDasStringConsumer : IDasBase {
                DasResult SetName(IDasReadOnlyString* p_name);
                DasResult GetName([out] IDasReadOnlyString** pp_out_name);
            }
        }
        """
    )


def _artifacts_text() -> str:
    artifacts = generate_csharp_artifacts(
        _contract_doc(),
        namespace_root="Das.Generated",
        idl_header_names=["Core.h"],
    )
    return "\n".join(artifacts.files.values())


class TestCSharpGeneratorUtf16Contract(unittest.TestCase):
    def test_d77_38_d77_40_csharp_abi_uses_ushort_pointer_helpers(self):
        combined = _artifacts_text()

        self.assertIn("ushort*", combined)
        self.assertIn("CreateIDasReadOnlyStringFromUtf16WithLength", combined)
        self.assertIn("CreateIDasStringFromUtf16WithLength", combined)
        self.assertIn("DasCSharpGetIDasReadOnlyStringUtf16", combined)
        self.assertNotIn('EntryPoint = "GetIDasReadOnlyStringUtf16"', combined)
        self.assertNotIn("LPWStr", combined)
        self.assertNotIn("string value)", combined)

    def test_d77_34_d77_44_d77_48_csharp_string_fixtures_are_lossless(self):
        combined = _artifacts_text()

        self.assertIn("if (value is null)", combined)
        self.assertIn("throw new ArgumentNullException(nameof(value));", combined)
        self.assertIn('EmbeddedNul = "left\\0right"', combined)
        self.assertIn('UnpairedSurrogate = "\\ud800"', combined)
        self.assertIn("checked((nuint)value.Length)", combined)


if __name__ == "__main__":
    unittest.main()
