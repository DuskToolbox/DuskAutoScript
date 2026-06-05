import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from csharp_generator import generate_csharp_artifacts
from das_idl_parser import parse_idl


def _contract_doc():
    return parse_idl(
        """
        errorcode DasResult {
            DAS_S_OK = 0,
            DAS_E_INVALID_ARGUMENT = -1073750038,
            DAS_E_FAIL = -2147483648,
        }

        enum DasMode {
            DAS_MODE_FAST = 1,
            DAS_MODE_SAFE = 2,
        }

        struct DasSize {
            uint32_t width;
            uint32_t height;
        }

        namespace Das {
            [uuid("11111111-2222-3333-4444-555555555555")]
            interface IDasSample : IDasBase {
                DasResult GetName([out] IDasReadOnlyString** pp_out_name);
                DasResult SetName(IDasReadOnlyString* p_name);
                DasResult GetSize([out] DasSize* p_out_size);
                DasResult GetMode([out] DasMode* p_out_mode);
                DasResult GetChild([out] IDasSample** pp_out_child);
                DasResult Attach(IDasBase* p_base);
                DasResult Flush();
            }
        }
        """
    )


def _artifacts():
    return generate_csharp_artifacts(
        _contract_doc(),
        namespace_root="Das.Generated",
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=["Core.h", "DasResult.h"],
    )


def _combined_text(artifacts):
    return "\n".join(artifacts.files.values())


class TestCSharpGeneratorContract(unittest.TestCase):
    def test_d77_46_d77_47_outputs_stable_per_type_paths(self):
        artifacts = _artifacts()

        self.assertIsInstance(artifacts.files, dict)
        self.assertIn("DasGenerated.csproj", artifacts.files)
        self.assertIn("Das.Generated/DasResult.cs", artifacts.files)
        self.assertIn("Das.Generated/DasException.cs", artifacts.files)
        self.assertIn("Das.Generated/Abi/DasGuid.cs", artifacts.files)
        self.assertIn("Das.Generated/Abi/NativeMethods.cs", artifacts.files)
        self.assertIn("Das.Generated/Interop/NativeHandle.cs", artifacts.files)
        self.assertIn("Das.Generated/Wrappers/IDasSample.cs", artifacts.files)
        self.assertIn("Das.Generated/Directors/IDasSampleDirector.cs", artifacts.files)
        self.assertIn("Das.Generated/Results/IDasSampleResults.cs", artifacts.files)
        self.assertGreaterEqual(
            len([path for path in artifacts.files if path.endswith(".cs")]),
            8,
            "D-77-47: C# reduce output must not collapse into one monolithic .cs file",
        )

    def test_d77_58_generated_namespaces_use_das_root(self):
        combined = _combined_text(_artifacts())

        self.assertIn("namespace Das.Generated", combined)
        self.assertIn("namespace Das.Generated.Abi", combined)
        self.assertIn("namespace Das.Generated.Wrappers", combined)
        self.assertNotIn("namespace DAS.", combined)
        self.assertNotIn("namespace DuskAutoScript.", combined)

    def test_d77_59_public_abi_uses_intptr_not_nint(self):
        combined = _combined_text(_artifacts())

        self.assertIn("System.IntPtr", combined)
        self.assertIn("IntPtr", combined)
        self.assertNotIn("nint", combined)

    def test_d77_19_d77_21_d77_30_result_and_checked_api_shape(self):
        artifacts = _artifacts()
        combined = _combined_text(artifacts)

        self.assertIn("public enum DasResult : int", artifacts.files["Das.Generated/DasResult.cs"])
        self.assertIn("DAS_E_INVALID_ARGUMENT = -1073750038", combined)
        self.assertIn("public sealed class DasException : System.Exception", combined)
        self.assertIn("public static void OrThrow(this DasResult result", combined)
        self.assertIn("public readonly struct IDasSampleResults", combined)
        self.assertIn("public DasResult Flush()", combined)
        self.assertIn("public void FlushOrThrow()", combined)
        self.assertNotIn("Ez", combined)

    def test_d77_22_generated_text_has_no_aot_hostile_constructs(self):
        combined = _combined_text(_artifacts())

        forbidden = (
            "dynamic",
            "Assembly.Load",
            "Reflection.Emit",
            "Type.GetType",
            "GetCustomAttributes",
            "GetMethods(",
            "InvokeMember",
            "CreateDelegate",
            "RuntimeHelpers.RunClassConstructor",
        )
        for pattern in forbidden:
            with self.subTest(pattern=pattern):
                self.assertNotIn(pattern, combined)

    def test_d77_23_d77_25_d77_26_modern_interop_has_tfm_gates(self):
        native_methods = _artifacts().files["Das.Generated/Abi/NativeMethods.cs"]

        self.assertIn("#if NET5_0_OR_GREATER", native_methods)
        self.assertIn("#if NET7_0_OR_GREATER", native_methods)
        self.assertIn("#if NET8_0_OR_GREATER", native_methods)
        self.assertIn("#if NETFRAMEWORK", native_methods)
        self.assertIn("LibraryImport", native_methods)
        self.assertIn("delegate* unmanaged", native_methods)


if __name__ == "__main__":
    unittest.main()
