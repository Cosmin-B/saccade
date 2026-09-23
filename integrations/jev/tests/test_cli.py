import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CliTests(unittest.TestCase):
    def cli(self, *arguments):
        environment = dict(os.environ, PYTHONPATH=str(ROOT))
        environment.pop('TYPESAFE_API_KEY', None)
        return subprocess.run([sys.executable, '-m', 'saccade_jev', *arguments], env=environment,
                              capture_output=True, text=True, timeout=10)

    def test_help_needs_no_credentials_or_desktop(self):
        result = self.cli('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--execute', result.stdout)

    def test_missing_key_fails_without_traceback_or_desktop_access(self):
        result = self.cli('Save', '--mcp', 'nonexistent-mcp')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('TYPESAFE_API_KEY', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_invalid_workflow_fails_before_network_or_desktop(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'workflow.json'
            path.write_text(json.dumps([{'goal': 'Save', 'actions': [{'kind': 'click'}]}]))
            result = self.cli('--workflow', str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('expectText', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

class WorkflowTests(unittest.IsolatedAsyncioTestCase):
    async def test_verified_stages_advance_and_failed_postconditions_stop(self):
        import argparse
        import contextlib
        import io
        from unittest.mock import patch
        from saccade_jev.__main__ import run
        from saccade_jev.transport import McpClient
        from test_control import Chooser

        fixture = Path(__file__).with_name('mcp_control_fixture.py')
        for expected, return_code, stages in [('Saved', 0, 2), ('Missing', 2, 1)]:
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'workflow.json'
                path.write_text(json.dumps([
                    {'goal': 'Save', 'actions': [{'kind': 'click'}], 'expectText': expected},
                    {'goal': 'Save is already complete', 'actions': [{'kind': 'click'}], 'expectText': 'Saved'}]))
                arguments = argparse.Namespace(workflow=str(path), goal=None, kind=None, text=None,
                    success_text=None, check_provider=False, model='jev-latest', mcp='unused',
                    minimum_confidence=.8, window_id=None, allow_activation=False, execute=True)
                output = io.StringIO()
                with patch('saccade_jev.__main__.JevChooser', return_value=Chooser()), \
                     patch('saccade_jev.__main__.McpClient', side_effect=lambda command: McpClient([sys.executable, str(fixture)])), \
                     contextlib.redirect_stdout(output):
                    result = await run(arguments)
                self.assertEqual(result, return_code)
                records = [json.loads(line) for line in output.getvalue().splitlines()]
                self.assertEqual(len(records), stages)
                self.assertEqual(records[-1]['status'], 'verified' if return_code == 0 else 'postcondition-failed')


if __name__ == '__main__':
    unittest.main()
