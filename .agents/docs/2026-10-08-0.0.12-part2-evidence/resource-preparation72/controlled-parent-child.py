import os,sys,time
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
