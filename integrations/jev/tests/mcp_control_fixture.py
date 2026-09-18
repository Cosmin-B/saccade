"""Synthetic desktop state machine with independent dispatch preconditions."""
import json
import sys
from test_control import scene

initialized = False
notified = False
saved = False
reads = 0
for line in sys.stdin:
    request = json.loads(line)
    method = request['method']
    if method == 'notifications/initialized':
        notified = True
        continue
    result = {'isError': True}
    if method == 'initialize':
        initialized = True
        result = {'protocolVersion': '2025-06-18', 'capabilities': {'tools': {}}}
    elif initialized and notified:
        if method == 'tools/list':
            result = {'tools': [{'name': name} for name in ('saccade_observe', 'saccade_query', 'saccade_act')]}
        elif method == 'tools/call':
            arguments = request['params']['arguments']
            if request['params']['name'] == 'saccade_observe':
                reads += 1
                observed = scene(str(100 + reads))
                if saved:
                    observed['targets'][0]['text'] = 'Saved'
                result = {'structuredContent': observed}
            elif arguments.get('kind') == 'physical':
                result = {'structuredContent': {'result': 0, 'physical': {
                    'sequence': '77', 'buttons': 0, 'modifiers': 0,
                    'activeLeaseId': '0', 'permissionEpoch': '4', 'flags': 0}}}
            else:
                expected = {'kind': 'click', 'generation': '102', 'targetId': '9007199254740995',
                            'processId': '101', 'windowId': '202', 'displayId': '303',
                            'permissionEpoch': '4', 'transformEpoch': '3', 'physicalSequence': '77',
                            'expectedButtons': 0, 'expectedModifiers': 0, 'verifyNextGeneration': True,
                            'dryRun': False, 'explicitWindow': False, 'allowActivation': False}
                if arguments == expected and not saved:
                    saved = True
                    result = {'structuredContent': {'result': 0, 'completedActions': 1,
                              'generation': '102', 'nextGeneration': {'generation': '103'}}}
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
