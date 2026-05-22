import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).parent


def _write_minimal_idl(idl_dir: Path) -> None:
    idl_dir.mkdir(parents=True, exist_ok=True)
    (idl_dir / "Core.idl").write_text(
        """
        module {
            [export, c_abi] void DasLogInfoU8(const char* p_string);
        }
        """,
        encoding="utf-8",
    )


class TestNapiBatchConfig(unittest.TestCase):
    def _run_gen_config(self, temp: Path, *extra_args: str) -> subprocess.CompletedProcess:
        idl_dir = temp / "idl"
        _write_minimal_idl(idl_dir)
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT_DIR / "gen_batch_config.py"),
                "--output",
                str(temp / "batch.json"),
                "--idl-dir",
                str(idl_dir),
                "--idl-files",
                "Core.idl",
                "--raw-output-dir",
                str(temp / "abi"),
                "--wrapper-output-dir",
                str(temp / "wrapper"),
                "--header-output-dir",
                str(temp / "header"),
                *extra_args,
            ],
            capture_output=True,
            text=True,
        )

    def test_node_reduce_config_writes_explicit_names(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)

            result = self._run_gen_config(
                temp,
                "--languages",
                "Node",
                "--node-output-dir",
                str(temp / "node"),
                "--node-package-name",
                "das-core",
                "--node-addon-name",
                "das_core_napi",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads((temp / "batch.json").read_text(encoding="utf-8"))
            reduce_config = config["reduce"]
            self.assertEqual(reduce_config["node_output_dir"], str(temp / "node"))
            self.assertEqual(reduce_config["node_package_name"], "das-core")
            self.assertEqual(reduce_config["node_addon_name"], "das_core_napi")
            self.assertEqual(reduce_config["node_idl_dir"], str(temp / "idl"))
            self.assertEqual(reduce_config["node_idl_files"], ["Core.idl"])
            self.assertNotIn(".sisyphus", json.dumps(config))

    def test_node_only_language_does_not_enable_swig(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)

            result = self._run_gen_config(
                temp,
                "--languages",
                "Node",
                "--swig-output-dir",
                str(temp / "swig"),
                "--node-output-dir",
                str(temp / "node"),
                "--node-package-name",
                "das-core",
                "--node-addon-name",
                "das_core_napi",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            config = json.loads((temp / "batch.json").read_text(encoding="utf-8"))
            self.assertEqual(len(config["tasks"]), 1)
            task = config["tasks"][0]
            self.assertNotIn("--swig-output-dir", task)
            self.assertNotIn("--swig", task)

    def test_node_reduce_names_are_required(self):
        cases = [
            (
                [
                    "--languages",
                    "Node",
                    "--node-output-dir",
                    "node",
                    "--node-addon-name",
                    "das_core_napi",
                ],
                "--node-package-name",
            ),
            (
                [
                    "--languages",
                    "Node",
                    "--node-output-dir",
                    "node",
                    "--node-package-name",
                    "das-core",
                ],
                "--node-addon-name",
            ),
        ]

        for extra_args, missing_flag in cases:
            with self.subTest(missing_flag=missing_flag):
                with tempfile.TemporaryDirectory() as temp_dir:
                    result = self._run_gen_config(Path(temp_dir), *extra_args)

                self.assertEqual(result.returncode, 2)
                self.assertIn(missing_flag, result.stderr)

    def test_batch_list_outputs_includes_node_support_files_once(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            node_dir = temp / "node"

            result = self._run_gen_config(
                temp,
                "--languages",
                "Node",
                "--node-output-dir",
                str(node_dir),
                "--node-package-name",
                "das-core",
                "--node-addon-name",
                "das_core_napi",
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            list_result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT_DIR / "das_idl_batch_gen.py"),
                    "--config",
                    str(temp / "batch.json"),
                    "--list-outputs",
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(list_result.returncode, 0, list_result.stderr)
            outputs = list_result.stdout.splitlines()
            node_outputs = [item for item in outputs if "/node/" in item]
            self.assertCountEqual(
                node_outputs,
                [
                    f"{node_dir}/das_core_napi_export.cpp".replace("\\", "/"),
                    f"{node_dir}/das_core_napi_export.d.ts".replace("\\", "/"),
                    f"{node_dir}/das_core_napi_export.js".replace("\\", "/"),
                ],
            )
            self.assertEqual(len(node_outputs), len(set(node_outputs)))
            self.assertFalse(
                any("das_core_napi_napi_export" in item for item in outputs)
            )


if __name__ == "__main__":
    unittest.main()
