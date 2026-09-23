"""Synthetic JSONL MCP peer; never connects to a desktop or network."""
import json
import sys
import time
from test_control import scene

initialized = False
notified = False
for line in sys.stdin:
    request = json.loads(line)
    method = request['method']
    if method == 'notifications/initialized':
        notified = True
        continue
    response = {'jsonrpc': '2.0', 'id': request['id']}
    if method == 'initialize':
        initialized = True
        result = {'protocolVersion': '2025-06-18', 'capabilities': {'tools': {}}}
    elif method == 'tools/list' and initialized and notified:
        result = {'tools': [{'name': name} for name in ('saccade_observe', 'saccade_query', 'saccade_act')]}
    elif method == 'tools/call' and initialized and notified:
        args = request['params']['arguments']
        kind = args.get('kind')
        if kind == 'hang':
            time.sleep(30)
        if kind == 'wrong-id':
            response['id'] += 1
        if kind == 'oversize':
            print('x' * (1024 * 1024 + 1), flush=True)
            continue
        if request['params']['name'] == 'saccade_observe':
            data = scene()
        else:
            data = {'result': -1 if kind == 'fail' else 0, 'completedActions': 1}
        result = {'structuredContent': data, 'isError': kind == 'fail'}
    else:
        result = {'isError': True}
    response['result'] = result
    print(json.dumps(response), flush=True)
