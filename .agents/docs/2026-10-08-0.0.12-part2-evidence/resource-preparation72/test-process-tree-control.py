from pathlib import Path
import json,subprocess,sys,select,time,hashlib
from process_tree_resources import ProcessTree,TreeChanged
q=Path(__file__).parent
engine=q/'controlled-parent-child.py'
assert not engine.exists()
engine.write_text('''import os,sys,time
r,w=os.pipe()
pid=os.fork()
if pid==0:
 os.close(w)
 memory=bytearray(32*1024*1024)
 for i in range(0,len(memory),4096):memory[i]=1
 print('CHILD_READY',flush=True)
 while True:
  command=os.read(r,1)
  if command==b'x':os._exit(0)
  if command==b'b':
   until=time.process_time()+.2
   while time.process_time()<until:pass
   print('CHILD_BUSY_DONE',flush=True)
os.close(r)
print('PARENT_READY',flush=True)
for line in sys.stdin:
 command=line.strip()
 if command=='busy':os.write(w,b'b')
 if command=='reap':
  os.write(w,b'x');os.waitpid(pid,0);print('CHILD_REAPED',flush=True)
 if command=='exit':break
''')
p=subprocess.Popen([sys.executable,str(engine)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
def line():
 if not select.select([p.stdout],[],[],10)[0]:raise TimeoutError('controlled process output deadline')
 return p.stdout.readline().decode().strip()
def send(value):p.stdin.write((value+'\n').encode());p.stdin.flush()
try:
 assert {line(),line()}=={'CHILD_READY','PARENT_READY'}
 tree=ProcessTree(p.pid);before=tree.read()
 assert len(before['members'])==2
 send('busy');assert line()=='CHILD_BUSY_DONE';busy=tree.read()
 assert busy['tree_cpu_lower_ms']-before['tree_cpu_upper_ms']>=100
 send('reap');assert line()=='CHILD_REAPED';reaped=tree.read()
 assert len(reaped['members'])==1
 assert before['tree_pss_kib']-reaped['tree_pss_kib']>=24*1024
 assert reaped['tree_cpu_lower_ms']>=busy['tree_cpu_lower_ms']-20
 assert reaped['members'][0]['reaped_cpu_ticks']>0
 tree.root=(tree.root[0],tree.root[1]+1)
 try:tree.read()
 except TreeChanged:identity_rejected=True
 else:raise AssertionError('PID identity mismatch accepted')
 send('exit');assert p.wait(timeout=10)==0
 report={'scope':'Actual Python parent/fork-child allocation, CPU and reap controls only. No clangd or latency/resource qualification.','before':before,'busy':busy,'reaped':reaped,'root_identity_mismatch_rejected':identity_rejected,'parent_exit_code':0,'sampler_source_sha256':hashlib.sha256((q/'process_tree_resources.py').read_bytes()).hexdigest()}
 target=q/'process-tree-control.json';assert not target.exists();target.write_text(json.dumps(report,indent=2)+'\n')
 print('Actual child allocation/CPU/reap and root identity controls pass.')
finally:
 if p.poll() is None:
  import os,signal
  os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=10)
