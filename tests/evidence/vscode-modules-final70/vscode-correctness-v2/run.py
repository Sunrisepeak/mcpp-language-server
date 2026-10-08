from pathlib import Path
import subprocess,os,json,time,signal,threading
root=Path(__file__).parent
if (root/'progress.json').exists():raise SystemExit('refusing to overwrite editor attempt')
args=json.loads((root/'command.json').read_text());env=os.environ.copy();env.update(json.loads((root/'environment.json').read_text()))
state={'status':'running','scope':'Correctness only; all timing/resource measurements excluded. Real VSCode module suite and private user data.','started_unix':time.time()}
(root/'progress.json').write_text(json.dumps(state,indent=2)+'\n')
owned={};stop=threading.Event();errors=[]
def scan():
 rows={}
 for entry in Path('/proc').iterdir():
  if not entry.name.isdigit():continue
  try:
   stat=(entry/'stat').read_text();parts=stat[stat.rfind(')')+2:].split()
   rows[int(entry.name)]=(int(parts[1]),int(parts[2]),int(parts[19]))
  except (OSError,ValueError):pass
 return rows
def watcher(pid):
 while not stop.is_set():
  try:
   rows=scan();family={pid}|{p for p,(_,ticks) in owned.items() if p in rows and rows[p][2]==ticks}
   changed=True
   while changed:
    changed=False
    for p,(parent,group,ticks) in rows.items():
     if parent in family and p not in family:family.add(p);changed=True
   for p in family:
    if p in rows:owned[p]=(rows[p][1],rows[p][2])
  except OSError as error:errors.append(str(error))
  stop.wait(.1)
with (root/'run.log').open('w') as log:
 proc=subprocess.Popen(args,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 state['controller_child_pid']=proc.pid;(root/'progress.json').write_text(json.dumps(state,indent=2)+'\n')
 thread=threading.Thread(target=watcher,args=(proc.pid,));thread.start()
 try:code=proc.wait(timeout=300)
 except subprocess.TimeoutExpired:code=124
 finally:
  stop.set();thread.join(timeout=2)
  rows=scan();groups={group for p,(group,ticks) in owned.items() if p in rows and rows[p][2]==ticks and rows[p][1]==group}
  if proc.poll() is None:groups.add(proc.pid)
  for group in groups:
   try:os.killpg(group,signal.SIGTERM)
   except ProcessLookupError:pass
  if groups:time.sleep(.5)
  rows=scan();remaining={group for p,(group,ticks) in owned.items() if p in rows and rows[p][2]==ticks and rows[p][1]==group}
  if proc.poll() is None:remaining.add(proc.pid)
  for group in remaining:
   try:os.killpg(group,signal.SIGKILL)
   except ProcessLookupError:pass
  proc.wait(timeout=5)
state.update(status='complete' if code==0 else 'failed',exit_code=code,finished_unix=time.time(),owned_processes={str(p):{'pgid':v[0],'start_ticks':v[1]} for p,v in owned.items()},cleanup_groups=sorted(groups),cleanup_kill_groups=sorted(remaining),watcher_errors=errors,watcher_stopped=not thread.is_alive())
(root/'progress.json').write_text(json.dumps(state,indent=2)+'\n')
print('Real VSCode final70 module correctness:',code,flush=True)
raise SystemExit(code)
