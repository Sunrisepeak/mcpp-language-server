from pathlib import Path
import hashlib,json,re,gzip
from io import StringIO
root=Path(__file__).parent
sha=lambda p:hashlib.sha256(p.read_bytes() if p.exists() else gzip.decompress(Path(str(p)+'.gz').read_bytes())).hexdigest()
labels=['baseline-before','candidate','baseline-after']
summary={'scope':'Same-recipe product UP20 actual one-save ABA. Existing C-2 answers included; engineShare is not new Sema execution. Three arms are not a multi-start distribution or release qualification.','arms':{}}
reference=None
for label in labels:
 d=root/label;identity=json.loads((d/'identity.json').read_text());measure=json.loads((d/'measure.json').read_text());work=json.loads(gzip.decompress((d/'workload.json.gz').read_bytes()));args=json.loads((d/'command.json').read_text())
 assert identity['exit_code']==0 and identity['tracked_source_restored'] and identity['workload_qualified']
 cleanup=identity['owned_cleanup'];assert not any(cleanup[k] for k in ['errors','term_groups','kill_groups','remaining_observed_live']) and cleanup['watcher_stopped']
 assert work['samples'] and not any(work[k] for k in ['errors','compiler_overlap','multiple_engine_overlap']) and work['watcher_stopped']
 assert not any(s['compilers'] or len({e['pgid'] for e in s['engines']})>1 for s in work['samples'])
 # Executable identities were audited live. Archive replay validates recorded inputs and actual traces; does not independently reconstruct executables.
 matched={k:v for k,v in identity['sha256'].items() if k!='server'}|{'source_audit_sha256':identity['source_audit_sha256'],'kit_provenance_sha256':identity['kit_provenance_sha256'],'cpus':identity['cpus'],'fixture_sha256':sha(d/'fixture/scenario.json'),'workspace':args[args.index('--workspace-dir')+1]}
 if reference is None:reference=matched
 else:assert matched==reference
 assert measure['failures']==0 and len(measure['checks'])==1 and measure['checks'][0]['ok']
 m=measure['checks'][0]['measure'];assert m['restarts']==m['errorSamples']==m['completionsUnanswered']==0 and m['importers']==8 and 0<m['probeSeconds'] and 0<m['republishSeconds'] and 0<m['recoverSeconds']<=60
 warnings=[]
 for number,line in enumerate(StringIO(gzip.decompress((d/'run.log.gz').read_bytes()).decode()),1):
  if '[warning] the event loop took' in line:
   found=re.search(r'took (\d+) ms for .* and (\d+) ms for its timers',line)
   assert found
   warnings.append({'line':number,'event_ms':int(found[1]),'timers_ms':int(found[2]),'text':line.strip()})
 summary['arms'][label]={'server_sha256':identity['sha256']['server'],'measure':m,'event_loop_warnings':warnings,'observed_workload_samples':len(work['samples']),'run_log_sha256':sha(d/'run.log')}
assert summary['arms']['baseline-before']['server_sha256']==summary['arms']['baseline-after']['server_sha256']!=summary['arms']['candidate']['server_sha256']
summary['same_inputs_except_server']=reference
summary['completion_p95_ms']={label:summary['arms'][label]['measure']['completion']['p95']*1000 for label in labels}
summary['candidate_p95_improvement_percent_vs_each_baseline']={label:(1-summary['completion_p95_ms']['candidate']/summary['completion_p95_ms'][label])*100 for label in ['baseline-before','baseline-after']}
summary['timer_warning_limit']='Absence means no slow timer warning in this log, not zero timer cost or exclusive CPU attribution.'
summary['status']='passed'
assert summary==json.loads((root/'audit.json').read_text()), 'archive replay differs from live audit'
print(json.dumps({k:summary[k] for k in ['status','completion_p95_ms','candidate_p95_improvement_percent_vs_each_baseline']},indent=2))
