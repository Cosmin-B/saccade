"""Persistent, bounded JSONL transport for the native saccade-mcp executable."""
import asyncio
import json
import math
import os

from .control import JevError, encode

MAX_REQUEST = 65536
MAX_RESPONSE = 1024 * 1024
PROTOCOL = '2025-06-18'


class McpClient:
    def __init__(self, command=('saccade-mcp',), *, timeout=5.0):
        if not command or isinstance(command, str):
            raise JevError('MCP command must be a nonempty argument sequence')
        if not math.isfinite(timeout) or not 0 < timeout <= 60:
            raise JevError('MCP timeout must be between zero and sixty seconds')
        self.command = tuple(command)
        self.timeout = timeout
        self.process = None
        self.tool_names = set()
        self._id = 0
        self._broken = False
        self._lock = asyncio.Lock()

    async def __aenter__(self):
        environment = dict(os.environ)
        environment.pop('TYPESAFE_API_KEY', None)
        try:
            self.process = await asyncio.create_subprocess_exec(
                *self.command, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL, env=environment, limit=MAX_RESPONSE)
            initialized = await self._request('initialize', {
                'protocolVersion': PROTOCOL, 'capabilities': {},
                'clientInfo': {'name': 'saccade-jev', 'version': '0.1.0'}})
            if initialized.get('protocolVersion') != PROTOCOL:
                raise JevError('Unsupported MCP protocol version')
            await self._write({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
            inventory = await self._request('tools/list', {})
            self.tool_names = {tool['name'] for tool in inventory['tools']}
            if not {'saccade_observe', 'saccade_query', 'saccade_act'} <= self.tool_names:
                raise JevError('MCP server lacks required Saccade tools')
            return self
        except BaseException:
            await self.close()
            raise

    async def __aexit__(self, exc_type, exc, traceback):
        await self.close()

    async def close(self):
        self._broken = True
        if self.process is None or self.process.returncode is not None:
            return
        self.process.stdin.close()
        try:
            await asyncio.wait_for(self.process.wait(), timeout=.5)
        except asyncio.TimeoutError:
            self.process.kill()
            await self.process.wait()

    async def _write(self, message):
        data = (encode(message) + '\n').encode('utf-8')
        if len(data) > MAX_REQUEST:
            raise JevError('MCP request exceeds native input capacity')
        self.process.stdin.write(data)
        await asyncio.wait_for(self.process.stdin.drain(), self.timeout)

    async def _exchange(self, method, params):
        self._id += 1
        await self._write({'jsonrpc': '2.0', 'id': self._id, 'method': method, 'params': params})
        line = await self.process.stdout.readline()
        if not line or len(line) > MAX_RESPONSE:
            raise JevError('MCP closed or response exceeds budget')
        response = json.loads(line)
        if response.get('jsonrpc') != '2.0' or response.get('id') != self._id:
            raise JevError('MCP response ID or protocol mismatch')
        if 'error' in response or not isinstance(response.get('result'), dict):
            raise JevError('MCP request failed')
        return response['result']

    async def _request(self, method, params):
        async with self._lock:
            if self._broken or self.process is None:
                raise JevError('MCP connection is not usable')
            try:
                return await asyncio.wait_for(self._exchange(method, params), self.timeout)
            except asyncio.CancelledError:
                self._broken = True
                raise
            except (asyncio.TimeoutError, OSError, ValueError, AttributeError, JevError):
                self._broken = True
                raise JevError('MCP exchange failed; never replay an unconfirmed action') from None

    async def call(self, name, arguments):
        if name not in self.tool_names:
            raise JevError('Unknown Saccade tool')
        result = await self._request('tools/call', {'name': name, 'arguments': arguments})
        data = result.get('structuredContent')
        if result.get('isError') or not isinstance(data, dict) or data.get('result', 0) != 0:
            raise JevError('Saccade rejected the request or outcome is unconfirmed')
        return data
