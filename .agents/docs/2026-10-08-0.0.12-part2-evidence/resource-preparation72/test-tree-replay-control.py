from pathlib import Path
import sys,json,importlib.util,hashlib
q=Path(__file__).parent
spec=importlib.util.spec_from_file_location('tree_replay',q/'project-completion-tree.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
engine=q/'controlled-tree-lsp.py';assert not engine.exists()
engine.write_text('''#!/usr/bin/env python3
import os,json,sys,time
commands_r,commands_w=os.pipe();acks_r,acks_w=os.pipe()
pid=os.fork()
if pid==0:
 os.close(commands_w);os.close(acks_r)
 memory=bytearray(16*1024*1024)
 while True:
  command=os.read(commands_r,1)
  if command==b'x':os._exit(0)
  until=time.process_time()+.15
  while time.process_time()<until:pass
  os.write(acks_w,b'd')
os.close(commands_r);os.close(acks_w)
try:
 while True:
  headers={}
  while True:
   line=sys.stdin.buffer.readline()
   if not line:sys.exit(0)
   if line in (b'\\r\\n',b'\\n'):break
   key,value=line.decode().split(':',1);headers[key.lower()]=value.strip()
  message=json.loads(sys.stdin.buffer.read(int(headers['content-length'])))
  if message['method']=='exit':break
  if 'id' not in message:continue
  result=None
  if message['method']=='initialize':result={'capabilities':{}}
  if message['method']=='textDocument/completion':
   os.write(commands_w,b'b');os.read(acks_r,1)
   result={'items':[{'label':'vector','kind':7}],'isIncomplete':False}
  body=json.dumps({'jsonrpc':'2.0','id':message['id'],'result':result}).encode()
  sys.stdout.buffer.write(('Content-Length: %d\\r\\n\\r\\n'%len(body)).encode()+body);sys.stdout.buffer.flush()
finally:
 os.write(commands_w,b'x');os.waitpid(pid,0)
''');engine.chmod(0o755)
case=q/'controlled-tree-lsp-case.json';assert not case.exists()
case.write_text(json.dumps({'id':'tree-hook-control','platform':module.project.replay.platform_name(),'baseline':'pass','project':'.','timeout_seconds':15,'sequence':[{'method':'initialize','params':{}},{'method':'textDocument/completion','params':{},'expected_symbols':['vector']},{'method':'textDocument/completion','params':{},'defer':'pending'},{'action':'await','request':'pending','expect':{'/result/items/0/kind':7}}]},indent=2)+'\n')
sampler=module.TreeResources()
try:r=module.project.replay.replay(engine,case,process_started=sampler.started,request_snapshot=sampler.snapshot)
finally:resources=sampler.finish()
assert r['outcome']=='pass' and r['readers_stopped'],r
assert resources['sampler_stopped'] and not resources['errors'] and not resources['remaining_owned_processes'],resources
replies=[row for row in r['raw_responses'] if row['method']=='textDocument/completion'];assert len(replies)==2
for row in replies:
 b=row['resource_snapshots'];assert len(b['start']['members'])==len(b['end']['members'])==2
 assert b['start']['monotonic_ns']<=row['started_monotonic_ns']<=row['completed_monotonic_ns']<=b['end']['sample_started_monotonic_ns']
 assert b['end']['tree_cpu_lower_ms']>b['start']['tree_cpu_upper_ms']
report={'scope':'Actual framed Python parent/fork LSP hook control, ordinary and deferred replies, CPU and cleanup; mock answers are not clangd semantic or release evidence. All timings excluded.','replay':r,'resources':resources}
p=q/'tree-replay-control.json';assert not p.exists();p.write_text(json.dumps(report,indent=2)+'\n')
print('Actual ordinary/deferred tree snapshots and owned-process cleanup pass.')
