"""Configurable macOS game healer; defaults to observe-only decisions."""
import argparse
import asyncio
import json
import sys
import time
from pathlib import Path

from .control import JevError
from .game import GameController, MacScreenHealthSource, load_healer_config
from .transport import McpClient
from .voice import LocalVoiceMonitor


async def run(args):
    try:
        with Path(args.config).open('rb') as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise JevError('Healer configuration exceeds 64 KiB')
        process, bars, heals = load_healer_config(json.loads(raw))
    except (OSError, ValueError, UnicodeError) as error:
        raise JevError('Cannot read healer configuration') from error
    source = MacScreenHealthSource(bars)
    started = time.monotonic()
    actions = 0
    voice = LocalVoiceMonitor(device=args.voice_device) if args.voice else None
    enabled = voice is None
    if voice:
        voice.start()
    try:
        async with McpClient([args.mcp]) as driver:
            controller = GameController(driver, source, process, bars, heals,
                                        minimum_cast_interval=args.minimum_cast_interval)
            while actions < args.max_actions and time.monotonic() - started < args.max_seconds:
                command = voice.poll() if voice else None
                if command == 'stop':
                    return 0
                if command == 'pause':
                    enabled = False
                    print(json.dumps({'status': 'paused'}), flush=True)
                if command == 'resume':
                    enabled = True
                    print(json.dumps({'status': 'resumed'}), flush=True)
                if enabled:
                    result = await controller.tick(execute=args.execute)
                    if result['status'] != 'cooldown':
                        print(json.dumps(result), flush=True)
                    if result['status'] == 'input-sent':
                        actions += 1
                    if result['status'] == 'dry-run':
                        return 0
                await asyncio.sleep(args.interval)
    finally:
        if voice:
            voice.stop()
    return 0


def main():
    parser = argparse.ArgumentParser(description='Bounded macOS game healer using calibrated visible health bars')
    parser.add_argument('config', help='Local JSON configuration')
    parser.add_argument('--mcp', default='saccade-mcp', help='Native MCP executable')
    parser.add_argument('--execute', action='store_true', help='Emit pointer and key input')
    parser.add_argument('--max-actions', type=int, default=30)
    parser.add_argument('--max-seconds', type=float, default=60)
    parser.add_argument('--interval', type=float, default=.2)
    parser.add_argument('--minimum-cast-interval', type=float, default=1.5)
    parser.add_argument('--voice', action='store_true', help='Local Parakeet Redux microphone commands; starts paused')
    parser.add_argument('--voice-device', choices=('cpu', 'mps'), default='cpu')
    args = parser.parse_args()
    if (not 1 <= args.max_actions <= 1000 or not 1 <= args.max_seconds <= 3600 or
            not .05 <= args.interval <= 10 or not .1 <= args.minimum_cast_interval <= 60):
        parser.error('max-actions, max-seconds, or interval outside allowed bounds')
    try:
        return asyncio.run(run(args))
    except (JevError, OSError) as error:
        print('saccade-game-healer: ' + (str(error) if isinstance(error, JevError) else 'Local service unavailable'),
              file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('saccade-game-healer: stopped', file=sys.stderr)
        return 130


if __name__ == '__main__':
    sys.exit(main())
