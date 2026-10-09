"""Clean only observed subprocess identities; never target processes by name."""
from pathlib import Path
import os,signal,threading,time


def rows():
    result={}
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():continue
        try:
            text=(path/'stat').read_text();fields=text[text.rfind(')')+2:].split()
            result[int(path.name)]=(int(fields[1]),int(fields[2]),int(fields[19]),fields[0])
        except (FileNotFoundError,ProcessLookupError):continue
    return result


class OwnedProcesses:
    def __init__(self,pid):
        current=rows();row=current[pid]
        self.root=(pid,row[2]);self.known={pid:(row[1],row[2])}
        self.stop=threading.Event();self.errors=[]
        self.thread=threading.Thread(target=self.watch,daemon=True);self.thread.start()

    def capture(self):
        current=rows()
        family={p for p,(_,start) in self.known.items() if p in current and current[p][2]==start}
        while True:
            more={p for p,(parent,_,_,_) in current.items() if parent in family}
            if more<=family:break
            family|=more
        for pid in family:
            _,group,start,_=current[pid];self.known[pid]=(group,start)
        return current

    def watch(self):
        while not self.stop.is_set():
            try:self.capture()
            except (OSError,ValueError,IndexError) as error:self.errors.append(str(error))
            self.stop.wait(.05)

    def cleanup(self):
        self.stop.set();self.thread.join(timeout=2)
        current=self.capture();own_group=os.getpgrp()
        groups={group for pid,(group,start) in self.known.items() if pid in current and current[pid][2]==start and current[pid][1]==group and current[pid][3] not in ('Z','X')}
        if own_group in groups:raise RuntimeError('refuse to signal caller process group')
        for group in groups:
            try:os.killpg(group,signal.SIGTERM)
            except ProcessLookupError:pass
        if groups:time.sleep(.2)
        current=rows()
        remaining={group for pid,(group,start) in self.known.items() if pid in current and current[pid][2]==start and current[pid][1]==group and current[pid][3] not in ('Z','X')}
        if own_group in remaining:raise RuntimeError('refuse to signal caller process group')
        for group in remaining:
            try:os.killpg(group,signal.SIGKILL)
            except ProcessLookupError:pass
        deadline=time.monotonic()+2
        while time.monotonic()<deadline:
            current=rows();live=[pid for pid,(_,start) in self.known.items() if pid in current and current[pid][2]==start and current[pid][3] not in ('Z','X')]
            if not live:break
            time.sleep(.02)
        return {'known':{str(pid):{'pgid':v[0],'start_ticks':v[1]} for pid,v in self.known.items()},'term_groups':sorted(groups),'kill_groups':sorted(remaining),'remaining_observed_live':live,'errors':self.errors,'watcher_stopped':not self.thread.is_alive(),'scope':'Observed identity/descendant cleanup only; periodic discovery does not prove all fast unobserved escapes.'}
