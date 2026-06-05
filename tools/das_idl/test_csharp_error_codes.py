import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from das_idl_parser import parse_idl_file


ROOT = Path(__file__).resolve().parents[2]
DAS_RESULT_IDL = ROOT / "idl" / "DasResult.idl"
GLOBAL_ERROR_MESSAGES_CPP = (
    ROOT / "das" / "Core" / "Exceptions" / "src" / "GlobalErrorMessages.cpp"
)

GENERIC_CSHARP_ERROR = ("DAS_E_CSHARP_ERROR", -1073750021)
EXPECTED_DEDICATED_CSHARP_ERRORS = {
    "DAS_E_CSHARP_MISSING_RUNTIMECONFIG": -1073750044,
    "DAS_E_CSHARP_UNSUPPORTED_TFM": -1073750045,
    "DAS_E_CSHARP_HOSTFXR_INIT_FAILED": -1073750046,
    "DAS_E_CSHARP_COM_CLR_INIT_FAILED": -1073750047,
    "DAS_E_CSHARP_ENTRYPOINT_MISSING": -1073750048,
    "DAS_E_CSHARP_PLUGIN_INIT_FAILED": -1073750049,
}
RESERVED_CSHARP_SEGMENT_START = -1073800000
RESERVED_CSHARP_SEGMENT_END = -1073800099


def _das_result_values():
    doc = parse_idl_file(str(DAS_RESULT_IDL))
    values = {}
    for error_code in doc.error_codes:
        if error_code.name != "DasResult":
            continue
        for value in error_code.values:
            values[value.name] = value.value
    return values


def _message_table_entries():
    text = GLOBAL_ERROR_MESSAGES_CPP.read_text(encoding="utf-8")
    return dict(
        re.findall(
            r"\{\s*(DAS_E_CSHARP_[A-Z0-9_]+)\s*,\s*\"([^\"]+)\"\s*\}",
            text,
        )
    )


class TestCSharpErrorCodeContract(unittest.TestCase):
    def test_generic_csharp_error_remains_compatibility_constant(self):
        values = _das_result_values()

        name, expected_value = GENERIC_CSHARP_ERROR
        self.assertIn(name, values)
        self.assertEqual(values[name], expected_value)

    def test_d77_16_dedicated_csharp_errors_use_locked_values(self):
        values = _das_result_values()

        dedicated = {
            name: value
            for name, value in values.items()
            if name.startswith("DAS_E_CSHARP_") and name != GENERIC_CSHARP_ERROR[0]
        }
        self.assertEqual(dedicated, EXPECTED_DEDICATED_CSHARP_ERRORS)

    def test_d77_16a_large_csharp_error_family_uses_reserved_segment(self):
        values = _das_result_values()

        dedicated = {
            name: value
            for name, value in values.items()
            if name.startswith("DAS_E_CSHARP_") and name != GENERIC_CSHARP_ERROR[0]
        }
        if len(dedicated) <= 10:
            return

        for name, value in dedicated.items():
            with self.subTest(name=name):
                self.assertGreaterEqual(value, RESERVED_CSHARP_SEGMENT_START)
                self.assertLessEqual(value, RESERVED_CSHARP_SEGMENT_END)

    def test_d77_16_error_values_are_unique(self):
        doc = parse_idl_file(str(DAS_RESULT_IDL))
        seen = {}
        duplicates = {}

        for error_code in doc.error_codes:
            if error_code.name != "DasResult":
                continue
            for value in error_code.values:
                prior = seen.setdefault(value.value, value.name)
                if prior != value.name:
                    duplicates.setdefault(value.value, [prior]).append(value.name)

        self.assertEqual(duplicates, {})

    def test_dedicated_csharp_errors_have_predefined_messages(self):
        messages = _message_table_entries()

        for name in EXPECTED_DEDICATED_CSHARP_ERRORS:
            with self.subTest(name=name):
                self.assertIn(name, messages)
                self.assertTrue(messages[name].strip())


if __name__ == "__main__":
    unittest.main()
