"""Verify the actual audit CLI admission, including nonzero failure status."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

AUDIT = Path(__file__).with_name('audit.py').resolve()


class AuditCLI(unittest.TestCase):
    def run_audit(self, source, flags=('--check',), admission=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'src').mkdir()
            (root / 'src/OverlayTest.cpp').write_text(source, encoding='utf-8')
            if admission is not None:
                (root / 'src/FrameGen').mkdir()
                (root / 'src/FrameGen/NeuralAdmission.h').write_text(admission, encoding='utf-8')
            dictionary = root / 'dictionary.ini'
            dictionary.write_text('[Strings]\n', encoding='utf-8')
            before = dictionary.read_bytes()
            result = subprocess.run([sys.executable, str(AUDIT), str(dictionary), *flags],
                                    cwd=root, capture_output=True, text=True)
            self.assertEqual(dictionary.read_bytes(), before, 'checks must not edit dictionaries')
            return result

    def test_dynamic_admission_reasons_are_audited(self):
        result = self.run_audit('TR(view.reason);', admission='inline constexpr const char* reason = "GTX 10 admission reason";')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 missing', result.stdout)

    def test_unwrapped_prose_fails(self):
        result = self.run_audit('ImGui::TextWrapped("New menu prose");')
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('New menu prose', result.stdout)

    def test_missing_translation_is_allowed(self):
        result = self.run_audit('ImGui::TextWrapped(TR("New menu prose"));')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 missing', result.stdout)

    def test_wrapped_multiline_and_context(self):
        result = self.run_audit('TRF("First part "\n"second part", value);\nTRC("Menu text", "Context");')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_non_menu_text_remains_excluded(self):
        result = self.run_audit('// "Comment prose"\nlogger::info("Diagnostic prose");\n'
                                'ImGui::PushID("Internal identifier");\n'
                                'ImGui::Text("Stable label"); // not translated: external name')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_report_flag_is_not_enforcement(self):
        result = self.run_audit('ImGui::Text("New menu prose");', ('--unwrapped',))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 menu texts outside', result.stdout)


if __name__ == '__main__':
    unittest.main()
