"""Command line client; normal Saccade operation never imports this package."""
import argparse
import asyncio
from dataclasses import asdict
import json
import sys
from pathlib import Path

from .control import ActionSpec, CAPABILITIES, Controller, JevError, build_candidates
from .provider import JevChooser
from .transport import McpClient


def text_postcondition(expected):
    if not isinstance(expected, str) or not expected or len(expected.encode('utf-8')) > 2048:
        raise JevError('expectText must be nonempty text of at most 2048 UTF-8 bytes')
    return lambda observation: any(
        target.get('text') == expected and not target['flags'] & (4 | 8 | 32 | 64)
        for target in observation['targets'])


def load_workflow(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise JevError('Workflow exceeds 64 KiB')
    try:
        stages = json.loads(raw)
        if not isinstance(stages, list) or not 1 <= len(stages) <= 32:
            raise JevError('Workflow requires one to thirty-two stages')
        parsed = []
        for stage in stages:
            if not isinstance(stage, dict) or set(stage) != {'goal', 'actions', 'expectText'}:
                raise JevError('Each workflow stage requires goal, actions, and expectText')
            goal = stage['goal']
            if not isinstance(goal, str) or not goal.strip() or len(goal.encode('utf-8')) > 4096:
                raise JevError('Invalid workflow goal')
            if not isinstance(stage['actions'], list) or not 1 <= len(stage['actions']) <= 16:
                raise JevError('Each stage requires one to sixteen actions')
            specs = [ActionSpec(**action) for action in stage['actions']]
            for spec in specs:
                spec.parameters()
            parsed.append((goal, specs, text_postcondition(stage['expectText'])))
        return parsed
    except (ValueError, TypeError, KeyError) as error:
        raise JevError('Malformed workflow or action specification') from None


async def check_provider(chooser):
    # Synthetic only: this command never starts MCP or observes a desktop.
    observed = {
        'generation': '1', 'epochs': {'transform': '1', 'permission': '1', 'topology': '1'},
        'context': {'processId': '1', 'windowId': '1', 'displayId': '1'},
        'scope': {'kind': 'active-window'}, 'truncated': False, 'sourceIncomplete': False,
        'targets': [{'id': '1', 'windowId': '1', 'displayId': '1', 'role': 'button',
                     'text': 'Save', 'flags': 1, 'capabilities': 2,
                     'xQ8': 0, 'yQ8': 0, 'widthQ8': 2560, 'heightQ8': 2560}]}
    candidates = build_candidates(observed, [ActionSpec('click')])
    choice, confidence = await chooser.choose('Click the Save button.', observed, candidates)
    passed = choice == candidates[0].id
    print(json.dumps({'providerCheck': 'passed' if passed else 'stopped', 'confidence': confidence}))
    return 0 if passed else 2


async def run(args):
    if args.workflow:
        if args.goal or args.kind or args.text is not None or args.success_text:
            raise JevError('Workflow cannot be combined with single-step goal or actions')
        stages = load_workflow(args.workflow)
    elif not args.check_provider:
        if not args.goal:
            raise JevError('Provide a goal or --workflow')
        specs = []
        for kind in args.kind or ['click', 'invoke']:
            spec = ActionSpec(kind, text=args.text if kind == 'text' else None,
                              delta_y_q8=args.scroll_y_q8 if kind == 'scroll' else 0,
                              key_usage=args.key_usage if kind in ('key', 'key-chord') else 0,
                              modifiers=args.modifiers if kind in ('key', 'key-chord') else 0,
                              secondary_target_id=args.secondary_target if kind in ('drag', 'text-select') else None)
            spec.parameters()
            specs.append(spec)
        stages = [(args.goal, specs, text_postcondition(args.success_text) if args.success_text else None)]
    chooser = JevChooser(model=args.model)
    if args.check_provider:
        return await check_provider(chooser)
    async with McpClient([args.mcp]) as driver:
        controller = Controller(driver, chooser, minimum_confidence=args.minimum_confidence,
                                window_id=args.window_id, allow_activation=args.allow_activation)
        for index, (goal, specs, verify) in enumerate(stages):
            result = await controller.step(goal, specs, execute=args.execute, verify=verify)
            print(json.dumps(dict(stage=index + 1, **asdict(result))), flush=True)
            if result.status == 'dry-run':
                return 0  # Later stages depend on effects that a dry run cannot create.
            if result.status not in ('executed', 'verified'):
                return 2
    return 0


def main():
    parser = argparse.ArgumentParser(description=(
        'Optional Jev decision client. Sends bounded UI text/geometry to TypeSafe. '
        'Defaults to native dry-run validation; --execute emits input.'))
    parser.add_argument('goal', nargs='?')
    parser.add_argument('--mcp', default='saccade-mcp', help='Native MCP executable path')
    parser.add_argument('--model', default='jev-latest')
    parser.add_argument('--kind', action='append', choices=tuple(CAPABILITIES), help='Allowed action kind; repeatable')
    parser.add_argument('--text', help='Exact caller-provided text payload for text actions')
    parser.add_argument('--scroll-y-q8', type=int, default=25600)
    parser.add_argument('--key-usage', type=int, default=0, help='USB HID usage for key/key-chord')
    parser.add_argument('--modifiers', type=int, default=0, help='Saccade modifier bits')
    parser.add_argument('--secondary-target', help='Observed drop/selection target ID')
    parser.add_argument('--window-id', help='Exact native window ID')
    parser.add_argument('--allow-activation', action='store_true', help='Allow exact-window foreground activation')
    parser.add_argument('--minimum-confidence', type=float, default=.8)
    parser.add_argument('--success-text', help='Exact visible target text required for verified success')
    parser.add_argument('--workflow', help='Local JSON stages: goal, actions, expectText')
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--check-provider', action='store_true', help='Live API check using synthetic data only')
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except (JevError, OSError) as error:
        # OS error strings may contain private paths; print only our bounded errors.
        message = str(error) if isinstance(error, JevError) else 'Local file or MCP executable unavailable'
        print('saccade-jev: ' + message, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('saccade-jev: cancelled; do not replay unconfirmed input', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
