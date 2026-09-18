import copy
import unittest

try:
    from saccade_jev import ActionSpec, Controller, JevError, build_candidates
except ImportError:
    ActionSpec = Controller = JevError = build_candidates = None


def scene(generation='9007199254740993'):
    return {
        'generation': generation,
        'epochs': {'scene': generation, 'frame': '40', 'captureTimeNs': '50',
                   'transform': '3', 'permission': '4', 'topology': '5'},
        'context': {'processId': '101', 'windowId': '202', 'displayId': '303'},
        'scope': {'kind': 'active-window', 'sourceMode': 'fused', 'stableId': '0'},
        'truncated': False, 'sourceIncomplete': False,
        'targets': [
            {'id': '9007199254740995', 'parentId': '0', 'windowId': '202',
             'displayId': '303', 'role': 'button', 'roleCode': 1, 'capabilities': 66,
             'flags': 1, 'sourceBits': 2, 'confidenceQ16': 65535, 'order': 0,
             'xQ8': 256, 'yQ8': 256, 'widthQ8': 2560, 'heightQ8': 2560,
             'safeXQ8': 512, 'safeYQ8': 512, 'text': 'Save'},
        ],
    }


class Driver:
    def __init__(self):
        self.before = scene()
        self.fresh = scene('9007199254740994')
        self.after = scene('9007199254740996')
        self.after['targets'][0]['text'] = 'Saved'
        self.observations = 0
        self.actions = []
        self.fail_action = False
        self.physical_reads = 0
        self.override = False

    async def call(self, name, args):
        if name == 'saccade_observe':
            result = [self.before, self.fresh, self.after][min(self.observations, 2)]
            self.observations += 1
            return copy.deepcopy(result)
        if args['kind'] == 'physical':
            self.physical_reads += 1
            return {'result': 0, 'physical': {'sequence': '77', 'buttons': 0,
                    'modifiers': 0, 'activeLeaseId': '0', 'permissionEpoch': '4',
                    'flags': 2 if self.override and self.physical_reads > 1 else 0}}
        self.actions.append(copy.deepcopy(args))
        if self.fail_action:
            raise JevError('outcome unconfirmed')
        return {'result': 0, 'completedActions': 1,
                'generation': args['generation'], 'nextGeneration': {'generation': '9007199254740995'}}


class Chooser:
    def __init__(self, choice=None, confidence=0.99):
        self.choice = choice
        self.confidence = confidence

    async def choose(self, goal, observation, candidates):
        return self.choice or candidates[0].id, self.confidence


class ControlTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.assertIsNotNone(Controller, 'Jev integration is not implemented')

    def test_capabilities_and_sensitive_targets_filter_candidates(self):
        observed = scene()
        for flags in [3, 5, 9, 33]:
            target = copy.deepcopy(observed['targets'][0])
            target['id'] = str(int(target['id']) + flags)
            target['flags'] = flags
            observed['targets'].append(target)
        candidates = build_candidates(observed, [ActionSpec('click'), ActionSpec('text', text='hello')])
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].arguments()['targetId'], '9007199254740995')
        self.assertEqual(candidates[0].arguments()['kind'], 'click')

    def test_text_is_explicit_and_arguments_are_not_mutable(self):
        observed = scene()
        observed['targets'][0]['capabilities'] = 32
        candidates = build_candidates(observed, [ActionSpec('text', text='literal $value')])
        args = candidates[0].arguments()
        args['text'] = 'injected'
        self.assertEqual(candidates[0].arguments()['text'], 'literal $value')
        with self.assertRaises(JevError):
            build_candidates(observed, [ActionSpec('text')])

    def test_incomplete_duplicate_and_over_budget_observations_fail(self):
        for field in ['truncated', 'sourceIncomplete']:
            observed = scene()
            observed[field] = True
            with self.assertRaises(JevError):
                build_candidates(observed, [ActionSpec('click')])
        observed = scene()
        observed['targets'].append(copy.deepcopy(observed['targets'][0]))
        with self.assertRaises(JevError):
            build_candidates(observed, [ActionSpec('click')])
        with self.assertRaises(JevError):
            build_candidates(scene(), [ActionSpec('click'), ActionSpec('invoke')], maximum=1)

    async def test_fresh_generation_rebind_preserves_identity_and_guards(self):
        driver = Driver()
        result = await Controller(driver, Chooser()).step('Save', [ActionSpec('click')], execute=True,
                    verify=lambda observation: observation['targets'][0]['text'] == 'Saved')
        self.assertEqual(result.status, 'verified')
        self.assertEqual(len(driver.actions), 1)
        action = driver.actions[0]
        self.assertEqual(action['generation'], '9007199254740994')
        self.assertEqual(action['targetId'], '9007199254740995')
        self.assertEqual(action['physicalSequence'], '77')
        self.assertEqual(action['processId'], '101')
        self.assertEqual(action['windowId'], '202')
        self.assertEqual(action['transformEpoch'], '3')
        self.assertTrue(action['verifyNextGeneration'])

    async def test_unknown_stale_stop_and_uncertain_choices_emit_no_input(self):
        for choice, confidence in [('invented', .99), ('older:c0', .99), ('stop', .99), (None, .1), (None, float('nan'))]:
            driver = Driver()
            try:
                result = await Controller(driver, Chooser(choice, confidence)).step('Save', [ActionSpec('click')], execute=True)
                self.assertIn(result.status, ['stopped', 'uncertain'])
            except JevError:
                pass
            self.assertEqual(driver.actions, [])

    async def test_changed_target_context_epochs_or_physical_state_emit_no_input(self):
        mutations = [lambda d: d.fresh['targets'][0].update(text='Delete'),
                     lambda d: d.fresh['targets'][0].update(xQ8=2048),
                     lambda d: d.fresh['context'].update(windowId='999'),
                     lambda d: d.fresh['epochs'].update(permission='999'),
                     lambda d: setattr(d, 'override', True)]
        for mutate in mutations:
            driver = Driver()
            mutate(driver)
            with self.assertRaises(JevError):
                await Controller(driver, Chooser()).step('Save', [ActionSpec('click')], execute=True)
            self.assertEqual(driver.actions, [])

    async def test_dry_run_and_unconfirmed_dispatch_are_never_retried(self):
        driver = Driver()
        result = await Controller(driver, Chooser()).step('Save', [ActionSpec('click')])
        self.assertEqual(result.status, 'dry-run')
        self.assertTrue(driver.actions[0]['dryRun'])
        self.assertFalse(driver.actions[0]['verifyNextGeneration'])
        driver = Driver()
        driver.fail_action = True
        with self.assertRaises(JevError):
            await Controller(driver, Chooser()).step('Save', [ActionSpec('click')], execute=True)
        self.assertEqual(len(driver.actions), 1)

    async def test_scene_advance_is_not_task_success(self):
        driver = Driver()
        result = await Controller(driver, Chooser()).step('Save', [ActionSpec('click')], execute=True,
                                                        verify=lambda observation: False)
        self.assertEqual(result.status, 'postcondition-failed')
        driver = Driver()
        result = await Controller(driver, Chooser()).step('Save', [ActionSpec('click')], execute=True)
        self.assertEqual(result.status, 'executed')

class ActionCoverageTests(unittest.TestCase):
    def test_keyboard_candidates_are_bound_to_window_not_fake_target_capability(self):
        candidates = build_candidates(scene(), [ActionSpec('key', key_usage=40)])
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].arguments(), {'kind': 'key', 'keyUsage': 40, 'modifiers': 0})

    def test_all_target_action_kinds_retain_explicit_arguments(self):
        observed = scene()
        observed['targets'][0]['capabilities'] = 511
        kinds = ['move', 'hover', 'click', 'invoke', 'window']
        for kind in kinds:
            with self.subTest(kind=kind):
                candidate = build_candidates(observed, [ActionSpec(kind)])[0]
                self.assertEqual(candidate.arguments()['kind'], kind)
        for spec, expected in [(ActionSpec('scroll', delta_y_q8=-256), {'deltaYQ8': -256}),
                               (ActionSpec('text', text='hello'), {'text': 'hello'}),
                               (ActionSpec('drag', secondary_target_id='9007199254740995'), {'secondaryTargetId': '9007199254740995'}),
                               (ActionSpec('text-select', secondary_target_id='9007199254740995'), {'secondaryTargetId': '9007199254740995'})]:
            candidate = build_candidates(observed, [spec])[0]
            for key, value in expected.items():
                self.assertEqual(candidate.arguments()[key], value)

    def test_exact_window_actions_require_background_disposition(self):
        observed = scene()
        observed['scope']['kind'] = 'window'
        observed['scope']['stableId'] = '202'
        self.assertEqual(build_candidates(observed, [ActionSpec('click')]), ())
        observed['targets'][0]['flags'] = 129
        self.assertEqual(len(build_candidates(observed, [ActionSpec('invoke')])), 1)
        observed['targets'][0]['flags'] = 257
        self.assertEqual(build_candidates(observed, [ActionSpec('click')]), ())
        self.assertEqual(len(build_candidates(observed, [ActionSpec('click')], allow_activation=True)), 1)
        self.assertEqual(build_candidates(observed, [ActionSpec('key', key_usage=40)]), ())

class ScopeTests(unittest.IsolatedAsyncioTestCase):
    async def test_explicit_window_scope_cannot_fall_back_to_foreground(self):
        driver = Driver()
        with self.assertRaises(JevError):
            await Controller(driver, Chooser(), window_id='202').step('Save', [ActionSpec('click')], execute=True)
        self.assertEqual(driver.actions, [])

    def test_exact_window_identity_must_match_scope(self):
        observed = scene()
        observed['scope'].update(kind='window', stableId='999')
        with self.assertRaises(JevError):
            build_candidates(observed, [ActionSpec('click')])


if __name__ == '__main__':
    unittest.main()
