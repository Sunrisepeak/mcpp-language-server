#!/usr/bin/env python3
import os,sys
from pathlib import Path
root=Path(__file__).parent
server='/home/speak/workspace/github/mcpp-language-server/target/x86_64-linux-gnu/c6a96775022a4395/bin/mcppls'
os.execv("/usr/bin/strace",["strace","-qq","-ttt","-T","-s","120","-e","trace=%file,write","-o",str(root/("main-thread-syscalls-"+str(os.getpid())+".log")),server,*sys.argv[1:]])
