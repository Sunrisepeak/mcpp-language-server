from pathlib import Path
import threading,time,json
class Workload:
 def __init__(self,root):
  self.root=root; self.samples=[]; self.errors=[]; self.stop=threading.Event();self.began=time.monotonic()
  self.thread=threading.Thread(target=self.watch);self.thread.start()
 def watch(self):
  while not self.stop.is_set():
   try:
    compilers=[];engines=[]
    for p in Path('/proc').iterdir():
     if not p.name.isdigit():continue
     try:
      name=(p/'comm').read_text().strip()
      if name in {'cc1','cc1plus','clang','clang++','gcc','g++','ninja','rustc'} or name.startswith(('clang-', 'clang++-', 'gcc-', 'g++-', 'cc1', 'mcxx-x-mcxx')):compilers.append({'pid':int(p.name),'name':name})
      if name.startswith('clangd'):
       s=(p/'stat').read_text();f=s[s.rfind(')')+2:].split()
       if f[0] not in ('Z','X'):engines.append({'pid':int(p.name),'pgid':int(f[2]),'start_ticks':int(f[19])})
     except (FileNotFoundError,ProcessLookupError):pass
    self.samples.append({'elapsed_ms':(time.monotonic()-self.began)*1000,'compilers':compilers,'engines':engines})
   except Exception as e:self.errors.append(repr(e));self.stop.set()
   self.stop.wait(.2)
 def finish(self):
  self.stop.set();self.thread.join(timeout=2)
  report={'sampling_interval_ms':200,'scope':'Observed compiler/ninja and independent clangd process groups only; does not prove total system idleness or unsampled fast processes','samples':self.samples,'errors':self.errors,'watcher_stopped':not self.thread.is_alive(),'compiler_overlap':any(x['compilers'] for x in self.samples),'multiple_engine_overlap':any(len({e['pgid'] for e in x['engines']})>1 for x in self.samples)}
  (self.root/'workload.json').write_text(json.dumps(report,indent=2)+'\n')
  return bool(self.samples) and not(report['errors'] or report['compiler_overlap'] or report['multiple_engine_overlap']) and report['watcher_stopped']
