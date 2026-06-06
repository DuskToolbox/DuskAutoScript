import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from csharp_generator import generate_csharp_artifacts as _generate_csharp_artifacts
from das_idl_parser import parse_idl


ROOT = Path(__file__).resolve().parents[2]
TEST_DAS_NATIVE_MODULE = "DasCoreNativeForTest"
TEST_CSHARP_NATIVE_SUPPORT_MODULE = "DasCoreCSharpSupportForTest"

BANNED_PUBLIC_HELPERS = (
    "DasAddRef",
    "DasRelease",
    "GetIDasVariantVectorString",
    "GetIDasVariantVectorComponent",
    "PushBackIDasVariantVectorString",
    "PushBackIDasVariantVectorComponent",
    "DispatchIDasComponent",
)


def generate_csharp_artifacts(*args, **kwargs):
    kwargs.setdefault("das_native_module_name", TEST_DAS_NATIVE_MODULE)
    kwargs.setdefault(
        "csharp_native_support_module_name",
        TEST_CSHARP_NATIVE_SUPPORT_MODULE,
    )
    return _generate_csharp_artifacts(*args, **kwargs)


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _strip_swig_hidden_blocks(text: str) -> str:
    visible_lines: list[str] = []
    hidden_depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "#ifndef SWIG":
            hidden_depth += 1
            continue
        if hidden_depth:
            if stripped.startswith("#if"):
                hidden_depth += 1
            elif stripped.startswith("#endif"):
                hidden_depth -= 1
            continue
        visible_lines.append(line)
    return "\n".join(visible_lines)


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
        package_name="Das.Generated",
        project_name="DasGenerated",
        idl_header_names=["Core.h"],
    )
    return "\n".join(artifacts.files.values())


class TestNativeUtf16StringAbi(unittest.TestCase):
    def test_d77_37_old_wchar_abi_names_removed(self):
        source = "\n".join(
            [
                _read("include/das/DasString.hpp"),
                _read(
                    "das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/DasStringImpl.h"
                ),
                _read("das/Core/ForeignInterfaceHost/src/DasStringImpl.cpp"),
            ]
        )

        for old_name in (
            "GetW",
            "SetW",
            "SetSwigW",
            "CreateIDasStringFromWChar",
            "CreateIDasReadOnlyStringFromWChar",
        ):
            with self.subTest(old_name=old_name):
                self.assertNotIn(old_name, source)

    def test_d77_37_d77_40_native_contract_uses_explicit_utf16_code_units(self):
        header = _read("include/das/DasString.hpp")

        self.assertRegex(header, r"using\s+DasUtf16CodeUnit\s*=\s*uint16_t\s*;")
        self.assertRegex(
            header,
            r"GetUtf16\s*\(\s*const\s+char16_t\*\*\s+\w+\s*,\s*size_t\*\s+\w+\s*\)",
        )
        self.assertRegex(
            header,
            r"SetUtf16\s*\(\s*const\s+char16_t\*\s+\w+\s*,\s*size_t\s+\w+\s*\)",
        )
        self.assertRegex(
            header,
            r"CreateIDasReadOnlyStringFromUtf16WithLength\s*\(\s*const\s+DasUtf16CodeUnit\*\s+\w+\s*,\s*size_t\s+\w+",
        )
        self.assertRegex(
            header,
            r"CreateIDasStringFromUtf16WithLength\s*\(\s*const\s+DasUtf16CodeUnit\*\s+\w+\s*,\s*size_t\s+\w+",
        )
        self.assertNotIn("GetIDasReadOnlyStringUtf16", header)

    def test_d77_39_d77_41_raw_utf16_header_api_is_swig_hidden(self):
        header = _read("include/das/DasString.hpp")
        swig_visible_header = _strip_swig_hidden_blocks(header)

        for raw_pattern in (
            "GetUtf16",
            "SetUtf16",
            "CreateIDasStringFromUtf16WithLength",
            "CreateIDasReadOnlyStringFromUtf16WithLength",
            "GetIDasReadOnlyStringUtf16",
            "DasUtf16CodeUnit*",
            "char16_t**",
        ):
            with self.subTest(raw_pattern=raw_pattern):
                self.assertNotIn(raw_pattern, swig_visible_header)

    def test_d77_39_d77_41_swig_sources_do_not_reference_raw_utf16_api(self):
        swig_text = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (ROOT / "SWIG").rglob("*.i")
        )

        raw_api = re.compile(
            r"GetUtf16|SetUtf16|CreateIDas(?:ReadOnly)?StringFromUtf16WithLength"
        )
        self.assertIsNone(raw_api.search(swig_text))

    def test_d77_42_d77_44_implementation_uses_explicit_length_utf16(self):
        impl = _read("das/Core/ForeignInterfaceHost/src/DasStringImpl.cpp")

        self.assertIn("length", impl)
        self.assertRegex(
            impl,
            r"UnicodeString\s*\(\s*reinterpret_cast<const char16_t\*>\(\s*p_utf16_string\s*\)\s*,\s*int_length\s*\)",
        )
        self.assertNotIn("u_strlen(p_utf16_string)", impl)
        self.assertNotIn("fromWCS", impl)
        self.assertNotIn("toWCS", impl)

    def test_public_headers_do_not_reintroduce_csharp_helper_abi(self):
        public_api = "\n".join(
            (
                _read("include/das/DasApi.h"),
                _read("das/Core/ForeignInterfaceHost/include/das/Core/ForeignInterfaceHost/DasStringImpl.h"),
            )
        )

        for helper_name in BANNED_PUBLIC_HELPERS:
            with self.subTest(helper_name=helper_name):
                self.assertNotIn(
                    helper_name,
                    public_api,
                    f"{helper_name} belongs to the generated C# native support boundary; "
                    "it must not be reintroduced into public DAS headers.",
                )

    def test_get_idas_read_only_string_utf16_is_only_in_support_abi_path(self):
        public_string_header = _read("include/das/DasString.hpp")
        private_string_helper = "GetIDasReadOnlyStringUtf16"

        self.assertNotIn(
            private_string_helper,
            public_string_header,
            "GetIDasReadOnlyStringUtf16 is now exported via C# native support helpers "
            f"({TEST_CSHARP_NATIVE_SUPPORT_MODULE}) and must not remain in public headers.",
        )


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

        self.assertIn("ArgumentNullException.ThrowIfNull(value)", combined)
        self.assertIn('EmbeddedNul = "left\\0right"', combined)
        self.assertIn('UnpairedSurrogate = "\\ud800"', combined)
        self.assertIn("checked((nuint)value.Length)", combined)


if __name__ == "__main__":
    unittest.main()
