import asyncio
import io
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

from test_control import scene
from saccade_jev import ActionSpec, JevError, build_candidates
try:
    from saccade_jev.provider import JevChooser, parse_choice
    from saccade_jev.transport import McpClient
except ImportError:
    JevChooser = McpClient = parse_choice = None


def answer(choice, probabilities=None, confidence=.95):
    return {'answers': {'action': {'type': 'choice', 'choice': choice, 'confidence': confidence,
            'probabilities': probabilities if probabilities is not None else {choice: .95, 'stop': .05}}}}


class BoundaryTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.assertIsNotNone(JevChooser, 'Provider integration is not implemented')
        self.candidates = build_candidates(scene(), [ActionSpec('click')])

    def test_response_rejects_unknown_choices_and_invalid_distributions(self):
        identity = self.candidates[0].id
        value = answer(identity)
        self.assertEqual(parse_choice(value, self.candidates), (identity, .95))
        for invalid in [answer('invented'), answer(identity, {identity: 1}),
                        answer(identity, {identity: .8, 'stop': .8}),
                        answer(identity, {identity: float('nan'), 'stop': .05}),
                        answer(identity, confidence=True), {'answers': {}}]:
            with self.assertRaises(JevError):
                parse_choice(invalid, self.candidates)

    async def test_provider_uses_documented_wire_shape_and_redacts_sensitive_targets(self):
        identity = self.candidates[0].id
        observed = scene()
        observed['targets'].append(dict(observed['targets'][0], id='55', flags=9, text='secret-value'))
        captured = []
        def open_request(request, timeout):
            captured.append(request)
            return io.BytesIO(json.dumps(answer(identity)).encode())
        with patch('saccade_jev.provider.urllib.request.OpenerDirector.open', side_effect=open_request):
            chosen = await JevChooser(api_key='test-secret').choose('Save', observed, self.candidates)
        self.assertEqual(chosen, (identity, .95))
        self.assertEqual(len(captured), 1)
        request = captured[0]
        self.assertEqual(request.full_url, 'https://api.typesafe.ai/v1/systemone')
        payload = json.loads(request.data)
        self.assertEqual(payload['model'], 'jev-latest')
        self.assertEqual(payload['questions']['action']['type'], 'choice')
        self.assertEqual(set(payload['questions']['action']['criteria']), {identity, 'stop'})
        self.assertNotIn('secret-value', request.data.decode())
        self.assertNotIn('test-secret', request.data.decode())

    async def test_provider_bounds_payload_and_does_not_retry_failure(self):
        with patch('saccade_jev.provider.urllib.request.OpenerDirector.open', side_effect=OSError('secret transport details')) as call:
            with self.assertRaises(JevError) as caught:
                await JevChooser(api_key='test-secret').choose('Save', scene(), self.candidates)
            self.assertNotIn('secret transport details', str(caught.exception))
            self.assertEqual(call.call_count, 1)
            with self.assertRaises(JevError):
                await JevChooser(api_key='test-secret').choose('x' * 70000, scene(), self.candidates)
            self.assertEqual(call.call_count, 1)

    async def test_persistent_mcp_handshake_tool_calls_and_error(self):
        fixture = Path(__file__).with_name('mcp_fixture.py')
        async with McpClient([sys.executable, str(fixture)]) as client:
            observed = await client.call('saccade_observe', {})
            self.assertEqual(observed['generation'], '9007199254740993')
            result = await client.call('saccade_act', {'kind': 'click'})
            self.assertEqual(result['completedActions'], 1)
            with self.assertRaises(JevError):
                await client.call('saccade_act', {'kind': 'fail'})
        self.assertIsNotNone(client.process.returncode)

    async def test_mcp_timeout_poisons_connection(self):
        fixture = Path(__file__).with_name('mcp_fixture.py')
        async with McpClient([sys.executable, str(fixture)]) as client:
            client.timeout = .2
            with self.assertRaises(JevError):
                await client.call('saccade_act', {'kind': 'hang'})
            with self.assertRaises(JevError):
                await client.call('saccade_observe', {})
        self.assertIsNotNone(client.process.returncode)

    async def test_mcp_wrong_id_and_oversized_response_fail(self):
        fixture = Path(__file__).with_name('mcp_fixture.py')
        for kind in ('wrong-id', 'oversize'):
            async with McpClient([sys.executable, str(fixture)]) as client:
                with self.assertRaises(JevError):
                    await client.call('saccade_act', {'kind': kind})

    @unittest.skipUnless(os.environ.get('SACCADE_MCP_TEST_EXECUTABLE'), 'native MCP executable not provided')
    async def test_native_mcp_inventory(self):
        async with McpClient([os.environ['SACCADE_MCP_TEST_EXECUTABLE']]) as client:
            self.assertEqual(client.tool_names, {'saccade_observe', 'saccade_query', 'saccade_act'})

class EndToEndTests(unittest.IsolatedAsyncioTestCase):
    async def test_decision_dispatch_and_postcondition_through_persistent_mcp(self):
        from saccade_jev import Controller
        from test_control import Chooser
        fixture = Path(__file__).with_name('mcp_control_fixture.py')
        async with McpClient([sys.executable, str(fixture)]) as driver:
            result = await Controller(driver, Chooser()).step(
                'Click Save', [ActionSpec('click')], execute=True,
                verify=lambda observed: observed['targets'][0]['text'] == 'Saved')
        self.assertEqual(result.status, 'verified')
        self.assertEqual(result.generation, '103')


if __name__ == '__main__':
    unittest.main()
