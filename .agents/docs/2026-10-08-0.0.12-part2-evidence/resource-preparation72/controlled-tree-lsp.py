#!/usr/bin/env python3
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
   if line in (b'\r\n',b'\n'):break
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
  sys.stdout.buffer.write(('Content-Length: %d\r\n\r\n'%len(body)).encode()+body);sys.stdout.buffer.flush()
finally:
 os.write(commands_w,b'x');os.waitpid(pid,0)
