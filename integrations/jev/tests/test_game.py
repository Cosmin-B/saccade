import copy
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from saccade_jev.control import JevError
from saccade_jev.game import (GameController, HealthBar, HealthReading,
                              MacScreenHealthSource, choose_heal, load_healer_config, read_fraction)
from saccade_jev.voice import parse_voice_command


def configuration():
    return {'process': 'ExampleGame',
            'bars': [{'name': 'tank', 'rect': [10, 20, 20, 4], 'hover': [15, 21],
                      'fill': [20, 180, 20], 'empty': [30, 30, 30]}],
            'heals': [{'name': 'emergency', 'keyUsage': 5, 'below': .3},
                      {'name': 'regular', 'keyUsage': 4, 'below': .8}]}


class Pixels:
    def __init__(self, filled, *, corrupt=False):
        self.filled, self.corrupt = filled, corrupt

    def getpixel(self, point):
        if self.corrupt and point[0] % 2:
            return (255, 0, 255)
        return (20, 180, 20) if point[0] < self.filled else (30, 30, 30)


class Source:
    def __init__(self, fractions):
        self.fractions = list(fractions)

    async def read(self):
        return (HealthReading('tank', self.fractions.pop(0), time.monotonic()),)


class Driver:
    def __init__(self):
        self.observes = 0
        self.sequence = 77
        self.pointer = (0, 0)
        self.actions = []
        self.override = False

    async def call(self, name, args):
        if name == 'saccade_observe':
            self.observes += 1
            return {'generation': str(100 + self.observes),
                    'context': {'processId': '123', 'windowId': '44', 'displayId': '55'},
                    'scope': {'kind': 'active-window'},
                    'epochs': {'transform': '2', 'permission': '3', 'topology': '4'},
                    'truncated': False, 'sourceIncomplete': False, 'targets': []}
        if args['kind'] == 'physical':
            return {'result': 0, 'physical': {'sequence': str(self.sequence), 'buttons': 0,
                    'modifiers': 0, 'activeLeaseId': '0', 'permissionEpoch': '3',
                    'flags': 2 if self.override else 0,
                    'xQ8': self.pointer[0], 'yQ8': self.pointer[1]}}
        self.actions.append(copy.deepcopy(args))
        if args['kind'] == 'move':
            self.sequence += 1
            self.pointer = (args['xQ8'], args['yQ8'])
        return {'result': 0, 'completedActions': 1,
                'physical': {'sequence': str(self.sequence)},
                'nextGeneration': {'generation': '200'}}


def matching_process(*args, **kwargs):
    return subprocess.CompletedProcess(args, 0, stdout='/Applications/ExampleGame.app/Contents/MacOS/ExampleGame\n')


class GameTests(unittest.TestCase):
    def test_calibration_and_priority(self):
        process, bars, heals = load_healer_config(configuration())
        self.assertEqual(process, 'ExampleGame')
        self.assertEqual(choose_heal([HealthReading('tank', .2, 0)], bars, heals)[1].name, 'emergency')
        self.assertEqual(choose_heal([HealthReading('tank', .5, 0)], bars, heals)[1].name, 'regular')
        self.assertIsNone(choose_heal([HealthReading('tank', .9, 0)], bars, heals))

    def test_ambiguous_pixels_stop(self):
        bar = HealthBar('tank', (0, 0, 20, 4), (1, 1), (20, 180, 20), (30, 30, 30))
        self.assertEqual(read_fraction(Pixels(5), bar), .25)
        with self.assertRaises(JevError):
            read_fraction(Pixels(5, corrupt=True), bar)

    def test_bad_calibration_rejected(self):
        bad = configuration()
        bad['bars'][0]['hover'] = [999, 21]
        with self.assertRaises(JevError):
            load_healer_config(bad)
        bad = configuration()
        bad['heals'][0]['keyUsage'] = bad['heals'][1]['keyUsage']
        with self.assertRaises(JevError):
            load_healer_config(bad)

    def test_voice_commands_are_exact_and_bounded(self):
        self.assertEqual(parse_voice_command('Saccade, pause healing.'), 'pause')
        self.assertEqual(parse_voice_command('Saccade resume healing'), 'resume')
        self.assertEqual(parse_voice_command('Saccade stop healing!'), 'stop')
        self.assertIsNone(parse_voice_command('Someone said saccade stop healing'))
        self.assertIsNone(parse_voice_command('Saccade cast emergency heal'))

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS capture only')
    def test_retina_capture_reads_calibrated_bar(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('optional Pillow extra is not installed')
        _, bars, _ = load_healer_config(configuration())

        def capture(args, **kwargs):
            image = Image.new('RGB', (40, 8), (30, 30, 30))
            for x in range(10):
                for y in range(8):
                    image.putpixel((x, y), (20, 180, 20))
            image.save(args[-1])
            return subprocess.CompletedProcess(args, 0)

        with patch('saccade_jev.game.subprocess.run', side_effect=capture):
            readings = MacScreenHealthSource(bars)._read()
        self.assertEqual(readings[0].fraction, .25)


class GameControlTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.process, self.bars, self.heals = load_healer_config(configuration())

    @patch('saccade_jev.game.subprocess.run', side_effect=matching_process)
    async def test_dry_run_does_not_emit_input(self, _ps):
        driver = Driver()
        result = await GameController(driver, Source([.2]), self.process, self.bars, self.heals).tick()
        self.assertEqual(result['status'], 'dry-run')
        self.assertEqual(driver.actions, [])

    @patch('saccade_jev.game.subprocess.run', side_effect=matching_process)
    async def test_hover_then_heal_uses_guarded_native_actions(self, _ps):
        driver = Driver()
        result = await GameController(driver, Source([.2, .2]), self.process, self.bars, self.heals).tick(execute=True)
        self.assertEqual(result['status'], 'input-sent')
        self.assertEqual([action['kind'] for action in driver.actions], ['move', 'key'])
        self.assertEqual(driver.actions[1]['keyUsage'], 5)
        self.assertEqual(driver.actions[1]['physicalSequence'], '78')
        self.assertEqual(driver.actions[1]['processId'], '123')

    @patch('saccade_jev.game.subprocess.run', side_effect=matching_process)
    async def test_changed_healing_decision_stops_before_key(self, _ps):
        driver = Driver()
        with self.assertRaises(JevError):
            await GameController(driver, Source([.2, .9]), self.process, self.bars, self.heals).tick(execute=True)
        self.assertEqual([action['kind'] for action in driver.actions], ['move'])

    @patch('saccade_jev.game.subprocess.run', side_effect=matching_process)
    async def test_healthy_party_does_not_emit_input(self, _ps):
        driver = Driver()
        result = await GameController(driver, Source([.95]), self.process, self.bars, self.heals).tick(execute=True)
        self.assertEqual(result['status'], 'idle')
        self.assertEqual(driver.actions, [])

    @patch('saccade_jev.game.subprocess.run', side_effect=matching_process)
    async def test_minimum_interval_prevents_repeated_casts(self, _ps):
        driver = Driver()
        controller = GameController(driver, Source([.2, .2]), self.process, self.bars, self.heals)
        self.assertEqual((await controller.tick(execute=True))['status'], 'input-sent')
        self.assertEqual((await controller.tick(execute=True))['status'], 'cooldown')
        self.assertEqual([action['kind'] for action in driver.actions], ['move', 'key'])


if __name__ == '__main__':
    unittest.main()
