"""Bounded Jev decisions over Saccade's existing, guarded MCP actions."""
from __future__ import annotations

import json
import math
import secrets
from dataclasses import dataclass
from typing import Callable, Optional


class JevError(RuntimeError):
    """A bounded operation stopped; an input action must not be replayed."""


def encode(value):
    return json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(',', ':'))


def uint(value, bits=64, nonzero=False):
    if isinstance(value, str) and value.isascii() and value.isdecimal():
        value = int(value)
    if type(value) is not int or not int(nonzero) <= value < 2 ** bits:
        raise JevError('Invalid unsigned identifier or integer')
    return value


def probability(value):
    if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1:
        raise JevError('Invalid probability or confidence')
    return float(value)


# Wire capability bits from include/saccade/saccade_agent.h.
CAPABILITIES = {'move': 1, 'hover': 1, 'click': 2, 'scroll': 4, 'drag': 8,
                'text': 32, 'invoke': 64, 'window': 128, 'text-select': 256,
                'key': 512, 'key-chord': 512}
BLOCKED_FLAGS = 2 | 4 | 8 | 32 | 64
MAX_TARGETS = 64
MAX_CANDIDATES = 128


@dataclass(frozen=True)
class ActionSpec:
    """Caller-owned parameters. The model may only choose a complete candidate."""
    kind: str
    text: Optional[str] = None
    delta_x_q8: int = 0
    delta_y_q8: int = 0
    key_usage: int = 0
    modifiers: int = 0
    secondary_target_id: Optional[str] = None

    def parameters(self):
        if self.kind not in CAPABILITIES:
            raise JevError('Unsupported candidate action kind')
        args = {'kind': self.kind}
        if self.kind == 'text':
            if not isinstance(self.text, str) or not self.text or len(self.text.encode('utf-8')) > 16384 or '\0' in self.text:
                raise JevError('Text requires a nonempty, bounded caller-provided value')
            args['text'] = self.text
        elif self.text is not None:
            raise JevError('Text payload requires the text action')
        if self.kind == 'scroll':
            for value in (self.delta_x_q8, self.delta_y_q8):
                if type(value) is not int or not -(2 ** 31) <= value < 2 ** 31:
                    raise JevError('Invalid Q8 scroll delta')
            if not (self.delta_x_q8 or self.delta_y_q8):
                raise JevError('Scroll requires an explicit nonzero delta')
            args.update(deltaXQ8=self.delta_x_q8, deltaYQ8=self.delta_y_q8)
        elif self.delta_x_q8 or self.delta_y_q8:
            raise JevError('Scroll deltas require the scroll action')
        if self.kind in ('key', 'key-chord'):
            args.update(keyUsage=uint(self.key_usage, 32, True), modifiers=uint(self.modifiers, 32))
        elif self.key_usage or self.modifiers:
            raise JevError('Keyboard parameters require a keyboard action')
        if self.kind in ('drag', 'text-select'):
            args['secondaryTargetId'] = str(uint(self.secondary_target_id, nonzero=True))
        elif self.secondary_target_id is not None:
            raise JevError('Secondary target requires drag or text-select')
        return args


@dataclass(frozen=True)
class Candidate:
    id: str
    description: str
    _arguments_json: str

    def arguments(self):
        return json.loads(self._arguments_json)


@dataclass(frozen=True)
class StepResult:
    status: str
    candidate_id: Optional[str] = None
    confidence: Optional[float] = None
    generation: Optional[str] = None


def validate_scene(observation):
    try:
        uint(observation['generation'], nonzero=True)
        if observation['truncated'] is not False or observation['sourceIncomplete'] is not False:
            raise JevError('Incomplete observation; narrow the scope before choosing')
        if observation['scope']['kind'] not in ('active-window', 'window'):
            raise JevError('Actions require active-window or exact-window observations')
        for field in ('processId', 'windowId', 'displayId'):
            uint(observation['context'][field], nonzero=True)
        if observation['scope']['kind'] == 'window' and (
                uint(observation['scope']['stableId'], nonzero=True) != uint(observation['context']['windowId'])):
            raise JevError('Exact-window scope does not match the observed window')
        for field in ('transform', 'permission', 'topology'):
            uint(observation['epochs'][field], nonzero=True)
        targets = observation['targets']
        if not isinstance(targets, list) or len(targets) > MAX_TARGETS:
            raise JevError('Observation exceeds target budget')
        ids = set()
        for target in targets:
            identity = str(uint(target['id'], nonzero=True))
            if identity in ids:
                raise JevError('Duplicate target identity')
            ids.add(identity)
            uint(target['flags'], 32)
            uint(target['capabilities'], 32)
            for field in ('windowId', 'displayId'):
                if str(uint(target[field])) != str(uint(observation['context'][field])):
                    raise JevError('Target does not belong to the observed context')
            text = target.get('text', '')
            if not isinstance(text, str) or len(text.encode('utf-8')) > 2048:
                raise JevError('Target text exceeds decision budget')
    except (KeyError, TypeError, AttributeError) as error:
        raise JevError('Malformed Saccade observation') from error


def eligible(target):
    return target['flags'] & 1 and not target['flags'] & BLOCKED_FLAGS


def build_candidates(observation, specs, maximum=MAX_CANDIDATES, allow_activation=False):
    validate_scene(observation)
    if type(maximum) is not int or not 1 <= maximum <= MAX_CANDIDATES:
        raise JevError('Invalid candidate budget')
    if not 1 <= len(specs) <= 16:
        raise JevError('Provide between one and sixteen action specifications')
    parameters = [spec.parameters() for spec in specs]
    targets = observation['targets']
    by_id = {str(target['id']): target for target in targets}
    nonce = secrets.token_hex(8)
    candidates = []
    for args in parameters:
        if args['kind'] in ('key', 'key-chord') and observation['scope']['kind'] == 'active-window':
            candidates.append(Candidate(f'{nonce}:{len(candidates)}',
                                        encode({'action': args, 'context': observation['context']}), encode(args)))
    if len(candidates) > maximum:
        raise JevError('Candidate budget exceeded')
    for target in targets:
        if not eligible(target):
            continue
        for args in parameters:
            kind = args['kind']
            if kind in ('key', 'key-chord'):
                continue
            if not target['capabilities'] & CAPABILITIES[kind]:
                continue
            if observation['scope']['kind'] == 'window':
                if kind not in ('click', 'invoke') or target['flags'] & 512:
                    continue
                if not target['flags'] & 128 and not (allow_activation and target['flags'] & 256):
                    continue
            if kind in ('drag', 'text-select'):
                other = by_id.get(args['secondaryTargetId'])
                capability = 16 if kind == 'drag' else 256
                if not other or not eligible(other) or not other['capabilities'] & capability:
                    continue
            local = dict(args, targetId=str(target['id']))
            description = encode({'action': args, 'target': {
                key: target[key] for key in ('id', 'role', 'text', 'xQ8', 'yQ8', 'widthQ8', 'heightQ8') if key in target}})
            candidates.append(Candidate(f'{nonce}:{len(candidates)}', description, encode(local)))
            if len(candidates) > maximum:
                raise JevError('Candidate budget exceeded; narrow the action set or scope')
    return tuple(candidates)


def scene_identity(observation):
    """Ignore publication clocks and confidence; preserve all actionable evidence."""
    targets = [{k: v for k, v in target.items() if k not in ('index', 'order', 'confidenceQ16')}
               for target in observation['targets']]
    return (observation['context'], observation['scope'],
            tuple(observation['epochs'][k] for k in ('transform', 'permission', 'topology')),
            sorted((encode(target) for target in targets)))


def physical_identity(result):
    try:
        if result['result'] != 0:
            raise JevError('Physical state unavailable')
        physical = result['physical']
        values = tuple(uint(physical[key]) for key in
                       ('sequence', 'buttons', 'modifiers', 'activeLeaseId', 'permissionEpoch', 'flags'))
        if any(values[i] for i in (1, 2, 3, 5)):
            raise JevError('Physical input is active, suspended, or overridden')
        return values
    except (KeyError, TypeError) as error:
        raise JevError('Malformed physical state') from error


class Controller:
    def __init__(self, driver, chooser, *, minimum_confidence=0.8, window_id=None, allow_activation=False):
        self.driver = driver
        self.chooser = chooser
        self.minimum_confidence = probability(minimum_confidence)
        self.allow_activation = allow_activation
        if allow_activation and window_id is None:
            raise JevError('Activation requires an explicit window ID')
        self.scope = {'scope': 'active-window', 'sourceMode': 'fused', 'maximumTargets': MAX_TARGETS}
        if window_id is not None:
            self.scope.update(scope='window', scopeId=str(uint(window_id, nonzero=True)))

    async def _observe(self, **extra):
        observed = await self.driver.call('saccade_observe', dict(self.scope, **extra))
        validate_scene(observed)
        if observed['scope']['kind'] != self.scope['scope'] or (
                self.scope['scope'] == 'window' and observed['context']['windowId'] != self.scope['scopeId']):
            raise JevError('Observation does not match the requested scope')
        return observed

    async def step(self, goal, specs, *, execute=False, verify: Optional[Callable] = None):
        if not isinstance(goal, str) or not goal.strip() or len(goal.encode('utf-8')) > 4096:
            raise JevError('Provide a nonempty goal of at most 4096 UTF-8 bytes')
        physical = physical_identity(await self.driver.call('saccade_act', {'kind': 'physical'}))
        observed = await self._observe()
        candidates = build_candidates(observed, specs, allow_activation=self.allow_activation)
        if verify is not None and verify(observed):
            return StepResult('verified', generation=observed['generation'])
        if not candidates:
            return StepResult('no-candidates', generation=observed['generation'])
        choice, confidence = await self.chooser.choose(goal, observed, candidates)
        confidence = probability(confidence)
        if choice == 'stop':
            return StepResult('stopped', confidence=confidence)
        selected = next((candidate for candidate in candidates if candidate.id == choice), None)
        if selected is None:
            raise JevError('Jev returned an unknown or stale candidate ID')
        if confidence < self.minimum_confidence:
            return StepResult('uncertain', selected.id, confidence)
        current_physical = physical_identity(await self.driver.call('saccade_act', {'kind': 'physical'}))
        fresh = await self._observe()
        if current_physical != physical or scene_identity(fresh) != scene_identity(observed):
            raise JevError('Decision became stale; no action dispatched')
        if uint(fresh['generation']) < uint(observed['generation']):
            raise JevError('Scene generation regressed')
        if physical[4] != uint(fresh['epochs']['permission']):
            raise JevError('Physical and scene permission epochs disagree')
        action = selected.arguments()
        action.update(fresh['context'])
        action.update(generation=fresh['generation'], transformEpoch=fresh['epochs']['transform'],
                      permissionEpoch=fresh['epochs']['permission'], physicalSequence=str(physical[0]),
                      expectedButtons=0, expectedModifiers=0, dryRun=not execute,
                      verifyNextGeneration=execute, explicitWindow=self.scope['scope'] == 'window',
                      allowActivation=self.allow_activation)
        # This is the only dispatch. Never retry after timeout, rejection, or lost response.
        completion = await self.driver.call('saccade_act', action)
        if completion.get('result') != 0:
            raise JevError('Native action was rejected or its outcome is unconfirmed')
        if not execute:
            return StepResult('dry-run', selected.id, confidence, fresh['generation'])
        if completion.get('completedActions') != 1:
            raise JevError('Native action completion is unconfirmed')
        next_generation = completion.get('nextGeneration', {}).get('generation')
        if uint(next_generation) <= uint(fresh['generation']):
            raise JevError('Post-action generation was not verified; do not replay input')
        after = await self._observe(afterGeneration=fresh['generation'])
        if uint(after['generation']) < uint(next_generation):
            raise JevError('Post-action observation is stale')
        status = 'executed' if verify is None else ('verified' if verify(after) else 'postcondition-failed')
        return StepResult(status, selected.id, confidence, after['generation'])
