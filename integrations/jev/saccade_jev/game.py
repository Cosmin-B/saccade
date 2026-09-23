"""Bounded game control using caller-configured visual evidence and Saccade input."""
from __future__ import annotations

import asyncio
import math
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from .control import JevError, physical_identity, uint


@dataclass(frozen=True)
class HealthBar:
    name: str
    rect: tuple[int, int, int, int]
    hover: tuple[int, int]
    fill: tuple[int, int, int]
    empty: tuple[int, int, int]


@dataclass(frozen=True)
class Heal:
    name: str
    key_usage: int
    below: float


@dataclass(frozen=True)
class HealthReading:
    name: str
    fraction: float
    captured_at: float


def _triplet(value):
    if not isinstance(value, (list, tuple)) or len(value) != 3 or any(type(v) is not int or not 0 <= v <= 255 for v in value):
        raise JevError('Health-bar colors must be RGB triplets')
    return tuple(value)


def load_healer_config(document):
    if not isinstance(document, dict) or set(document) != {'bars', 'heals', 'process'}:
        raise JevError('Healer configuration requires bars, heals, and process')
    process = document['process']
    if not isinstance(process, str) or not process or len(process) > 256 or '/' in process:
        raise JevError('Configure the exact game executable name')
    raw_bars, raw_heals = document['bars'], document['heals']
    if not isinstance(raw_bars, list) or not 1 <= len(raw_bars) <= 40 or not isinstance(raw_heals, list) or not 1 <= len(raw_heals) <= 12:
        raise JevError('Configure one to forty bars and one to twelve heals')
    bars = []
    for item in raw_bars:
        if not isinstance(item, dict) or set(item) != {'name', 'rect', 'hover', 'fill', 'empty'}:
            raise JevError('Invalid health bar')
        name, rect, hover = item['name'], item['rect'], item['hover']
        if not isinstance(name, str) or not 1 <= len(name) <= 64 or not isinstance(rect, list) or len(rect) != 4 or not isinstance(hover, list) or len(hover) != 2:
            raise JevError('Invalid health-bar name or geometry')
        if any(type(v) is not int or not 0 <= v <= 32767 for v in rect + hover) or not 4 <= rect[2] <= 1024 or not 3 <= rect[3] <= 128:
            raise JevError('Invalid health-bar coordinates')
        if not (rect[0] <= hover[0] < rect[0] + rect[2] and rect[1] <= hover[1] < rect[1] + rect[3]):
            raise JevError('Hover point must lie inside its health bar')
        fill, empty = _triplet(item['fill']), _triplet(item['empty'])
        if sum((a - b) ** 2 for a, b in zip(fill, empty)) < 40 ** 2:
            raise JevError('Fill and empty colors are too similar')
        bars.append(HealthBar(name, tuple(rect), tuple(hover), fill, empty))
    if len({bar.name for bar in bars}) != len(bars):
        raise JevError('Health-bar names must be unique')
    heals = []
    for item in raw_heals:
        if not isinstance(item, dict) or set(item) != {'name', 'keyUsage', 'below'}:
            raise JevError('Invalid heal binding')
        name, threshold = item['name'], item['below']
        if not isinstance(name, str) or not 1 <= len(name) <= 64 or type(threshold) not in (float, int) or not math.isfinite(threshold) or not 0 < threshold < 1:
            raise JevError('Invalid heal name or threshold')
        heals.append(Heal(name, uint(item['keyUsage'], 32, True), float(threshold)))
    if len({heal.key_usage for heal in heals}) != len(heals):
        raise JevError('Heal bindings must use distinct keys')
    if len({heal.below for heal in heals}) != len(heals):
        raise JevError('Heal thresholds must be distinct')
    return process, tuple(bars), tuple(sorted(heals, key=lambda heal: heal.below))


def read_fraction(image, bar):
    """Classify columns as filled/empty; ambiguous or noncontiguous bars stop."""
    x, y, width, height = bar.rect
    samples = []
    for col in range(x, x + width):
        votes = []
        for row in (y + height // 4, y + height // 2, y + 3 * height // 4):
            pixel = image.getpixel((col, row))[:3]
            filled = sum((a - b) ** 2 for a, b in zip(pixel, bar.fill))
            empty = sum((a - b) ** 2 for a, b in zip(pixel, bar.empty))
            votes.append(1 if filled < 35 ** 2 and filled * 2 < empty else
                         0 if empty < 35 ** 2 and empty * 2 < filled else None)
        samples.append(1 if votes.count(1) >= 2 else 0 if votes.count(0) >= 2 else None)
    known = [value for value in samples if value is not None]
    if len(known) < width * .9:
        raise JevError('Health bar contains too many unknown pixels')
    filled_count = sum(known)
    boundary = filled_count
    contradictions = sum(value != (1 if index < boundary else 0)
                         for index, value in enumerate(samples) if value is not None)
    if contradictions > width * .08:
        raise JevError('Health bar is not a left-to-right fill')
    return filled_count / len(known)


class MacScreenHealthSource:
    """Capture configured screen rectangles using macOS Screen Recording permission."""
    def __init__(self, bars, *, timeout=2.0):
        if sys.platform != 'darwin':
            raise JevError('macOS screen health capture requires macOS')
        self.bars = tuple(bars)
        self.timeout = timeout

    async def read(self):
        return await asyncio.to_thread(self._read)

    def _read(self):
        try:
            from PIL import Image
        except ImportError:
            raise JevError('Install the game extra: python -m pip install "./integrations/jev[game]"') from None
        left = min(bar.rect[0] for bar in self.bars)
        top = min(bar.rect[1] for bar in self.bars)
        right = max(bar.rect[0] + bar.rect[2] for bar in self.bars)
        bottom = max(bar.rect[1] + bar.rect[3] for bar in self.bars)
        if right - left > 2048 or bottom - top > 2048 or (right - left) * (bottom - top) > 2_000_000:
            raise JevError('Calibrated screen region exceeds capture budget')
        with tempfile.TemporaryDirectory(prefix='saccade-game-') as directory:
            path = Path(directory) / 'capture.png'
            try:
                subprocess.run(['/usr/sbin/screencapture', '-x', '-R',
                                f'{left},{top},{right-left},{bottom-top}', str(path)],
                               check=True, timeout=self.timeout, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
                captured_at = time.monotonic()
                with Image.open(path) as image:
                    image.load()
                    width, height = right-left, bottom-top
                    if image.width % width or image.height % height or image.width // width != image.height // height:
                        raise JevError('Screen capture dimensions changed')
                    scale = image.width // width
                    if not 1 <= scale <= 3:
                        raise JevError('Unsupported screen capture scale')
                    readings = []
                    for bar in self.bars:
                        local = HealthBar(bar.name, ((bar.rect[0]-left) * scale, (bar.rect[1]-top) * scale,
                                                     bar.rect[2] * scale, bar.rect[3] * scale),
                                          bar.hover, bar.fill, bar.empty)
                        readings.append(HealthReading(bar.name, read_fraction(image, local), captured_at))
                    return tuple(readings)
            except (OSError, subprocess.SubprocessError) as error:
                raise JevError('Screen capture unavailable or timed out') from error


def choose_heal(readings, bars, heals):
    if len(readings) != len(bars) or {reading.name for reading in readings} != {bar.name for bar in bars}:
        raise JevError('Health observations do not match configured bars')
    lowest = min(readings, key=lambda item: item.fraction)
    if not math.isfinite(lowest.fraction) or not 0 <= lowest.fraction <= 1:
        raise JevError('Invalid health fraction')
    eligible = [heal for heal in heals if lowest.fraction < heal.below]
    return (next(bar for bar in bars if bar.name == lowest.name), eligible[0]) if eligible else None


def validate_game_scene(scene):
    """Only context is used; the separate screen reader supplies game evidence."""
    try:
        if scene['scope']['kind'] != 'active-window' or scene['sourceIncomplete'] is not False:
            raise JevError('Game window observation is incomplete')
        uint(scene['generation'], nonzero=True)
        for field in ('processId', 'windowId', 'displayId'):
            uint(scene['context'][field], nonzero=True)
        for field in ('transform', 'permission', 'topology'):
            uint(scene['epochs'][field], nonzero=True)
    except (KeyError, TypeError, AttributeError) as error:
        raise JevError('Malformed game window observation') from error


class GameController:
    """Two guarded actions: hover a calibrated frame, then press a mouseover binding."""
    def __init__(self, driver, source, process, bars, heals, *, max_capture_age=.5, minimum_cast_interval=1.5):
        self.driver, self.source, self.process = driver, source, process
        self.bars, self.heals = bars, heals
        self.max_capture_age = max_capture_age
        if not math.isfinite(minimum_cast_interval) or not .1 <= minimum_cast_interval <= 60:
            raise JevError('Invalid minimum cast interval')
        self.minimum_cast_interval = minimum_cast_interval
        self.last_cast_at = float('-inf')

    async def _scene(self):
        scene = await self.driver.call('saccade_observe', {'scope': 'active-window', 'sourceMode': 'fused', 'maximumTargets': 1})
        validate_game_scene(scene)
        pid = uint(scene['context']['processId'], nonzero=True)
        try:
            completed = await asyncio.to_thread(subprocess.run, ['/bin/ps', '-p', str(pid), '-o', 'comm='],
                                                capture_output=True, text=True, timeout=1.0, check=True)
        except (OSError, subprocess.SubprocessError) as error:
            raise JevError('Focused game process cannot be verified') from error
        if Path(completed.stdout.strip()).name != self.process:
            raise JevError('Focused process does not match the configured game')
        return scene

    async def tick(self, *, execute=False):
        if execute and time.monotonic() - self.last_cast_at < self.minimum_cast_interval:
            return {'status': 'cooldown'}
        before = await self._scene()
        initial_physical = physical_identity(await self.driver.call('saccade_act', {'kind': 'physical'}))
        readings = await self.source.read()
        if not readings or time.monotonic() - min(item.captured_at for item in readings) > self.max_capture_age:
            raise JevError('Health observation is stale')
        selection = choose_heal(readings, self.bars, self.heals)
        if selection is None:
            return {'status': 'idle'}
        bar, heal = selection
        if not execute:
            return {'status': 'dry-run', 'ally': bar.name, 'heal': heal.name,
                    'health': next(reading.fraction for reading in readings if reading.name == bar.name)}
        current = await self._scene()
        physical = physical_identity(await self.driver.call('saccade_act', {'kind': 'physical'}))
        if current['context'] != before['context'] or current['epochs']['permission'] != before['epochs']['permission'] or physical != initial_physical:
            raise JevError('Game context or physical input changed')
        common = dict(current['context'], transformEpoch=current['epochs']['transform'],
                      permissionEpoch=current['epochs']['permission'],
                      physicalSequence=str(physical[0]), expectedButtons=0, expectedModifiers=0)
        moved = await self.driver.call('saccade_act', dict(common, kind='move',
                           xQ8=bar.hover[0] * 256, yQ8=bar.hover[1] * 256))
        if moved.get('completedActions') != 1:
            raise JevError('Pointer move outcome unconfirmed')
        fresh = await self._scene()
        physical_result = await self.driver.call('saccade_act', {'kind': 'physical'})
        physical = physical_identity(physical_result)
        if fresh['context'] != before['context'] or physical[4] != uint(fresh['epochs']['permission']):
            raise JevError('Game focus or permission changed after hover')
        if (physical[0] != uint(moved['physical']['sequence']) or
                physical_result['physical']['xQ8'] != bar.hover[0] * 256 or
                physical_result['physical']['yQ8'] != bar.hover[1] * 256):
            raise JevError('Pointer moved after hover')
        updated = await self.source.read()
        if not updated or time.monotonic() - min(item.captured_at for item in updated) > self.max_capture_age:
            raise JevError('Health observation is stale after hover')
        if choose_heal(updated, self.bars, self.heals) != selection:
            raise JevError('Healing decision changed after hover')
        if physical_identity(await self.driver.call('saccade_act', {'kind': 'physical'})) != physical:
            raise JevError('Physical input changed before heal')
        if time.monotonic() - min(item.captured_at for item in updated) > self.max_capture_age:
            raise JevError('Health observation expired before heal')
        cast = await self.driver.call('saccade_act', dict(fresh['context'], kind='key', keyUsage=heal.key_usage,
                      modifiers=0, transformEpoch=fresh['epochs']['transform'],
                      permissionEpoch=fresh['epochs']['permission'], physicalSequence=str(physical[0]),
                      expectedButtons=0, expectedModifiers=0, verifyNextGeneration=True))
        if cast.get('completedActions') != 1 or uint(cast.get('nextGeneration', {}).get('generation')) <= uint(fresh['generation']):
            raise JevError('Heal input outcome unconfirmed')
        self.last_cast_at = time.monotonic()
        return {'status': 'input-sent', 'ally': bar.name, 'heal': heal.name}
