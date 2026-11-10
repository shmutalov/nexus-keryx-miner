"""CPU-only wrapper generation checks; no runtime loading or device access."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/runtime/add_length_aware_wrapper.py"
spec = importlib.util.spec_from_file_location("sized_wrapper", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SizedWrapperTests(unittest.TestCase):
    def test_both_pinned_wrappers_keep_legacy_and_add_sized_exports(self):
        for suffix in ("keryx_llama.cpp", "abi9/keryx_llama.cpp"):
            source = (ROOT / "vendor/keryx-trace-abi6/tools/keryx-llama" / suffix).read_text()
            generated = module.transform(source)
            for name in ("generate", "generate_traced"):
                self.assertEqual(generated.count("KERYX_EXPORT int keryx_llama_" + name + "("), 1)
                self.assertEqual(generated.count("KERYX_EXPORT int keryx_llama_" + name + "_n("), 1)
            self.assertIn("size_t rest_len", generated)
            self.assertIn("prompt_len > 60U * 1024", generated)
            self.assertIn("rest_len > 60U * 1024", generated)
            # A second transformation is refused instead of adding ambiguous exports.
            with self.assertRaises(ValueError):
                module.transform(generated)

    def test_hash_mismatch_and_original_overwrite_are_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertFalse(root.is_relative_to("/tmp"))
            source = ROOT / "vendor/keryx-trace-abi6/tools/keryx-llama/abi9/keryx_llama.cpp"
            original = source.read_bytes()
            copy = root / "base.cpp"
            copy.write_bytes(original)
            dest = root / "generated.cpp"
            digest = hashlib.sha256(original).hexdigest()
            for output, expected in ((dest, "0" * 64), (copy, digest)):
                result = subprocess.run([sys.executable, str(TOOL), "--input", str(copy),
                    "--output", str(output), "--expected-sha256", expected], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(copy.read_bytes(), original)
                self.assertFalse(dest.exists())


if __name__ == "__main__":
    unittest.main()
