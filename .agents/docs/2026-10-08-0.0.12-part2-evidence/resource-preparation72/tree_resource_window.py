"""Observed stable process-tree window; no latency or physical cache claim."""
import math


def stable_tree_window(raw):
    if raw.get('outcome')!='pass' or not raw.get('readers_stopped'):
        raise ValueError('replay/readers incomplete')
    resources=raw.get('resources',{})
    if resources.get('latency_qualification') is not False:
        raise ValueError('resource-only observer identity missing')
    if resources.get('errors') or not resources.get('sampler_stopped') or resources.get('remaining_owned_processes'):
        raise ValueError('observer/cleanup failed')
    answers=raw.get('context_answers',[])
    if not answers or any(a.get('semantic_pass') is not True for a in answers):
        raise ValueError('semantic request failure or missing replies')
    stable=[a for a in answers if a.get('phase') in ('settled-warm','edited')]
    if not stable or stable[0]['phase']!='settled-warm':
        raise ValueError('no AST-ready stable window')
    boundaries=[];root_identity=None;previous=None
    for answer in stable:
        pair=answer.get('resource_snapshots',{})
        start,end=pair.get('start',{}),pair.get('end',{})
        for sample in (start,end):
            for key in ('tree_cpu_lower_ms','tree_cpu_upper_ms','tree_rss_sum_kib','tree_pss_kib','tree_private_kib'):
                value=sample.get(key)
                if not isinstance(value,(int,float)) or not math.isfinite(value) or value<0:
                    raise ValueError('invalid/missing tree counter')
            if sample['tree_cpu_lower_ms']>sample['tree_cpu_upper_ms']:
                raise ValueError('invalid CPU interval')
            if not sample.get('members') or not sample.get('root_identity'):
                raise ValueError('tree identity missing')
            if root_identity is None:root_identity=sample['root_identity']
            if sample['root_identity']!=root_identity:raise ValueError('root identity changed')
        if not (start['sample_started_monotonic_ns']<=start['monotonic_ns']<=answer['started_monotonic_ns']<=answer['completed_monotonic_ns']<=end['sample_started_monotonic_ns']<=end['monotonic_ns']):
            raise ValueError('request/snapshot clocks out of order')
        if previous is not None and start['sample_started_monotonic_ns']<previous:
            raise ValueError('stable windows overlap')
        previous=end['monotonic_ns'];boundaries.extend((start,end))
    first,last=boundaries[0],boundaries[-1]
    if any(first['sample_started_monotonic_ns']<=r['monotonic_ns']<=last['monotonic_ns'] for r in resources.get('membership_races',[])):
        raise ValueError('membership/reap sampling gap within stable window')
    periodic=[s for s in resources.get('samples',[]) if first['monotonic_ns']<=s.get('sample_started_monotonic_ns',-1)<=s.get('monotonic_ns',-1)<=last['sample_started_monotonic_ns']]
    samples=boundaries+periodic
    memory={}
    for key in ('tree_rss_sum_kib','tree_pss_kib','tree_private_kib'):
        values=[s.get(key) for s in samples]
        if any(not isinstance(v,(int,float)) or not math.isfinite(v) or v<0 for v in values):
            raise ValueError('invalid periodic memory observation')
        values.sort();memory[key]={'p95':values[math.ceil(.95*len(values))-1],'observed_max':max(values),'observations':len(values)}
    lower=last['tree_cpu_lower_ms']-first['tree_cpu_upper_ms']
    upper=last['tree_cpu_upper_ms']-first['tree_cpu_lower_ms']
    if upper<0:raise ValueError('tree CPU counters moved backwards')
    return {'stable_requests':len(stable),'root_identity':root_identity,'cpu_lower_ms':max(0,lower),'cpu_upper_ms':upper,'memory':memory,
            'scope':'Resource-only stable root plus observed descendants. CPU quantization and read interval bounds; PSS apportioning, RSS sum counts shared pages more than once. Periodic observation cannot prove fast unsampled child escape or physical cache allocation. All elapsed times excluded.'}
