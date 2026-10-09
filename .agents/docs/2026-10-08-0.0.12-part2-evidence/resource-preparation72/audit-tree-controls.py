"""Replay resource-window reduction on the recorded controlled fork LSP run."""
from pathlib import Path
import copy,json
from tree_resource_window import stable_tree_window
q=Path(__file__).parent
control=json.loads((q/'tree-replay-control-v2.json').read_text())
raw=control['replay'];raw['resources']=control['resources']
raw['context_answers']=[dict(r,phase=p,semantic_pass=True) for r,p in zip([r for r in raw['raw_responses'] if r['method']=='textDocument/completion'],['settled-warm','edited'])]
window=stable_tree_window(raw)
assert window['stable_requests']==2 and window['cpu_lower_ms']>0
assert window==json.loads((q/'tree-window-control-v2.json').read_text())['window']
for name in ['observer-error','missing-boundary','membership-gap']:
    bad=copy.deepcopy(raw)
    if name=='observer-error':bad['resources']['errors'].append('controlled error')
    if name=='missing-boundary':del bad['context_answers'][1]['resource_snapshots']
    if name=='membership-gap':bad['resources']['membership_races'].append({'monotonic_ns':bad['context_answers'][0]['completed_monotonic_ns'],'error':'controlled gap'})
    try:stable_tree_window(bad)
    except (ValueError,KeyError):pass
    else:raise AssertionError(name+' accepted')
print('Recorded actual mocked fork window and three negative reductions pass; no engine qualification.')
