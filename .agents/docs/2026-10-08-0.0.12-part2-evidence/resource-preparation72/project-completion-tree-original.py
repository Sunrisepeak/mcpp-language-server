#!/usr/bin/env python3
"""Resource-only replay; all latency values from this invocation are excluded."""
from pathlib import Path
import sys,json,threading,time,os
sys.path.insert(0,'/home/speak/workspace/github/mcppls-clangd/tests/probes')
import project_completion as project
from process_tree_resources import ProcessTree,TreeChanged,stat,identity


class TreeResources:
    def __init__(self):
        self.stop=threading.Event();self.samples=[];self.errors=[];self.races=[];self.thread=None

    def started(self,proc,deadline):
        self.proc=proc;self.deadline=deadline;self.began=time.monotonic();self.tree=ProcessTree(proc.pid)
        def observe():
            while not self.stop.is_set() and time.monotonic()<deadline:
                try:self.samples.append(self.read())
                except TreeChanged as error:
                    if proc.poll() is not None:break
                    self.races.append({'monotonic_ns':time.monotonic_ns(),'error':str(error)})
                except (OSError,ValueError,IndexError) as error:
                    self.errors.append(str(error));break
                self.stop.wait(.2)
        self.thread=threading.Thread(target=observe,daemon=True);self.thread.start()

    def read(self):
        sample=self.tree.read()
        root=next(r for r in sample['members'] if identity(r)==self.tree.root)
        values=dict(line.split(':',1) for line in (Path('/proc')/str(self.proc.pid)/'status').read_text().splitlines() if ':' in line)
        sample.update(cpu_ms=root['cpu_ticks']*sample['cpu_tick_ms'],
                      rss_kib=int(values['VmRSS'].split()[0]),high_water_rss_kib=int(values['VmHWM'].split()[0]),
                      elapsed_ms=(time.monotonic()-self.began)*1000)
        sample['monotonic_ns']=time.monotonic_ns()
        return sample

    def snapshot(self,proc,method):
        if method!='textDocument/completion':return None
        try:return self.read()
        except (OSError,ValueError,IndexError,TreeChanged) as error:
            self.errors.append('request-boundary observation: '+str(error));return {'error':str(error)}

    def finish(self):
        self.stop.set()
        if self.thread:self.thread.join(timeout=2)
        remaining=[]
        if hasattr(self,'tree'):
            for pid,start in self.tree.known:
                try:
                    row=stat(pid)
                    if row['start_ticks']==start and row['state'] not in ('Z','X'):remaining.append(row)
                except (FileNotFoundError,ProcessLookupError):pass
                except OSError as error:self.errors.append('cleanup observation: '+str(error))
        if remaining:self.errors.append('observed owned processes remain after replay')
        return {'interval_ms':200,'cpu_tick_ms':1000/os.sysconf('SC_CLK_TCK'),
                'clock':'time.monotonic_ns; shared with request spans','samples':self.samples,'errors':self.errors,
                'membership_races':self.races,'remaining_owned_processes':remaining,
                'sampler_stopped':self.thread is None or not self.thread.is_alive(),
                'latency_qualification':False,
                'scope':'Resource-only Linux root plus observed descendants, including tracked reparented children. Own plus reaped CPU; stable membership/reap required per observation. PSS proportional mapped pages, private clean+dirty, RSS sum double counts shared pages. Fast unsampled child lifetimes/escapes and physical cache allocation remain outside coverage. All replay latency excluded.'}

if __name__=='__main__':
    assert '--resources' in sys.argv and '--trace' not in sys.argv
    output=Path(sys.argv[sys.argv.index('--output')+1]);assert not output.exists()
    project.Resources=TreeResources
    try:project.main()
    finally:
        if output.exists():
            data=json.loads(output.read_text());data['latency_qualification']=False
            data['resource_only_scope']='Additional process-tree/PSS observer; exclude all elapsed times from performance qualification.'
            output.write_text(json.dumps(data,indent=2)+'\n')
