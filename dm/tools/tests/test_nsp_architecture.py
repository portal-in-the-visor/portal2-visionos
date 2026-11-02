"""Architecture detection with restricted sysctl and Rosetta Python."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('nsp_architecture', Path(__file__).resolve().parents[3] / 'scripts/build_ipa_from_nsp.py')
nsp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nsp)


class Architecture(unittest.TestCase):
    def test_native_arm_does_not_need_sysctl(self):
        with mock.patch.object(nsp.sys, 'platform', 'darwin'), mock.patch.object(nsp.os, 'uname', return_value=SimpleNamespace(machine='arm64')), mock.patch.object(nsp.subprocess, 'run', side_effect=PermissionError):
            self.assertTrue(nsp.apple_silicon())

    def test_translated_and_intel_python_use_hardware_query(self):
        for answer, expected in [('1\n', True), ('0\n', False), ('', False)]:
            with self.subTest(answer=answer), mock.patch.object(nsp.sys, 'platform', 'darwin'), mock.patch.object(nsp.os, 'uname', return_value=SimpleNamespace(machine='x86_64')), mock.patch.object(nsp.subprocess, 'run', return_value=SimpleNamespace(stdout=answer)):
                self.assertEqual(nsp.apple_silicon(), expected)
