"""Full Python controller -> native MCP -> Service -> ActionPlanner -> form state."""
import os
import unittest
from saccade_jev import ActionSpec, Controller, JevError
from saccade_jev.transport import McpClient


class FormChooser:
    async def choose(self, goal, observation, candidates):
        wanted = 'text' if 'name' in goal else 'click'
        target = '11' if wanted == 'text' else '12'
        candidate = next(c for c in candidates if c.arguments()['kind'] == wanted and c.arguments()['targetId'] == target)
        return candidate.id, .99


def has_text(text):
    return lambda observation: any(target.get('text') == text for target in observation['targets'])


class NativeEndToEndTests(unittest.IsolatedAsyncioTestCase):
    def executable(self):
        executable = os.environ.get('SACCADE_JEV_NATIVE_FIXTURE')
        if not executable:
            self.skipTest('native Jev fixture not configured')
        self.assertTrue(os.path.isfile(executable), 'native Jev E2E fixture is not built')
        return executable

    async def test_text_then_click_verified_from_native_application_state(self):
        async with McpClient([self.executable()]) as driver:
            controller = Controller(driver, FormChooser())
            typed = await controller.step('Enter the name Ada', [ActionSpec('text', text='Ada')], execute=True,
                                          verify=has_text('Entered: Ada'))
            self.assertEqual(typed.status, 'verified')
            saved = await controller.step('Save the form', [ActionSpec('click')], execute=True,
                                          verify=has_text('Saved: Ada'))
            self.assertEqual(saved.status, 'verified')
            self.assertGreater(int(saved.generation), int(typed.generation))

    async def test_dry_run_does_not_mutate_native_form(self):
        async with McpClient([self.executable()]) as driver:
            result = await Controller(driver, FormChooser()).step('Enter the name Ada', [ActionSpec('text', text='Ada')])
            self.assertEqual(result.status, 'dry-run')
            observed = await driver.call('saccade_observe', {'scope': 'active-window'})
            self.assertTrue(has_text('Ready')(observed))
            self.assertFalse(has_text('Entered: Ada')(observed))

    async def test_native_service_rejects_stale_generation_without_input(self):
        async with McpClient([self.executable()]) as driver:
            with self.assertRaises(JevError):
                await driver.call('saccade_act', {'kind': 'click', 'targetId': '12', 'generation': '1'})
            observed = await driver.call('saccade_observe', {'scope': 'active-window'})
            self.assertTrue(has_text('Ready')(observed))


if __name__ == '__main__':
    unittest.main()
